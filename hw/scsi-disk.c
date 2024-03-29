/*
 * SCSI Device emulation
 *
 * Copyright (c) 2006 CodeSourcery.
 * Based on code by Fabrice Bellard
 *
 * Written by Paul Brook
 * Modifications:
 *  2009-Dec-12 Artyom Tarasenko : implemented stamdard inquiry for the case
 *                                 when the allocation length of CDB is smaller
 *                                 than 36.
 *  2009-Oct-13 Artyom Tarasenko : implemented the block descriptor in the
 *                                 MODE SENSE response.
 *
 * This code is licensed under the LGPL.
 *
 * Note that this file only handles the SCSI architecture model and device
 * commands.  Emulation of interface/link layer protocols is handled by
 * the host adapter emulator.
 */

//#define DEBUG_SCSI

#ifdef DEBUG_SCSI
#define DPRINTF(fmt, ...) \
do { printf("scsi-disk: " fmt , ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) do {} while(0)
#endif

#include "qemu-common.h"
#include "qemu-error.h"
#include "scsi.h"
#include "scsi-defs.h"
#include "sysemu.h"
#include "blockdev.h"
#include "hw/block-common.h"
#include "dma.h"

#ifdef __linux
#include <scsi/sg.h>
#endif

#define SCSI_DMA_BUF_SIZE    131072
#define SCSI_MAX_INQUIRY_LEN 256

typedef struct SCSIDiskState SCSIDiskState;

typedef struct SCSIDiskReq {
    SCSIRequest req;
    /* Both sector and sector_count are in terms of qemu 512 byte blocks.  */
    uint64_t sector;
    uint32_t sector_count;
    uint32_t buflen;
    bool started;
    struct iovec iov;
    QEMUIOVector qiov;
    BlockAcctCookie acct;
} SCSIDiskReq;

#define SCSI_DISK_F_REMOVABLE   0
#define SCSI_DISK_F_DPOFUA      1

struct SCSIDiskState
{
    SCSIDevice qdev;
    uint32_t features;
    bool media_changed;
    bool media_event;
    bool eject_request;
    uint64_t wwn;
    QEMUBH *bh;
    char *version;
    char *serial;
    bool tray_open;
    bool tray_locked;
};

static int scsi_handle_rw_error(SCSIDiskReq *r, int error);

static void scsi_free_request(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    if (r->iov.iov_base) {
        qemu_vfree(r->iov.iov_base);
    }
}

/* Helper function for command completion with sense.  */
static void scsi_check_condition(SCSIDiskReq *r, SCSISense sense)
{
    DPRINTF("Command complete tag=0x%x sense=%d/%d/%d\n",
            r->req.tag, sense.key, sense.asc, sense.ascq);
    scsi_req_build_sense(&r->req, sense);
    scsi_req_complete(&r->req, CHECK_CONDITION);
}

/* Cancel a pending data transfer.  */
static void scsi_cancel_io(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    DPRINTF("Cancel tag=0x%x\n", req->tag);
    if (r->req.aiocb) {
        bdrv_aio_cancel(r->req.aiocb);

        /* This reference was left in by scsi_*_data.  We take ownership of
         * it the moment scsi_req_cancel is called, independent of whether
         * bdrv_aio_cancel completes the request or not.  */
        scsi_req_unref(&r->req);
    }
    r->req.aiocb = NULL;
}

static uint32_t scsi_init_iovec(SCSIDiskReq *r, size_t size)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    if (!r->iov.iov_base) {
        r->buflen = size;
        r->iov.iov_base = qemu_blockalign(s->qdev.conf.bs, r->buflen);
    }
    r->iov.iov_len = MIN(r->sector_count * 512, r->buflen);
    qemu_iovec_init_external(&r->qiov, &r->iov, 1);
    return r->qiov.size / 512;
}

static void scsi_disk_save_request(QEMUFile *f, SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    qemu_put_be64s(f, &r->sector);
    qemu_put_be32s(f, &r->sector_count);
    qemu_put_be32s(f, &r->buflen);
    if (r->buflen) {
        if (r->req.cmd.mode == SCSI_XFER_TO_DEV) {
            qemu_put_buffer(f, r->iov.iov_base, r->iov.iov_len);
        } else if (!req->retry) {
            uint32_t len = r->iov.iov_len;
            qemu_put_be32s(f, &len);
            qemu_put_buffer(f, r->iov.iov_base, r->iov.iov_len);
        }
    }
}

static void scsi_disk_load_request(QEMUFile *f, SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    qemu_get_be64s(f, &r->sector);
    qemu_get_be32s(f, &r->sector_count);
    qemu_get_be32s(f, &r->buflen);
    if (r->buflen) {
        scsi_init_iovec(r, r->buflen);
        if (r->req.cmd.mode == SCSI_XFER_TO_DEV) {
            qemu_get_buffer(f, r->iov.iov_base, r->iov.iov_len);
        } else if (!r->req.retry) {
            uint32_t len;
            qemu_get_be32s(f, &len);
            r->iov.iov_len = len;
            assert(r->iov.iov_len <= r->buflen);
            qemu_get_buffer(f, r->iov.iov_base, r->iov.iov_len);
        }
    }

    qemu_iovec_init_external(&r->qiov, &r->iov, 1);
}

static void scsi_flush_complete(void * opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    bdrv_acct_done(s->qdev.conf.bs, &r->acct);

    if (ret < 0) {
        if (scsi_handle_rw_error(r, -ret)) {
            goto done;
        }
    }

    scsi_req_complete(&r->req, GOOD);

done:
    if (!r->req.io_canceled) {
        scsi_req_unref(&r->req);
    }
}

static bool scsi_is_cmd_fua(SCSICommand *cmd)
{
    switch (cmd->buf[0]) {
    case READ_10:
    case READ_12:
    case READ_16:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
        return (cmd->buf[1] & 8) != 0;

    case VERIFY_10:
    case VERIFY_12:
    case VERIFY_16:
    case WRITE_VERIFY_10:
    case WRITE_VERIFY_12:
    case WRITE_VERIFY_16:
        return true;

    case READ_6:
    case WRITE_6:
    default:
        return false;
    }
}

static void scsi_write_do_fua(SCSIDiskReq *r)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    if (scsi_is_cmd_fua(&r->req.cmd)) {
        bdrv_acct_start(s->qdev.conf.bs, &r->acct, 0, BDRV_ACCT_FLUSH);
        r->req.aiocb = bdrv_aio_flush(s->qdev.conf.bs, scsi_flush_complete, r);
        return;
    }

    scsi_req_complete(&r->req, GOOD);
    if (!r->req.io_canceled) {
        scsi_req_unref(&r->req);
    }
}

static void scsi_dma_complete(void *opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);

    if (r->req.aiocb != NULL) {
        r->req.aiocb = NULL;
        bdrv_acct_done(s->qdev.conf.bs, &r->acct);
    }

    if (ret < 0) {
        if (scsi_handle_rw_error(r, -ret)) {
            goto done;
        }
    }

    r->sector += r->sector_count;
    r->sector_count = 0;
    if (r->req.cmd.mode == SCSI_XFER_TO_DEV) {
        scsi_write_do_fua(r);
        return;
    } else {
        scsi_req_complete(&r->req, GOOD);
    }

done:
    if (!r->req.io_canceled) {
        scsi_req_unref(&r->req);
    }
}

static void scsi_read_complete(void * opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    int n;

    if (r->req.aiocb != NULL) {
        r->req.aiocb = NULL;
        bdrv_acct_done(s->qdev.conf.bs, &r->acct);
    }

    if (ret < 0) {
        if (scsi_handle_rw_error(r, -ret)) {
            goto done;
        }
    }

    DPRINTF("Data ready tag=0x%x len=%zd\n", r->req.tag, r->qiov.size);

    n = r->qiov.size / 512;
    r->sector += n;
    r->sector_count -= n;
    scsi_req_data(&r->req, r->qiov.size);

done:
    if (!r->req.io_canceled) {
        scsi_req_unref(&r->req);
    }
}

/* Actually issue a read to the block device.  */
static void scsi_do_read(void *opaque, int ret)
{
    SCSIDiskReq *r = opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    uint32_t n;

    if (r->req.aiocb != NULL) {
        r->req.aiocb = NULL;
        bdrv_acct_done(s->qdev.conf.bs, &r->acct);
    }

    if (ret < 0) {
        if (scsi_handle_rw_error(r, -ret)) {
            goto done;
        }
    }

    if (r->req.io_canceled) {
        return;
    }

    /* The request is used as the AIO opaque value, so add a ref.  */
    scsi_req_ref(&r->req);

    if (r->req.sg) {
        dma_acct_start(s->qdev.conf.bs, &r->acct, r->req.sg, BDRV_ACCT_READ);
        r->req.resid -= r->req.sg->size;
        r->req.aiocb = dma_bdrv_read(s->qdev.conf.bs, r->req.sg, r->sector,
                                     scsi_dma_complete, r);
    } else {
        n = scsi_init_iovec(r, SCSI_DMA_BUF_SIZE);
        bdrv_acct_start(s->qdev.conf.bs, &r->acct, n * BDRV_SECTOR_SIZE, BDRV_ACCT_READ);
        r->req.aiocb = bdrv_aio_readv(s->qdev.conf.bs, r->sector, &r->qiov, n,
                                      scsi_read_complete, r);
    }

done:
    if (!r->req.io_canceled) {
        scsi_req_unref(&r->req);
    }
}

/* Read more data from scsi device into buffer.  */
static void scsi_read_data(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    bool first;

    if (r->sector_count == (uint32_t)-1) {
        DPRINTF("Read buf_len=%zd\n", r->iov.iov_len);
        r->sector_count = 0;
        r->started = true;
        scsi_req_data(&r->req, r->iov.iov_len);
        return;
    }
    DPRINTF("Read sector_count=%d\n", r->sector_count);
    if (r->sector_count == 0) {
        /* This also clears the sense buffer for REQUEST SENSE.  */
        scsi_req_complete(&r->req, GOOD);
        return;
    }

    /* No data transfer may already be in progress */
    assert(r->req.aiocb == NULL);

    /* The request is used as the AIO opaque value, so add a ref.  */
    scsi_req_ref(&r->req);
    if (r->req.cmd.mode == SCSI_XFER_TO_DEV) {
        DPRINTF("Data transfer direction invalid\n");
        scsi_read_complete(r, -EINVAL);
        return;
    }

    if (s->tray_open) {
        scsi_read_complete(r, -ENOMEDIUM);
        return;
    }

    first = !r->started;
    r->started = true;
    if (first && scsi_is_cmd_fua(&r->req.cmd)) {
        bdrv_acct_start(s->qdev.conf.bs, &r->acct, 0, BDRV_ACCT_FLUSH);
        r->req.aiocb = bdrv_aio_flush(s->qdev.conf.bs, scsi_do_read, r);
    } else {
        scsi_do_read(r, 0);
    }
}

/*
 * scsi_handle_rw_error has two return values.  0 means that the error
 * must be ignored, 1 means that the error has been processed and the
 * caller should not do anything else for this request.  Note that
 * scsi_handle_rw_error always manages its reference counts, independent
 * of the return value.
 */
static int scsi_handle_rw_error(SCSIDiskReq *r, int error)
{
    int is_read = (r->req.cmd.xfer == SCSI_XFER_FROM_DEV);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    BlockErrorAction action = bdrv_get_on_error(s->qdev.conf.bs, is_read);

    if (action == BLOCK_ERR_IGNORE) {
        bdrv_emit_qmp_error_event(s->qdev.conf.bs, BDRV_ACTION_IGNORE, is_read);
        return 0;
    }

    if ((error == ENOSPC && action == BLOCK_ERR_STOP_ENOSPC)
            || action == BLOCK_ERR_STOP_ANY) {

        bdrv_emit_qmp_error_event(s->qdev.conf.bs, BDRV_ACTION_STOP, is_read);
        vm_stop(RUN_STATE_IO_ERROR);
        bdrv_iostatus_set_err(s->qdev.conf.bs, error);
        scsi_req_retry(&r->req);
    } else {
        switch (error) {
        case ENOMEDIUM:
            scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
            break;
        case ENOMEM:
            scsi_check_condition(r, SENSE_CODE(TARGET_FAILURE));
            break;
        case EINVAL:
            scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
            break;
        default:
            scsi_check_condition(r, SENSE_CODE(IO_ERROR));
            break;
        }
        bdrv_emit_qmp_error_event(s->qdev.conf.bs, BDRV_ACTION_REPORT, is_read);
    }
    return 1;
}

static void scsi_write_complete(void * opaque, int ret)
{
    SCSIDiskReq *r = (SCSIDiskReq *)opaque;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    uint32_t n;

    if (r->req.aiocb != NULL) {
        r->req.aiocb = NULL;
        bdrv_acct_done(s->qdev.conf.bs, &r->acct);
    }

    if (ret < 0) {
        if (scsi_handle_rw_error(r, -ret)) {
            goto done;
        }
    }

    n = r->qiov.size / 512;
    r->sector += n;
    r->sector_count -= n;
    if (r->sector_count == 0) {
        scsi_write_do_fua(r);
        return;
    } else {
        scsi_init_iovec(r, SCSI_DMA_BUF_SIZE);
        DPRINTF("Write complete tag=0x%x more=%d\n", r->req.tag, r->qiov.size);
        scsi_req_data(&r->req, r->qiov.size);
    }

done:
    if (!r->req.io_canceled) {
        scsi_req_unref(&r->req);
    }
}

static void scsi_write_data(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    uint32_t n;

    /* No data transfer may already be in progress */
    assert(r->req.aiocb == NULL);

    /* The request is used as the AIO opaque value, so add a ref.  */
    scsi_req_ref(&r->req);
    if (r->req.cmd.mode != SCSI_XFER_TO_DEV) {
        DPRINTF("Data transfer direction invalid\n");
        scsi_write_complete(r, -EINVAL);
        return;
    }

    if (!r->req.sg && !r->qiov.size) {
        /* Called for the first time.  Ask the driver to send us more data.  */
        r->started = true;
        scsi_write_complete(r, 0);
        return;
    }
    if (s->tray_open) {
        scsi_write_complete(r, -ENOMEDIUM);
        return;
    }

    if (r->req.cmd.buf[0] == VERIFY_10 || r->req.cmd.buf[0] == VERIFY_12 ||
        r->req.cmd.buf[0] == VERIFY_16) {
        if (r->req.sg) {
            scsi_dma_complete(r, 0);
        } else {
            scsi_write_complete(r, 0);
        }
        return;
    }

    if (r->req.sg) {
        dma_acct_start(s->qdev.conf.bs, &r->acct, r->req.sg, BDRV_ACCT_WRITE);
        r->req.resid -= r->req.sg->size;
        r->req.aiocb = dma_bdrv_write(s->qdev.conf.bs, r->req.sg, r->sector,
                                      scsi_dma_complete, r);
    } else {
        n = r->qiov.size / 512;
        bdrv_acct_start(s->qdev.conf.bs, &r->acct, n * BDRV_SECTOR_SIZE, BDRV_ACCT_WRITE);
        r->req.aiocb = bdrv_aio_writev(s->qdev.conf.bs, r->sector, &r->qiov, n,
                                       scsi_write_complete, r);
    }
}

/* Return a pointer to the data buffer.  */
static uint8_t *scsi_get_buf(SCSIRequest *req)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);

    return (uint8_t *)r->iov.iov_base;
}

static int scsi_disk_emulate_inquiry(SCSIRequest *req, uint8_t *outbuf)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, req->dev);
    int buflen = 0;
    int start;

    if (req->cmd.buf[1] & 0x1) {
        /* Vital product data */
        uint8_t page_code = req->cmd.buf[2];

        outbuf[buflen++] = s->qdev.type & 0x1f;
        outbuf[buflen++] = page_code ; // this page
        outbuf[buflen++] = 0x00;
        outbuf[buflen++] = 0x00;
        start = buflen;

        switch (page_code) {
        case 0x00: /* Supported page codes, mandatory */
        {
            DPRINTF("Inquiry EVPD[Supported pages] "
                    "buffer size %zd\n", req->cmd.xfer);
            outbuf[buflen++] = 0x00; // list of supported pages (this page)
            if (s->serial) {
                outbuf[buflen++] = 0x80; // unit serial number
            }
            outbuf[buflen++] = 0x83; // device identification
            if (s->qdev.type == TYPE_DISK) {
                outbuf[buflen++] = 0xb0; // block limits
                outbuf[buflen++] = 0xb2; // thin provisioning
            }
            break;
        }
        case 0x80: /* Device serial number, optional */
        {
            int l;

            if (!s->serial) {
                DPRINTF("Inquiry (EVPD[Serial number] not supported\n");
                return -1;
            }

            l = strlen(s->serial);
            if (l > 20) {
                l = 20;
            }

            DPRINTF("Inquiry EVPD[Serial number] "
                    "buffer size %zd\n", req->cmd.xfer);
            memcpy(outbuf+buflen, s->serial, l);
            buflen += l;
            break;
        }

        case 0x83: /* Device identification page, mandatory */
        {
            const char *str = s->serial ?: bdrv_get_device_name(s->qdev.conf.bs);
            int max_len = s->serial ? 20 : 255 - 8;
            int id_len = strlen(str);

            if (id_len > max_len) {
                id_len = max_len;
            }
            DPRINTF("Inquiry EVPD[Device identification] "
                    "buffer size %zd\n", req->cmd.xfer);

            outbuf[buflen++] = 0x2; // ASCII
            outbuf[buflen++] = 0;   // not officially assigned
            outbuf[buflen++] = 0;   // reserved
            outbuf[buflen++] = id_len; // length of data following
            memcpy(outbuf+buflen, str, id_len);
            buflen += id_len;

            if (s->wwn) {
                outbuf[buflen++] = 0x1; // Binary
                outbuf[buflen++] = 0x3; // NAA
                outbuf[buflen++] = 0;   // reserved
                outbuf[buflen++] = 8;
                stq_be_p(&outbuf[buflen], s->wwn);
                buflen += 8;
            }
            break;
        }
        case 0xb0: /* block limits */
        {
            unsigned int unmap_sectors =
                    s->qdev.conf.discard_granularity / s->qdev.blocksize;
            unsigned int min_io_size =
                    s->qdev.conf.min_io_size / s->qdev.blocksize;
            unsigned int opt_io_size =
                    s->qdev.conf.opt_io_size / s->qdev.blocksize;

            if (s->qdev.type == TYPE_ROM) {
                DPRINTF("Inquiry (EVPD[%02X] not supported for CDROM\n",
                        page_code);
                return -1;
            }
            /* required VPD size with unmap support */
            buflen = 0x40;
            memset(outbuf + 4, 0, buflen - 4);

            /* optimal transfer length granularity */
            outbuf[6] = (min_io_size >> 8) & 0xff;
            outbuf[7] = min_io_size & 0xff;

            /* optimal transfer length */
            outbuf[12] = (opt_io_size >> 24) & 0xff;
            outbuf[13] = (opt_io_size >> 16) & 0xff;
            outbuf[14] = (opt_io_size >> 8) & 0xff;
            outbuf[15] = opt_io_size & 0xff;

            /* optimal unmap granularity */
            outbuf[28] = (unmap_sectors >> 24) & 0xff;
            outbuf[29] = (unmap_sectors >> 16) & 0xff;
            outbuf[30] = (unmap_sectors >> 8) & 0xff;
            outbuf[31] = unmap_sectors & 0xff;
            break;
        }
        case 0xb2: /* thin provisioning */
        {
            buflen = 8;
            outbuf[4] = 0;
            outbuf[5] = 0x60; /* write_same 10/16 supported */
            outbuf[6] = s->qdev.conf.discard_granularity ? 2 : 1;
            outbuf[7] = 0;
            break;
        }
        default:
            return -1;
        }
        /* done with EVPD */
        assert(buflen - start <= 255);
        outbuf[start - 1] = buflen - start;
        return buflen;
    }

    /* Standard INQUIRY data */
    if (req->cmd.buf[2] != 0) {
        return -1;
    }

    /* PAGE CODE == 0 */
    buflen = req->cmd.xfer;
    if (buflen > SCSI_MAX_INQUIRY_LEN) {
        buflen = SCSI_MAX_INQUIRY_LEN;
    }
    memset(outbuf, 0, buflen);

    outbuf[0] = s->qdev.type & 0x1f;
    outbuf[1] = (s->features & (1 << SCSI_DISK_F_REMOVABLE)) ? 0x80 : 0;
    if (s->qdev.type == TYPE_ROM) {
        memcpy(&outbuf[16], "QEMU CD-ROM     ", 16);
    } else {
        memcpy(&outbuf[16], "QEMU HARDDISK   ", 16);
    }
    memcpy(&outbuf[8], "QEMU    ", 8);
    memset(&outbuf[32], 0, 4);
    memcpy(&outbuf[32], s->version, MIN(4, strlen(s->version)));
    /*
     * We claim conformance to SPC-3, which is required for guests
     * to ask for modern features like READ CAPACITY(16) or the
     * block characteristics VPD page by default.  Not all of SPC-3
     * is actually implemented, but we're good enough.
     */
    outbuf[2] = 5;
    outbuf[3] = 2; /* Format 2 */

    if (buflen > 36) {
        outbuf[4] = buflen - 5; /* Additional Length = (Len - 1) - 4 */
    } else {
        /* If the allocation length of CDB is too small,
               the additional length is not adjusted */
        outbuf[4] = 36 - 5;
    }

    /* Sync data transfer and TCQ.  */
    outbuf[7] = 0x10 | (req->bus->info->tcq ? 0x02 : 0);
    return buflen;
}

static inline bool media_is_dvd(SCSIDiskState *s)
{
    uint64_t nb_sectors;
    if (s->qdev.type != TYPE_ROM) {
        return false;
    }
    if (!bdrv_is_inserted(s->qdev.conf.bs)) {
        return false;
    }
    bdrv_get_geometry(s->qdev.conf.bs, &nb_sectors);
    return nb_sectors > CD_MAX_SECTORS;
}

static inline bool media_is_cd(SCSIDiskState *s)
{
    uint64_t nb_sectors;
    if (s->qdev.type != TYPE_ROM) {
        return false;
    }
    if (!bdrv_is_inserted(s->qdev.conf.bs)) {
        return false;
    }
    bdrv_get_geometry(s->qdev.conf.bs, &nb_sectors);
    return nb_sectors <= CD_MAX_SECTORS;
}

static int scsi_read_disc_information(SCSIDiskState *s, SCSIDiskReq *r,
                                      uint8_t *outbuf)
{
    uint8_t type = r->req.cmd.buf[1] & 7;

    if (s->qdev.type != TYPE_ROM) {
        return -1;
    }

    /* Types 1/2 are only defined for Blu-Ray.  */
    if (type != 0) {
        scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
        return -1;
    }

    memset(outbuf, 0, 34);
    outbuf[1] = 32;
    outbuf[2] = 0xe; /* last session complete, disc finalized */
    outbuf[3] = 1;   /* first track on disc */
    outbuf[4] = 1;   /* # of sessions */
    outbuf[5] = 1;   /* first track of last session */
    outbuf[6] = 1;   /* last track of last session */
    outbuf[7] = 0x20; /* unrestricted use */
    outbuf[8] = 0x00; /* CD-ROM or DVD-ROM */
    /* 9-10-11: most significant byte corresponding bytes 4-5-6 */
    /* 12-23: not meaningful for CD-ROM or DVD-ROM */
    /* 24-31: disc bar code */
    /* 32: disc application code */
    /* 33: number of OPC tables */

    return 34;
}

static int scsi_read_dvd_structure(SCSIDiskState *s, SCSIDiskReq *r,
                                   uint8_t *outbuf)
{
    static const int rds_caps_size[5] = {
        [0] = 2048 + 4,
        [1] = 4 + 4,
        [3] = 188 + 4,
        [4] = 2048 + 4,
    };

    uint8_t media = r->req.cmd.buf[1];
    uint8_t layer = r->req.cmd.buf[6];
    uint8_t format = r->req.cmd.buf[7];
    int size = -1;

    if (s->qdev.type != TYPE_ROM) {
        return -1;
    }
    if (media != 0) {
        scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
        return -1;
    }

    if (format != 0xff) {
        if (s->tray_open || !bdrv_is_inserted(s->qdev.conf.bs)) {
            scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
            return -1;
        }
        if (media_is_cd(s)) {
            scsi_check_condition(r, SENSE_CODE(INCOMPATIBLE_FORMAT));
            return -1;
        }
        if (format >= ARRAY_SIZE(rds_caps_size)) {
            return -1;
        }
        size = rds_caps_size[format];
        memset(outbuf, 0, size);
    }

    switch (format) {
    case 0x00: {
        /* Physical format information */
        uint64_t nb_sectors;
        if (layer != 0) {
            goto fail;
        }
        bdrv_get_geometry(s->qdev.conf.bs, &nb_sectors);

        outbuf[4] = 1;   /* DVD-ROM, part version 1 */
        outbuf[5] = 0xf; /* 120mm disc, minimum rate unspecified */
        outbuf[6] = 1;   /* one layer, read-only (per MMC-2 spec) */
        outbuf[7] = 0;   /* default densities */

        stl_be_p(&outbuf[12], (nb_sectors >> 2) - 1); /* end sector */
        stl_be_p(&outbuf[16], (nb_sectors >> 2) - 1); /* l0 end sector */
        break;
    }

    case 0x01: /* DVD copyright information, all zeros */
        break;

    case 0x03: /* BCA information - invalid field for no BCA info */
        return -1;

    case 0x04: /* DVD disc manufacturing information, all zeros */
        break;

    case 0xff: { /* List capabilities */
        int i;
        size = 4;
        for (i = 0; i < ARRAY_SIZE(rds_caps_size); i++) {
            if (!rds_caps_size[i]) {
                continue;
            }
            outbuf[size] = i;
            outbuf[size + 1] = 0x40; /* Not writable, readable */
            stw_be_p(&outbuf[size + 2], rds_caps_size[i]);
            size += 4;
        }
        break;
     }

    default:
        return -1;
    }

    /* Size of buffer, not including 2 byte size field */
    stw_be_p(outbuf, size - 2);
    return size;

fail:
    return -1;
}

static int scsi_event_status_media(SCSIDiskState *s, uint8_t *outbuf)
{
    uint8_t event_code, media_status;

    media_status = 0;
    if (s->tray_open) {
        media_status = MS_TRAY_OPEN;
    } else if (bdrv_is_inserted(s->qdev.conf.bs)) {
        media_status = MS_MEDIA_PRESENT;
    }

    /* Event notification descriptor */
    event_code = MEC_NO_CHANGE;
    if (media_status != MS_TRAY_OPEN) {
        if (s->media_event) {
            event_code = MEC_NEW_MEDIA;
            s->media_event = false;
        } else if (s->eject_request) {
            event_code = MEC_EJECT_REQUESTED;
            s->eject_request = false;
        }
    }

    outbuf[0] = event_code;
    outbuf[1] = media_status;

    /* These fields are reserved, just clear them. */
    outbuf[2] = 0;
    outbuf[3] = 0;
    return 4;
}

static int scsi_get_event_status_notification(SCSIDiskState *s, SCSIDiskReq *r,
                                              uint8_t *outbuf)
{
    int size;
    uint8_t *buf = r->req.cmd.buf;
    uint8_t notification_class_request = buf[4];
    if (s->qdev.type != TYPE_ROM) {
        return -1;
    }
    if ((buf[1] & 1) == 0) {
        /* asynchronous */
        return -1;
    }

    size = 4;
    outbuf[0] = outbuf[1] = 0;
    outbuf[3] = 1 << GESN_MEDIA; /* supported events */
    if (notification_class_request & (1 << GESN_MEDIA)) {
        outbuf[2] = GESN_MEDIA;
        size += scsi_event_status_media(s, &outbuf[size]);
    } else {
        outbuf[2] = 0x80;
    }
    stw_be_p(outbuf, size - 4);
    return size;
}

static int scsi_get_configuration(SCSIDiskState *s, uint8_t *outbuf)
{
    int current;

    if (s->qdev.type != TYPE_ROM) {
        return -1;
    }
    current = media_is_dvd(s) ? MMC_PROFILE_DVD_ROM : MMC_PROFILE_CD_ROM;
    memset(outbuf, 0, 40);
    stl_be_p(&outbuf[0], 36); /* Bytes after the data length field */
    stw_be_p(&outbuf[6], current);
    /* outbuf[8] - outbuf[19]: Feature 0 - Profile list */
    outbuf[10] = 0x03; /* persistent, current */
    outbuf[11] = 8; /* two profiles */
    stw_be_p(&outbuf[12], MMC_PROFILE_DVD_ROM);
    outbuf[14] = (current == MMC_PROFILE_DVD_ROM);
    stw_be_p(&outbuf[16], MMC_PROFILE_CD_ROM);
    outbuf[18] = (current == MMC_PROFILE_CD_ROM);
    /* outbuf[20] - outbuf[31]: Feature 1 - Core feature */
    stw_be_p(&outbuf[20], 1);
    outbuf[22] = 0x08 | 0x03; /* version 2, persistent, current */
    outbuf[23] = 8;
    stl_be_p(&outbuf[24], 1); /* SCSI */
    outbuf[28] = 1; /* DBE = 1, mandatory */
    /* outbuf[32] - outbuf[39]: Feature 3 - Removable media feature */
    stw_be_p(&outbuf[32], 3);
    outbuf[34] = 0x08 | 0x03; /* version 2, persistent, current */
    outbuf[35] = 4;
    outbuf[36] = 0x39; /* tray, load=1, eject=1, unlocked at powerup, lock=1 */
    /* TODO: Random readable, CD read, DVD read, drive serial number,
       power management */
    return 40;
}

static int scsi_emulate_mechanism_status(SCSIDiskState *s, uint8_t *outbuf)
{
    if (s->qdev.type != TYPE_ROM) {
        return -1;
    }
    memset(outbuf, 0, 8);
    outbuf[5] = 1; /* CD-ROM */
    return 8;
}

static int mode_sense_page(SCSIDiskState *s, int page, uint8_t **p_outbuf,
                           int page_control)
{
    static const int mode_sense_valid[0x3f] = {
        [MODE_PAGE_HD_GEOMETRY]            = (1 << TYPE_DISK),
        [MODE_PAGE_FLEXIBLE_DISK_GEOMETRY] = (1 << TYPE_DISK),
        [MODE_PAGE_CACHING]                = (1 << TYPE_DISK) | (1 << TYPE_ROM),
        [MODE_PAGE_R_W_ERROR]              = (1 << TYPE_DISK) | (1 << TYPE_ROM),
        [MODE_PAGE_AUDIO_CTL]              = (1 << TYPE_ROM),
        [MODE_PAGE_CAPABILITIES]           = (1 << TYPE_ROM),
    };
    uint8_t *p = *p_outbuf;

    if ((mode_sense_valid[page] & (1 << s->qdev.type)) == 0) {
        return -1;
    }

    p[0] = page;

    /*
     * If Changeable Values are requested, a mask denoting those mode parameters
     * that are changeable shall be returned. As we currently don't support
     * parameter changes via MODE_SELECT all bits are returned set to zero.
     * The buffer was already menset to zero by the caller of this function.
     */
    switch (page) {
    case MODE_PAGE_HD_GEOMETRY:
        p[1] = 0x16;
        if (page_control == 1) { /* Changeable Values */
            break;
        }
        /* if a geometry hint is available, use it */
        p[2] = (s->qdev.conf.cyls >> 16) & 0xff;
        p[3] = (s->qdev.conf.cyls >> 8) & 0xff;
        p[4] = s->qdev.conf.cyls & 0xff;
        p[5] = s->qdev.conf.heads & 0xff;
        /* Write precomp start cylinder, disabled */
        p[6] = (s->qdev.conf.cyls >> 16) & 0xff;
        p[7] = (s->qdev.conf.cyls >> 8) & 0xff;
        p[8] = s->qdev.conf.cyls & 0xff;
        /* Reduced current start cylinder, disabled */
        p[9] = (s->qdev.conf.cyls >> 16) & 0xff;
        p[10] = (s->qdev.conf.cyls >> 8) & 0xff;
        p[11] = s->qdev.conf.cyls & 0xff;
        /* Device step rate [ns], 200ns */
        p[12] = 0;
        p[13] = 200;
        /* Landing zone cylinder */
        p[14] = 0xff;
        p[15] =  0xff;
        p[16] = 0xff;
        /* Medium rotation rate [rpm], 5400 rpm */
        p[20] = (5400 >> 8) & 0xff;
        p[21] = 5400 & 0xff;
        break;

    case MODE_PAGE_FLEXIBLE_DISK_GEOMETRY:
        p[1] = 0x1e;
        if (page_control == 1) { /* Changeable Values */
            break;
        }
        /* Transfer rate [kbit/s], 5Mbit/s */
        p[2] = 5000 >> 8;
        p[3] = 5000 & 0xff;
        /* if a geometry hint is available, use it */
        p[4] = s->qdev.conf.heads & 0xff;
        p[5] = s->qdev.conf.secs & 0xff;
        p[6] = s->qdev.blocksize >> 8;
        p[8] = (s->qdev.conf.cyls >> 8) & 0xff;
        p[9] = s->qdev.conf.cyls & 0xff;
        /* Write precomp start cylinder, disabled */
        p[10] = (s->qdev.conf.cyls >> 8) & 0xff;
        p[11] = s->qdev.conf.cyls & 0xff;
        /* Reduced current start cylinder, disabled */
        p[12] = (s->qdev.conf.cyls >> 8) & 0xff;
        p[13] = s->qdev.conf.cyls & 0xff;
        /* Device step rate [100us], 100us */
        p[14] = 0;
        p[15] = 1;
        /* Device step pulse width [us], 1us */
        p[16] = 1;
        /* Device head settle delay [100us], 100us */
        p[17] = 0;
        p[18] = 1;
        /* Motor on delay [0.1s], 0.1s */
        p[19] = 1;
        /* Motor off delay [0.1s], 0.1s */
        p[20] = 1;
        /* Medium rotation rate [rpm], 5400 rpm */
        p[28] = (5400 >> 8) & 0xff;
        p[29] = 5400 & 0xff;
        break;

    case MODE_PAGE_CACHING:
        p[0] = 8;
        p[1] = 0x12;
        if (page_control == 1) { /* Changeable Values */
            break;
        }
        if (bdrv_enable_write_cache(s->qdev.conf.bs)) {
            p[2] = 4; /* WCE */
        }
        break;

    case MODE_PAGE_R_W_ERROR:
        p[1] = 10;
        p[2] = 0x80; /* Automatic Write Reallocation Enabled */
        if (s->qdev.type == TYPE_ROM) {
            p[3] = 0x20; /* Read Retry Count */
        }
        break;

    case MODE_PAGE_AUDIO_CTL:
        p[1] = 14;
        break;

    case MODE_PAGE_CAPABILITIES:
        p[1] = 0x14;
        if (page_control == 1) { /* Changeable Values */
            break;
        }

        p[2] = 0x3b; /* CD-R & CD-RW read */
        p[3] = 0; /* Writing not supported */
        p[4] = 0x7f; /* Audio, composite, digital out,
                        mode 2 form 1&2, multi session */
        p[5] = 0xff; /* CD DA, DA accurate, RW supported,
                        RW corrected, C2 errors, ISRC,
                        UPC, Bar code */
        p[6] = 0x2d | (s->tray_locked ? 2 : 0);
        /* Locking supported, jumper present, eject, tray */
        p[7] = 0; /* no volume & mute control, no
                     changer */
        p[8] = (50 * 176) >> 8; /* 50x read speed */
        p[9] = (50 * 176) & 0xff;
        p[10] = 2 >> 8; /* Two volume levels */
        p[11] = 2 & 0xff;
        p[12] = 2048 >> 8; /* 2M buffer */
        p[13] = 2048 & 0xff;
        p[14] = (16 * 176) >> 8; /* 16x read speed current */
        p[15] = (16 * 176) & 0xff;
        p[18] = (16 * 176) >> 8; /* 16x write speed */
        p[19] = (16 * 176) & 0xff;
        p[20] = (16 * 176) >> 8; /* 16x write speed current */
        p[21] = (16 * 176) & 0xff;
        break;

    default:
        return -1;
    }

    *p_outbuf += p[1] + 2;
    return p[1] + 2;
}

static int scsi_disk_emulate_mode_sense(SCSIDiskReq *r, uint8_t *outbuf)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, r->req.dev);
    uint64_t nb_sectors;
    bool dbd;
    int page, buflen, ret, page_control;
    uint8_t *p;
    uint8_t dev_specific_param;

    dbd = (r->req.cmd.buf[1] & 0x8) != 0;
    page = r->req.cmd.buf[2] & 0x3f;
    page_control = (r->req.cmd.buf[2] & 0xc0) >> 6;
    DPRINTF("Mode Sense(%d) (page %d, xfer %zd, page_control %d)\n",
        (r->req.cmd.buf[0] == MODE_SENSE) ? 6 : 10, page, r->req.cmd.xfer, page_control);
    memset(outbuf, 0, r->req.cmd.xfer);
    p = outbuf;

    if (s->qdev.type == TYPE_DISK) {
        dev_specific_param = s->features & (1 << SCSI_DISK_F_DPOFUA) ? 0x10 : 0;
        if (bdrv_is_read_only(s->qdev.conf.bs)) {
            dev_specific_param |= 0x80; /* Readonly.  */
        }
    } else {
        /* MMC prescribes that CD/DVD drives have no block descriptors,
         * and defines no device-specific parameter.  */
        dev_specific_param = 0x00;
        dbd = true;
    }

    if (r->req.cmd.buf[0] == MODE_SENSE) {
        p[1] = 0; /* Default media type.  */
        p[2] = dev_specific_param;
        p[3] = 0; /* Block descriptor length.  */
        p += 4;
    } else { /* MODE_SENSE_10 */
        p[2] = 0; /* Default media type.  */
        p[3] = dev_specific_param;
        p[6] = p[7] = 0; /* Block descriptor length.  */
        p += 8;
    }

    bdrv_get_geometry(s->qdev.conf.bs, &nb_sectors);
    if (!dbd && nb_sectors) {
        if (r->req.cmd.buf[0] == MODE_SENSE) {
            outbuf[3] = 8; /* Block descriptor length  */
        } else { /* MODE_SENSE_10 */
            outbuf[7] = 8; /* Block descriptor length  */
        }
        nb_sectors /= (s->qdev.blocksize / 512);
        if (nb_sectors > 0xffffff) {
            nb_sectors = 0;
        }
        p[0] = 0; /* media density code */
        p[1] = (nb_sectors >> 16) & 0xff;
        p[2] = (nb_sectors >> 8) & 0xff;
        p[3] = nb_sectors & 0xff;
        p[4] = 0; /* reserved */
        p[5] = 0; /* bytes 5-7 are the sector size in bytes */
        p[6] = s->qdev.blocksize >> 8;
        p[7] = 0;
        p += 8;
    }

    if (page_control == 3) {
        /* Saved Values */
        scsi_check_condition(r, SENSE_CODE(SAVING_PARAMS_NOT_SUPPORTED));
        return -1;
    }

    if (page == 0x3f) {
        for (page = 0; page <= 0x3e; page++) {
            mode_sense_page(s, page, &p, page_control);
        }
    } else {
        ret = mode_sense_page(s, page, &p, page_control);
        if (ret == -1) {
            return -1;
        }
    }

    buflen = p - outbuf;
    /*
     * The mode data length field specifies the length in bytes of the
     * following data that is available to be transferred. The mode data
     * length does not include itself.
     */
    if (r->req.cmd.buf[0] == MODE_SENSE) {
        outbuf[0] = buflen - 1;
    } else { /* MODE_SENSE_10 */
        outbuf[0] = ((buflen - 2) >> 8) & 0xff;
        outbuf[1] = (buflen - 2) & 0xff;
    }
    return buflen;
}

static int scsi_disk_emulate_read_toc(SCSIRequest *req, uint8_t *outbuf)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, req->dev);
    int start_track, format, msf, toclen;
    uint64_t nb_sectors;

    msf = req->cmd.buf[1] & 2;
    format = req->cmd.buf[2] & 0xf;
    start_track = req->cmd.buf[6];
    bdrv_get_geometry(s->qdev.conf.bs, &nb_sectors);
    DPRINTF("Read TOC (track %d format %d msf %d)\n", start_track, format, msf >> 1);
    nb_sectors /= s->qdev.blocksize / 512;
    switch (format) {
    case 0:
        toclen = cdrom_read_toc(nb_sectors, outbuf, msf, start_track);
        break;
    case 1:
        /* multi session : only a single session defined */
        toclen = 12;
        memset(outbuf, 0, 12);
        outbuf[1] = 0x0a;
        outbuf[2] = 0x01;
        outbuf[3] = 0x01;
        break;
    case 2:
        toclen = cdrom_read_toc_raw(nb_sectors, outbuf, msf, start_track);
        break;
    default:
        return -1;
    }
    return toclen;
}

static int scsi_disk_emulate_start_stop(SCSIDiskReq *r)
{
    SCSIRequest *req = &r->req;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, req->dev);
    bool start = req->cmd.buf[4] & 1;
    bool loej = req->cmd.buf[4] & 2; /* load on start, eject on !start */

    if (s->qdev.type == TYPE_ROM && loej) {
        if (!start && !s->tray_open && s->tray_locked) {
            scsi_check_condition(r,
                                 bdrv_is_inserted(s->qdev.conf.bs)
                                 ? SENSE_CODE(ILLEGAL_REQ_REMOVAL_PREVENTED)
                                 : SENSE_CODE(NOT_READY_REMOVAL_PREVENTED));
            return -1;
        }

        if (s->tray_open != !start) {
            bdrv_eject(s->qdev.conf.bs, !start);
            s->tray_open = !start;
        }
    }
    return 0;
}

static int scsi_disk_emulate_command(SCSIDiskReq *r)
{
    SCSIRequest *req = &r->req;
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, req->dev);
    uint64_t nb_sectors;
    uint8_t *outbuf;
    int buflen = 0;

    if (!r->iov.iov_base) {
        /*
         * FIXME: we shouldn't return anything bigger than 4k, but the code
         * requires the buffer to be as big as req->cmd.xfer in several
         * places.  So, do not allow CDBs with a very large ALLOCATION
         * LENGTH.  The real fix would be to modify scsi_read_data and
         * dma_buf_read, so that they return data beyond the buflen
         * as all zeros.
         */
        if (req->cmd.xfer > 65536) {
            goto illegal_request;
        }
        r->buflen = MAX(4096, req->cmd.xfer);
        r->iov.iov_base = qemu_blockalign(s->qdev.conf.bs, r->buflen);
    }

    outbuf = r->iov.iov_base;
    switch (req->cmd.buf[0]) {
    case TEST_UNIT_READY:
        assert(!s->tray_open && bdrv_is_inserted(s->qdev.conf.bs));
        break;
    case INQUIRY:
        buflen = scsi_disk_emulate_inquiry(req, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case MODE_SENSE:
    case MODE_SENSE_10:
        buflen = scsi_disk_emulate_mode_sense(r, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case READ_TOC:
        buflen = scsi_disk_emulate_read_toc(req, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case RESERVE:
        if (req->cmd.buf[1] & 1) {
            goto illegal_request;
        }
        break;
    case RESERVE_10:
        if (req->cmd.buf[1] & 3) {
            goto illegal_request;
        }
        break;
    case RELEASE:
        if (req->cmd.buf[1] & 1) {
            goto illegal_request;
        }
        break;
    case RELEASE_10:
        if (req->cmd.buf[1] & 3) {
            goto illegal_request;
        }
        break;
    case START_STOP:
        if (scsi_disk_emulate_start_stop(r) < 0) {
            return -1;
        }
        break;
    case ALLOW_MEDIUM_REMOVAL:
        s->tray_locked = req->cmd.buf[4] & 1;
        bdrv_lock_medium(s->qdev.conf.bs, req->cmd.buf[4] & 1);
        break;
    case READ_CAPACITY_10:
        /* The normal LEN field for this command is zero.  */
        memset(outbuf, 0, 8);
        bdrv_get_geometry(s->qdev.conf.bs, &nb_sectors);
        if (!nb_sectors) {
            scsi_check_condition(r, SENSE_CODE(LUN_NOT_READY));
            return -1;
        }
        if ((req->cmd.buf[8] & 1) == 0 && req->cmd.lba) {
            goto illegal_request;
        }
        nb_sectors /= s->qdev.blocksize / 512;
        /* Returned value is the address of the last sector.  */
        nb_sectors--;
        /* Remember the new size for read/write sanity checking. */
        s->qdev.max_lba = nb_sectors;
        /* Clip to 2TB, instead of returning capacity modulo 2TB. */
        if (nb_sectors > UINT32_MAX) {
            nb_sectors = UINT32_MAX;
        }
        outbuf[0] = (nb_sectors >> 24) & 0xff;
        outbuf[1] = (nb_sectors >> 16) & 0xff;
        outbuf[2] = (nb_sectors >> 8) & 0xff;
        outbuf[3] = nb_sectors & 0xff;
        outbuf[4] = 0;
        outbuf[5] = 0;
        outbuf[6] = s->qdev.blocksize >> 8;
        outbuf[7] = 0;
        buflen = 8;
        break;
    case REQUEST_SENSE:
        /* Just return "NO SENSE".  */
        buflen = scsi_build_sense(NULL, 0, outbuf, r->buflen,
                                  (req->cmd.buf[1] & 1) == 0);
        break;
    case MECHANISM_STATUS:
        buflen = scsi_emulate_mechanism_status(s, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case GET_CONFIGURATION:
        buflen = scsi_get_configuration(s, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case GET_EVENT_STATUS_NOTIFICATION:
        buflen = scsi_get_event_status_notification(s, r, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case READ_DISC_INFORMATION:
        buflen = scsi_read_disc_information(s, r, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case READ_DVD_STRUCTURE:
        buflen = scsi_read_dvd_structure(s, r, outbuf);
        if (buflen < 0) {
            goto illegal_request;
        }
        break;
    case SERVICE_ACTION_IN_16:
        /* Service Action In subcommands. */
        if ((req->cmd.buf[1] & 31) == SAI_READ_CAPACITY_16) {
            DPRINTF("SAI READ CAPACITY(16)\n");
            memset(outbuf, 0, req->cmd.xfer);
            bdrv_get_geometry(s->qdev.conf.bs, &nb_sectors);
            if (!nb_sectors) {
                scsi_check_condition(r, SENSE_CODE(LUN_NOT_READY));
                return -1;
            }
            if ((req->cmd.buf[14] & 1) == 0 && req->cmd.lba) {
                goto illegal_request;
            }
            nb_sectors /= s->qdev.blocksize / 512;
            /* Returned value is the address of the last sector.  */
            nb_sectors--;
            /* Remember the new size for read/write sanity checking. */
            s->qdev.max_lba = nb_sectors;
            outbuf[0] = (nb_sectors >> 56) & 0xff;
            outbuf[1] = (nb_sectors >> 48) & 0xff;
            outbuf[2] = (nb_sectors >> 40) & 0xff;
            outbuf[3] = (nb_sectors >> 32) & 0xff;
            outbuf[4] = (nb_sectors >> 24) & 0xff;
            outbuf[5] = (nb_sectors >> 16) & 0xff;
            outbuf[6] = (nb_sectors >> 8) & 0xff;
            outbuf[7] = nb_sectors & 0xff;
            outbuf[8] = 0;
            outbuf[9] = 0;
            outbuf[10] = s->qdev.blocksize >> 8;
            outbuf[11] = 0;
            outbuf[12] = 0;
            outbuf[13] = get_physical_block_exp(&s->qdev.conf);

            /* set TPE bit if the format supports discard */
            if (s->qdev.conf.discard_granularity) {
                outbuf[14] = 0x80;
            }

            /* Protection, exponent and lowest lba field left blank. */
            buflen = req->cmd.xfer;
            break;
        }
        DPRINTF("Unsupported Service Action In\n");
        goto illegal_request;
    default:
        scsi_check_condition(r, SENSE_CODE(INVALID_OPCODE));
        return -1;
    }
    buflen = MIN(buflen, req->cmd.xfer);
    return buflen;

illegal_request:
    if (r->req.status == -1) {
        scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
    }
    return -1;
}

/* Execute a scsi command.  Returns the length of the data expected by the
   command.  This will be Positive for data transfers from the device
   (eg. disk reads), negative for transfers to the device (eg. disk writes),
   and zero if the command does not transfer any data.  */

static int32_t scsi_send_command(SCSIRequest *req, uint8_t *buf)
{
    SCSIDiskReq *r = DO_UPCAST(SCSIDiskReq, req, req);
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, req->dev);
    int32_t len;
    uint8_t command;
    int rc;

    command = buf[0];
    DPRINTF("Command: lun=%d tag=0x%x data=0x%02x", req->lun, req->tag, buf[0]);

#ifdef DEBUG_SCSI
    {
        int i;
        for (i = 1; i < r->req.cmd.len; i++) {
            printf(" 0x%02x", buf[i]);
        }
        printf("\n");
    }
#endif

    switch (command) {
    case INQUIRY:
    case MODE_SENSE:
    case MODE_SENSE_10:
    case RESERVE:
    case RESERVE_10:
    case RELEASE:
    case RELEASE_10:
    case START_STOP:
    case ALLOW_MEDIUM_REMOVAL:
    case GET_CONFIGURATION:
    case GET_EVENT_STATUS_NOTIFICATION:
    case MECHANISM_STATUS:
    case REQUEST_SENSE:
        break;

    default:
        if (s->tray_open || !bdrv_is_inserted(s->qdev.conf.bs)) {
            scsi_check_condition(r, SENSE_CODE(NO_MEDIUM));
            return 0;
        }
        break;
    }

    switch (command) {
    case TEST_UNIT_READY:
    case INQUIRY:
    case MODE_SENSE:
    case MODE_SENSE_10:
    case RESERVE:
    case RESERVE_10:
    case RELEASE:
    case RELEASE_10:
    case START_STOP:
    case ALLOW_MEDIUM_REMOVAL:
    case READ_CAPACITY_10:
    case READ_TOC:
    case READ_DISC_INFORMATION:
    case READ_DVD_STRUCTURE:
    case GET_CONFIGURATION:
    case GET_EVENT_STATUS_NOTIFICATION:
    case MECHANISM_STATUS:
    case SERVICE_ACTION_IN_16:
    case REQUEST_SENSE:
        rc = scsi_disk_emulate_command(r);
        if (rc < 0) {
            return 0;
        }

        r->iov.iov_len = rc;
        break;
    case SYNCHRONIZE_CACHE:
        /* The request is used as the AIO opaque value, so add a ref.  */
        scsi_req_ref(&r->req);
        bdrv_acct_start(s->qdev.conf.bs, &r->acct, 0, BDRV_ACCT_FLUSH);
        r->req.aiocb = bdrv_aio_flush(s->qdev.conf.bs, scsi_flush_complete, r);
        return 0;
    case READ_6:
    case READ_10:
    case READ_12:
    case READ_16:
        len = r->req.cmd.xfer / s->qdev.blocksize;
        DPRINTF("Read (sector %" PRId64 ", count %d)\n", r->req.cmd.lba, len);
        if (r->req.cmd.lba > s->qdev.max_lba) {
            goto illegal_lba;
        }
        r->sector = r->req.cmd.lba * (s->qdev.blocksize / 512);
        r->sector_count = len * (s->qdev.blocksize / 512);
        break;
    case VERIFY_10:
    case VERIFY_12:
    case VERIFY_16:
    case WRITE_6:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
    case WRITE_VERIFY_10:
    case WRITE_VERIFY_12:
    case WRITE_VERIFY_16:
        len = r->req.cmd.xfer / s->qdev.blocksize;
        DPRINTF("Write %s(sector %" PRId64 ", count %d)\n",
                (command & 0xe) == 0xe ? "And Verify " : "",
                r->req.cmd.lba, len);
        if (r->req.cmd.lba > s->qdev.max_lba) {
            goto illegal_lba;
        }
        r->sector = r->req.cmd.lba * (s->qdev.blocksize / 512);
        r->sector_count = len * (s->qdev.blocksize / 512);
        break;
    case MODE_SELECT:
        DPRINTF("Mode Select(6) (len %lu)\n", (long)r->req.cmd.xfer);
        /* We don't support mode parameter changes.
           Allow the mode parameter header + block descriptors only. */
        if (r->req.cmd.xfer > 12) {
            goto fail;
        }
        break;
    case MODE_SELECT_10:
        DPRINTF("Mode Select(10) (len %lu)\n", (long)r->req.cmd.xfer);
        /* We don't support mode parameter changes.
           Allow the mode parameter header + block descriptors only. */
        if (r->req.cmd.xfer > 16) {
            goto fail;
        }
        break;
    case SEEK_10:
        DPRINTF("Seek(10) (sector %" PRId64 ")\n", r->req.cmd.lba);
        if (r->req.cmd.lba > s->qdev.max_lba) {
            goto illegal_lba;
        }
        break;
    case WRITE_SAME_10:
        len = lduw_be_p(&buf[7]);
        goto write_same;
    case WRITE_SAME_16:
        len = ldl_be_p(&buf[10]) & 0xffffffffULL;
    write_same:

        DPRINTF("WRITE SAME() (sector %" PRId64 ", count %d)\n",
                r->req.cmd.lba, len);

        if (r->req.cmd.lba > s->qdev.max_lba) {
            goto illegal_lba;
        }

        /*
         * We only support WRITE SAME with the unmap bit set for now.
         */
        if (!(buf[1] & 0x8)) {
            goto fail;
        }

        rc = bdrv_discard(s->qdev.conf.bs,
                          r->req.cmd.lba * (s->qdev.blocksize / 512),
                          len * (s->qdev.blocksize / 512));
        if (rc < 0) {
            /* XXX: better error code ?*/
            goto fail;
        }

        break;
    default:
        DPRINTF("Unknown SCSI command (%2.2x)\n", buf[0]);
        scsi_check_condition(r, SENSE_CODE(INVALID_OPCODE));
        return 0;
    fail:
        scsi_check_condition(r, SENSE_CODE(INVALID_FIELD));
        return 0;
    illegal_lba:
        scsi_check_condition(r, SENSE_CODE(LBA_OUT_OF_RANGE));
        return 0;
    }
    if (r->sector_count == 0 && r->iov.iov_len == 0) {
        scsi_req_complete(&r->req, GOOD);
    }
    len = r->sector_count * 512 + r->iov.iov_len;
    if (r->req.cmd.mode == SCSI_XFER_TO_DEV) {
        return -len;
    } else {
        if (!r->sector_count) {
            r->sector_count = -1;
        }
        return len;
    }
}

static void scsi_disk_reset(DeviceState *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev.qdev, dev);
    uint64_t nb_sectors;

    scsi_device_purge_requests(&s->qdev, SENSE_CODE(RESET));

    bdrv_get_geometry(s->qdev.conf.bs, &nb_sectors);
    nb_sectors /= s->qdev.blocksize / 512;
    if (nb_sectors) {
        nb_sectors--;
    }
    s->qdev.max_lba = nb_sectors;
}

static void scsi_destroy(SCSIDevice *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);

    scsi_device_purge_requests(&s->qdev, SENSE_CODE(NO_SENSE));
    blockdev_mark_auto_del(s->qdev.conf.bs);
}

static void scsi_cd_change_media_cb(void *opaque, bool load)
{
    SCSIDiskState *s = opaque;

    /*
     * When a CD gets changed, we have to report an ejected state and
     * then a loaded state to guests so that they detect tray
     * open/close and media change events.  Guests that do not use
     * GET_EVENT_STATUS_NOTIFICATION to detect such tray open/close
     * states rely on this behavior.
     *
     * media_changed governs the state machine used for unit attention
     * report.  media_event is used by GET EVENT STATUS NOTIFICATION.
     */
    s->media_changed = load;
    s->tray_open = !load;
    s->qdev.unit_attention = SENSE_CODE(UNIT_ATTENTION_NO_MEDIUM);
    s->media_event = true;
    s->eject_request = false;
}

static void scsi_cd_eject_request_cb(void *opaque, bool force)
{
    SCSIDiskState *s = opaque;

    s->eject_request = true;
    if (force) {
        s->tray_locked = false;
    }
}

static bool scsi_cd_is_tray_open(void *opaque)
{
    return ((SCSIDiskState *)opaque)->tray_open;
}

static bool scsi_cd_is_medium_locked(void *opaque)
{
    return ((SCSIDiskState *)opaque)->tray_locked;
}

static const BlockDevOps scsi_cd_block_ops = {
    .change_media_cb = scsi_cd_change_media_cb,
    .eject_request_cb = scsi_cd_eject_request_cb,
    .is_tray_open = scsi_cd_is_tray_open,
    .is_medium_locked = scsi_cd_is_medium_locked,
};

static void scsi_disk_unit_attention_reported(SCSIDevice *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);
    if (s->media_changed) {
        s->media_changed = false;
        s->qdev.unit_attention = SENSE_CODE(MEDIUM_CHANGED);
    }
}

static int scsi_initfn(SCSIDevice *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);

    if (!s->qdev.conf.bs) {
        error_report("drive property not set");
        return -1;
    }

    if (!(s->features & (1 << SCSI_DISK_F_REMOVABLE)) &&
        !bdrv_is_inserted(s->qdev.conf.bs)) {
        error_report("Device needs media, but drive is empty");
        return -1;
    }

    blkconf_serial(&s->qdev.conf, &s->serial);
    if (blkconf_geometry(&dev->conf, NULL, 65535, 255, 255) < 0) {
        return -1;
    }

    if (!s->version) {
        s->version = g_strdup(qemu_get_version());
    }

    if (bdrv_is_sg(s->qdev.conf.bs)) {
        error_report("unwanted /dev/sg*");
        return -1;
    }

    if (s->features & (1 << SCSI_DISK_F_REMOVABLE)) {
        bdrv_set_dev_ops(s->qdev.conf.bs, &scsi_cd_block_ops, s);
    }
    bdrv_set_buffer_alignment(s->qdev.conf.bs, s->qdev.blocksize);

    bdrv_iostatus_enable(s->qdev.conf.bs);
    add_boot_device_path(s->qdev.conf.bootindex, &dev->qdev, NULL);
    return 0;
}

static int scsi_hd_initfn(SCSIDevice *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);
    s->qdev.blocksize = s->qdev.conf.logical_block_size;
    s->qdev.type = TYPE_DISK;
    return scsi_initfn(&s->qdev);
}

static int scsi_cd_initfn(SCSIDevice *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);
    s->qdev.blocksize = 2048;
    s->qdev.type = TYPE_ROM;
    s->features |= 1 << SCSI_DISK_F_REMOVABLE;
    return scsi_initfn(&s->qdev);
}

static int scsi_disk_initfn(SCSIDevice *dev)
{
    DriveInfo *dinfo;

    if (!dev->conf.bs) {
        return scsi_initfn(dev);  /* ... and die there */
    }

    dinfo = drive_get_by_blockdev(dev->conf.bs);
    if (dinfo->media_cd) {
        return scsi_cd_initfn(dev);
    } else {
        return scsi_hd_initfn(dev);
    }
}

static const SCSIReqOps scsi_disk_reqops = {
    .size         = sizeof(SCSIDiskReq),
    .free_req     = scsi_free_request,
    .send_command = scsi_send_command,
    .read_data    = scsi_read_data,
    .write_data   = scsi_write_data,
    .cancel_io    = scsi_cancel_io,
    .get_buf      = scsi_get_buf,
    .load_request = scsi_disk_load_request,
    .save_request = scsi_disk_save_request,
};

static SCSIRequest *scsi_new_request(SCSIDevice *d, uint32_t tag, uint32_t lun,
                                     uint8_t *buf, void *hba_private)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, d);
    SCSIRequest *req;

    req = scsi_req_alloc(&scsi_disk_reqops, &s->qdev, tag, lun, hba_private);
    return req;
}

#ifdef __linux__
static int get_device_type(SCSIDiskState *s)
{
    BlockDriverState *bdrv = s->qdev.conf.bs;
    uint8_t cmd[16];
    uint8_t buf[36];
    uint8_t sensebuf[8];
    sg_io_hdr_t io_header;
    int ret;

    memset(cmd, 0, sizeof(cmd));
    memset(buf, 0, sizeof(buf));
    cmd[0] = INQUIRY;
    cmd[4] = sizeof(buf);

    memset(&io_header, 0, sizeof(io_header));
    io_header.interface_id = 'S';
    io_header.dxfer_direction = SG_DXFER_FROM_DEV;
    io_header.dxfer_len = sizeof(buf);
    io_header.dxferp = buf;
    io_header.cmdp = cmd;
    io_header.cmd_len = sizeof(cmd);
    io_header.mx_sb_len = sizeof(sensebuf);
    io_header.sbp = sensebuf;
    io_header.timeout = 6000; /* XXX */

    ret = bdrv_ioctl(bdrv, SG_IO, &io_header);
    if (ret < 0 || io_header.driver_status || io_header.host_status) {
        return -1;
    }
    s->qdev.type = buf[0];
    if (buf[1] & 0x80) {
        s->features |= 1 << SCSI_DISK_F_REMOVABLE;
    }
    return 0;
}

static int scsi_block_initfn(SCSIDevice *dev)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, dev);
    int sg_version;
    int rc;

    if (!s->qdev.conf.bs) {
        error_report("scsi-block: drive property not set");
        return -1;
    }

    /* check we are using a driver managing SG_IO (version 3 and after) */
    if (bdrv_ioctl(s->qdev.conf.bs, SG_GET_VERSION_NUM, &sg_version) < 0 ||
        sg_version < 30000) {
        error_report("scsi-block: scsi generic interface too old");
        return -1;
    }

    /* get device type from INQUIRY data */
    rc = get_device_type(s);
    if (rc < 0) {
        error_report("scsi-block: INQUIRY failed");
        return -1;
    }

    /* Make a guess for the block size, we'll fix it when the guest sends.
     * READ CAPACITY.  If they don't, they likely would assume these sizes
     * anyway. (TODO: check in /sys).
     */
    if (s->qdev.type == TYPE_ROM || s->qdev.type == TYPE_WORM) {
        s->qdev.blocksize = 2048;
    } else {
        s->qdev.blocksize = 512;
    }
    return scsi_initfn(&s->qdev);
}

static SCSIRequest *scsi_block_new_request(SCSIDevice *d, uint32_t tag,
                                           uint32_t lun, uint8_t *buf,
                                           void *hba_private)
{
    SCSIDiskState *s = DO_UPCAST(SCSIDiskState, qdev, d);

    switch (buf[0]) {
    case READ_6:
    case READ_10:
    case READ_12:
    case READ_16:
    case VERIFY_10:
    case VERIFY_12:
    case VERIFY_16:
    case WRITE_6:
    case WRITE_10:
    case WRITE_12:
    case WRITE_16:
    case WRITE_VERIFY_10:
    case WRITE_VERIFY_12:
    case WRITE_VERIFY_16:
        /* If we are not using O_DIRECT, we might read stale data from the
	 * host cache if writes were made using other commands than these
	 * ones (such as WRITE SAME or EXTENDED COPY, etc.).  So, without
	 * O_DIRECT everything must go through SG_IO.
         */
        if (bdrv_get_flags(s->qdev.conf.bs) & BDRV_O_NOCACHE) {
            break;
        }

        /* MMC writing cannot be done via pread/pwrite, because it sometimes
         * involves writing beyond the maximum LBA or to negative LBA (lead-in).
         * And once you do these writes, reading from the block device is
         * unreliable, too.  It is even possible that reads deliver random data
         * from the host page cache (this is probably a Linux bug).
         *
         * We might use scsi_disk_reqops as long as no writing commands are
         * seen, but performance usually isn't paramount on optical media.  So,
         * just make scsi-block operate the same as scsi-generic for them.
         */
        if (s->qdev.type == TYPE_ROM) {
            break;
	}
        return scsi_req_alloc(&scsi_disk_reqops, &s->qdev, tag, lun,
                              hba_private);
    }

    return scsi_req_alloc(&scsi_generic_req_ops, &s->qdev, tag, lun,
                          hba_private);
}
#endif

#define DEFINE_SCSI_DISK_PROPERTIES()                           \
    DEFINE_BLOCK_PROPERTIES(SCSIDiskState, qdev.conf),          \
    DEFINE_PROP_STRING("ver",  SCSIDiskState, version),         \
    DEFINE_PROP_STRING("serial",  SCSIDiskState, serial)

static Property scsi_hd_properties[] = {
    DEFINE_SCSI_DISK_PROPERTIES(),
    DEFINE_PROP_BIT("removable", SCSIDiskState, features,
                    SCSI_DISK_F_REMOVABLE, false),
    DEFINE_PROP_BIT("dpofua", SCSIDiskState, features,
                    SCSI_DISK_F_DPOFUA, false),
    DEFINE_PROP_HEX64("wwn", SCSIDiskState, wwn, 0),
    DEFINE_BLOCK_CHS_PROPERTIES(SCSIDiskState, qdev.conf),
    DEFINE_PROP_END_OF_LIST(),
};

static const VMStateDescription vmstate_scsi_disk_state = {
    .name = "scsi-disk",
    .version_id = 1,
    .minimum_version_id = 1,
    .minimum_version_id_old = 1,
    .fields = (VMStateField[]) {
        VMSTATE_SCSI_DEVICE(qdev, SCSIDiskState),
        VMSTATE_BOOL(media_changed, SCSIDiskState),
        VMSTATE_BOOL(media_event, SCSIDiskState),
        VMSTATE_BOOL(eject_request, SCSIDiskState),
        VMSTATE_BOOL(tray_open, SCSIDiskState),
        VMSTATE_BOOL(tray_locked, SCSIDiskState),
        VMSTATE_END_OF_LIST()
    }
};

static void scsi_hd_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->init         = scsi_hd_initfn;
    sc->destroy      = scsi_destroy;
    sc->alloc_req    = scsi_new_request;
    sc->unit_attention_reported = scsi_disk_unit_attention_reported;
    dc->fw_name = "disk";
    dc->desc = "virtual SCSI disk";
    dc->reset = scsi_disk_reset;
    dc->props = scsi_hd_properties;
    dc->vmsd  = &vmstate_scsi_disk_state;
}

static TypeInfo scsi_hd_info = {
    .name          = "scsi-hd",
    .parent        = TYPE_SCSI_DEVICE,
    .instance_size = sizeof(SCSIDiskState),
    .class_init    = scsi_hd_class_initfn,
};

static Property scsi_cd_properties[] = {
    DEFINE_SCSI_DISK_PROPERTIES(),
    DEFINE_PROP_HEX64("wwn", SCSIDiskState, wwn, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void scsi_cd_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->init         = scsi_cd_initfn;
    sc->destroy      = scsi_destroy;
    sc->alloc_req    = scsi_new_request;
    sc->unit_attention_reported = scsi_disk_unit_attention_reported;
    dc->fw_name = "disk";
    dc->desc = "virtual SCSI CD-ROM";
    dc->reset = scsi_disk_reset;
    dc->props = scsi_cd_properties;
    dc->vmsd  = &vmstate_scsi_disk_state;
}

static TypeInfo scsi_cd_info = {
    .name          = "scsi-cd",
    .parent        = TYPE_SCSI_DEVICE,
    .instance_size = sizeof(SCSIDiskState),
    .class_init    = scsi_cd_class_initfn,
};

#ifdef __linux__
static Property scsi_block_properties[] = {
    DEFINE_SCSI_DISK_PROPERTIES(),
    DEFINE_PROP_END_OF_LIST(),
};

static void scsi_block_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->init         = scsi_block_initfn;
    sc->destroy      = scsi_destroy;
    sc->alloc_req    = scsi_block_new_request;
    dc->fw_name = "disk";
    dc->desc = "SCSI block device passthrough";
    dc->reset = scsi_disk_reset;
    dc->props = scsi_block_properties;
    dc->vmsd  = &vmstate_scsi_disk_state;
}

static TypeInfo scsi_block_info = {
    .name          = "scsi-block",
    .parent        = TYPE_SCSI_DEVICE,
    .instance_size = sizeof(SCSIDiskState),
    .class_init    = scsi_block_class_initfn,
};
#endif

static Property scsi_disk_properties[] = {
    DEFINE_SCSI_DISK_PROPERTIES(),
    DEFINE_PROP_BIT("removable", SCSIDiskState, features,
                    SCSI_DISK_F_REMOVABLE, false),
    DEFINE_PROP_BIT("dpofua", SCSIDiskState, features,
                    SCSI_DISK_F_DPOFUA, false),
    DEFINE_PROP_HEX64("wwn", SCSIDiskState, wwn, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void scsi_disk_class_initfn(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SCSIDeviceClass *sc = SCSI_DEVICE_CLASS(klass);

    sc->init         = scsi_disk_initfn;
    sc->destroy      = scsi_destroy;
    sc->alloc_req    = scsi_new_request;
    sc->unit_attention_reported = scsi_disk_unit_attention_reported;
    dc->fw_name = "disk";
    dc->desc = "virtual SCSI disk or CD-ROM (legacy)";
    dc->reset = scsi_disk_reset;
    dc->props = scsi_disk_properties;
    dc->vmsd  = &vmstate_scsi_disk_state;
}

static TypeInfo scsi_disk_info = {
    .name          = "scsi-disk",
    .parent        = TYPE_SCSI_DEVICE,
    .instance_size = sizeof(SCSIDiskState),
    .class_init    = scsi_disk_class_initfn,
};

static void scsi_disk_register_types(void)
{
    type_register_static(&scsi_hd_info);
    type_register_static(&scsi_cd_info);
#ifdef __linux__
    type_register_static(&scsi_block_info);
#endif
    type_register_static(&scsi_disk_info);
}

type_init(scsi_disk_register_types)
