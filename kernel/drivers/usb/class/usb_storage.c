/*
 * A20OS — USB Mass Storage (BOT / SCSI transparent) class driver.
 *
 * Implements the USB Bulk-Only Transport (BOT): 31-byte CBW on the bulk-OUT
 * endpoint, optional data phase, 13-byte CSW on bulk-IN.  On top of that a
 * minimal SCSI subset (TEST UNIT READY, READ CAPACITY(10), READ(10),
 * WRITE(10), REQUEST SENSE) drives the block device exposed to the VFS as a
 * DEV_CLASS_BLOCK node (/dev/diskN) and mountable through block_cache.
 *
 * The CBW/CSW layouts and SCSI CDBs are public standards (USB-IF BOT spec,
 * SCSI SPC/SBC).  The transport flow (CBW → data → CSW, tag matching, reset
 * recovery) follows the approach of ViudiraTech/Uinxed-Kernel (Apache-2.0);
 * this implementation is rewritten for A20OS's USB core / driver model.
 * See docs/ACKNOWLEDGMENTS.md.
 */
#include "drivers/usb/usb.h"
#include "drivers/usb/usb_storage.h"
#include <stdint.h>

#include "core/klog.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/timer.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "mm/slab.h"
#include "core/errno.h"

#define USB_MSC_IO_CHUNK        4096U
#define USB_MSC_CMD_TIMEOUT_TICKS (TICKS_PER_SEC * 5)
#define USB_MSC_POLL_LIMIT      50000000U

/* SCSI opcodes (subset). */
#define SCSI_CMD_TEST_UNIT_READY 0x00U
#define SCSI_CMD_REQUEST_SENSE   0x03U
#define SCSI_CMD_INQUIRY         0x12U
#define SCSI_CMD_READ_CAPACITY10 0x25U
#define SCSI_CMD_READ10          0x28U
#define SCSI_CMD_WRITE10         0x2AU
#define SCSI_STATUS_GOOD         0x00U

#define USB_MSC_CBW_SIGNATURE    0x43425355U   /* "USBC" */
#define USB_MSC_CSW_SIGNATURE    0x53425355U   /* "USBS" */
#define USB_MSC_CSW_STATUS_PASS  0x00U

#define USB_MSC_CBW_SIZE         31U
#define USB_MSC_CSW_SIZE         13U

typedef struct __attribute__((packed)) usb_msc_cbw {
    uint32_t signature;         /* USB_MSC_CBW_SIGNATURE */
    uint32_t tag;
    uint32_t transfer_length;   /* data-phase byte count (LE) */
    uint8_t  flags;             /* 0x80 = data IN */
    uint8_t  lun;
    uint8_t  command_length;    /* valid CDB bytes (1..16) */
    uint8_t  command[16];       /* SCSI CDB */
} usb_msc_cbw_t;

typedef struct __attribute__((packed)) usb_msc_csw {
    uint32_t signature;         /* USB_MSC_CSW_SIGNATURE */
    uint32_t tag;
    uint32_t residue;
    uint8_t  status;            /* 0 = pass */
} usb_msc_csw_t;

typedef struct {
    usb_interface_t *iface;
    usb_endpoint_t  *bulk_out;
    usb_endpoint_t  *bulk_in;
    usb_device_t    *dev;

    /* Serializes whole BOT exchanges.  A mutex, not a spinlock: one command
     * spans several synchronous bulk transfers and can spend seconds waiting
     * for the device, and the old code held a raw spinlock (with interrupts
     * off) across all of it.  The block layer calls here from process context,
     * so sleeping is allowed. */
    mutex_t lock;
    block_dev_t block;
    uint32_t tag;
    uint64_t capacity;
    uint32_t sector_size;
    int ready;
    device_t *owner;
    int closing;
    int endpoints_live;
    int probing;
    int recovery_pending;
    uint32_t active_calls;
    uint32_t consumer_refs;

    /* Each BOT stage fits in one xHCI Normal TRB and must not cross a
     * 64 KiB boundary. Align each buffer to its containing power of two. */
    usb_msc_cbw_t  cbw ALIGNED(32);
    usb_msc_csw_t  csw ALIGNED(16);
    uint8_t        data[USB_MSC_IO_CHUNK] ALIGNED(USB_MSC_IO_CHUNK);
} usb_storage_dev_t;

static usb_storage_dev_t g_storage[USB_STORAGE_MAX_DEVS];
/* Slot admission/reuse only; never nest with the BOT mutex or HCD calls. */
static spinlock_t g_storage_lock = SPINLOCK_INIT;

static int usb_storage_stop(void *context);

static usb_storage_dev_t *msc_claim(device_t *dev) {
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    usb_storage_dev_t *st = NULL;
    for (unsigned i = 0; i < USB_STORAGE_MAX_DEVS; i++) {
        if (g_storage[i].owner || g_storage[i].consumer_refs ||
            g_storage[i].active_calls) continue;
        st = &g_storage[i];
        memset(st, 0, sizeof(*st));
        st->owner = dev;
        dev->drv_priv = st;
        break;
    }
    spin_unlock_irqrestore(&g_storage_lock, flags);
    return st;
}

static int msc_call_begin(usb_storage_dev_t *st) {
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    int ret = 0;
    if (!st->owner || !st->ready || st->closing) ret = -ENODEV;
    else if (st->active_calls == UINT32_MAX) ret = -EOVERFLOW;
    else st->active_calls++;
    spin_unlock_irqrestore(&g_storage_lock, flags);
    return ret;
}

static void msc_call_end(usb_storage_dev_t *st) {
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    if (st->active_calls) st->active_calls--;
    spin_unlock_irqrestore(&g_storage_lock, flags);
}

static void msc_release(block_dev_t *block) {
    usb_storage_dev_t *st = block->priv;
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    if (st->consumer_refs) st->consumer_refs--;
    spin_unlock_irqrestore(&g_storage_lock, flags);
}

static uint32_t msc_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void msc_put_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

/* One bulk transfer.  Synchronous: the HCD (xHCI) submits and waits. */
static int msc_bulk(usb_storage_dev_t *st, usb_endpoint_t *ep,
                    void *buf, size_t len, size_t *actual) {
    usb_urb_t urb;
    memset(&urb, 0, sizeof(urb));
    urb.dev = st->dev;
    urb.ep = ep;
    urb.transfer_type = USB_XFER_BULK;
    urb.direction = (ep->addr & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT;
    urb.buf = buf;
    urb.len = len;
    int ret = usb_submit_urb(&urb);
    if (actual) *actual = 0;
    if (!ret || ret == -EMSGSIZE) {
        if (urb.actual_length > len) return -EIO;
        if (actual) *actual = urb.actual_length;
        if (!ret && urb.actual_length != len) ret = -EMSGSIZE;
    }
    return ret;
}

static int msc_recovery_begin(usb_storage_dev_t *st) {
    /* Probe already owns lifecycle. Runtime calls must not wait for a
     * lifecycle callback that might itself be waiting for block I/O. */
    if (st->probing) driver_lifecycle_begin();
    else {
        int ret = driver_lifecycle_try_begin();
        if (ret) return ret;
    }
    if (__atomic_load_n(&st->closing, __ATOMIC_ACQUIRE) ||
        device_is_stopping(st->owner) || st->owner->plat_data != st->iface) {
        driver_lifecycle_end();
        return -ESHUTDOWN;
    }
    return 0;
}

/* BOT 1.0 section 5.3.4: Reset, Clear HALT IN, Clear HALT OUT.
 * The BOT mutex spans the entire recovery. Retire host endpoint contexts first
 * so their dequeue/toggle state cannot outlive the device-side reset. Rebuild
 * only after all three control requests succeed. No failed command is replayed.
 * https://www.usb.org/sites/default/files/usbmassbulk_10.pdf */
static int msc_reset(usb_storage_dev_t *st) {
    st->recovery_pending = 1;
    int ret = msc_recovery_begin(st);
    if (ret) return ret;
    usb_hcd_t *hcd = st->dev->hcd;
    ret = hcd->ops->release_endpoint(hcd, st->dev, st->bulk_in->addr);
    if (ret) goto out;
    ret = hcd->ops->release_endpoint(hcd, st->dev, st->bulk_out->addr);
    if (ret) goto out;
    ret = usb_control_msg(st->dev, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                          USB_REQ_BULK_ONLY_RESET, 0,
                          st->iface->interface_number, NULL, 0);
    if (ret) goto out;
    ret = usb_control_msg(st->dev, USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                          USB_REQ_CLEAR_FEATURE, USB_FEAT_ENDPOINT_HALT,
                          st->bulk_in->addr, NULL, 0);
    if (ret) goto out;
    ret = usb_control_msg(st->dev, USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                          USB_REQ_CLEAR_FEATURE, USB_FEAT_ENDPOINT_HALT,
                          st->bulk_out->addr, NULL, 0);
    if (ret) goto out;
    ret = hcd->ops->configure_endpoint(hcd, st->dev, st->bulk_out->addr,
                                      USB_XFER_BULK, st->bulk_out->max_packet, 0);
    if (ret) goto out;
    ret = hcd->ops->configure_endpoint(hcd, st->dev, st->bulk_in->addr,
                                      USB_XFER_BULK, st->bulk_in->max_packet, 0);
    if (!ret) st->recovery_pending = 0;
out:
    driver_lifecycle_end();
    return ret > 0 ? -EIO : ret;
}

/* Clear a stalled pipe without discarding the device's pending CSW. The
 * retired host context is rebuilt with the reset device-side data toggle. */
static int msc_clear_halt(usb_storage_dev_t *st, usb_endpoint_t *ep) {
    st->recovery_pending = 1;
    int ret = msc_recovery_begin(st);
    if (ret) return ret;
    usb_hcd_t *hcd = st->dev->hcd;
    ret = hcd->ops->release_endpoint(hcd, st->dev, ep->addr);
    if (ret) goto out;
    ret = usb_control_msg(st->dev, USB_TYPE_STANDARD | USB_RECIP_ENDPOINT,
                          USB_REQ_CLEAR_FEATURE, USB_FEAT_ENDPOINT_HALT,
                          ep->addr, NULL, 0);
    if (ret) goto out;
    ret = hcd->ops->configure_endpoint(hcd, st->dev, ep->addr,
                                      USB_XFER_BULK, ep->max_packet, 0);
    if (!ret) st->recovery_pending = 0;
out:
    driver_lifecycle_end();
    return ret > 0 ? -EIO : ret;
}

/* Send a SCSI CDB, transfer the data phase, read the CSW. */
static int msc_command(usb_storage_dev_t *st, const uint8_t *cdb,
                       uint8_t cdblen, void *data, uint32_t datalen,
                       int data_in) {
    if (!cdb || !cdblen || cdblen > 16 || (datalen && !data)) return -EINVAL;
    mutex_lock(&st->lock);
    int result = -ESHUTDOWN;
    if (__atomic_load_n(&st->closing, __ATOMIC_ACQUIRE)) goto out;
    if (st->recovery_pending) {
        result = msc_reset(st);
        if (result) goto out; /* No CBW until the previous recovery succeeds. */
    }

    uint32_t tag = ++st->tag;
    memset(&st->cbw, 0, sizeof(st->cbw));
    st->cbw.signature = USB_MSC_CBW_SIGNATURE;
    st->cbw.tag = tag;
    st->cbw.transfer_length = datalen;
    st->cbw.flags = data_in ? 0x80 : 0x00;
    st->cbw.lun = 0;
    st->cbw.command_length = cdblen;
    memcpy(st->cbw.command, cdb, cdblen);

    result = msc_bulk(st, st->bulk_out, &st->cbw, USB_MSC_CBW_SIZE, NULL);
    if (result) goto recover;

    uint32_t transferred = 0;
    int incomplete = 0;
    if (datalen) {
        usb_endpoint_t *dep = data_in ? st->bulk_in : st->bulk_out;
        while (transferred < datalen) {
            uint32_t chunk = datalen - transferred;
            if (chunk > USB_MSC_IO_CHUNK) chunk = USB_MSC_IO_CHUNK;
            if (!data_in) memcpy(st->data, (uint8_t *)data + transferred, chunk);
            size_t actual = 0;
            result = msc_bulk(st, dep, st->data, chunk, &actual);
            if (result == -EPIPE) {
                incomplete = 1;
                result = msc_clear_halt(st, dep);
                if (result) goto recover;
                break; /* Data terminated; collect this command's status. */
            }
            if (result && result != -EMSGSIZE) goto recover;
            if (data_in) memcpy((uint8_t *)data + transferred, st->data, actual);
            transferred += (uint32_t)actual;
            if (result == -EMSGSIZE) { incomplete = 1; break; }
        }
    }

    memset(&st->csw, 0, sizeof(st->csw));
    result = msc_bulk(st, st->bulk_in, &st->csw, USB_MSC_CSW_SIZE, NULL);
    if (result == -EPIPE) {
        result = msc_clear_halt(st, st->bulk_in);
        if (result) goto recover;
        /* BOT Figure 2: one retry after clearing a first CSW STALL. */
        memset(&st->csw, 0, sizeof(st->csw));
        result = msc_bulk(st, st->bulk_in, &st->csw, USB_MSC_CSW_SIZE, NULL);
    }
    if (result) goto recover;

    /* A failed-command status is meaningful only in a valid CSW. Phase
     * errors and malformed signatures/tags/residues require reset recovery. */
    int invalid = st->csw.signature != USB_MSC_CSW_SIGNATURE ||
                  st->csw.tag != tag || st->csw.residue > datalen ||
                  st->csw.status > 2;
    result = !invalid && st->csw.status == USB_MSC_CSW_STATUS_PASS &&
             !st->csw.residue && !incomplete ? 0 : -EIO;
    if (result) {
        static unsigned reports;
        if (__atomic_fetch_add(&reports, 1, __ATOMIC_RELAXED) < 16)
            kwarn("[USB-STORAGE] command failed opcode=%02x tag=%u signature=%08x csw_tag=%u status=%u residue=%u length=%u\n",
                  cdb[0], tag, st->csw.signature, st->csw.tag,
                  st->csw.status, st->csw.residue, datalen);
    }
    if (invalid || st->csw.status == 2) goto recover;
    goto out;
recover:
    if (result > 0) result = -EIO;
    /* Preserve this command's failure even if reset succeeds. A failed reset
     * remains pending for the next caller, still under this same mutex. */
    (void)msc_reset(st);
out:
    mutex_unlock(&st->lock);
    return result;
}

/* Hold one active call across the full BOT command, including its recovery.
 * A queued command observes closing after acquiring the BOT mutex. */
static int msc_rw(block_dev_t *block, uint64_t lba, void *buf,
                   size_t sectors, int write) {
    usb_storage_dev_t *st = block ? block->priv : NULL;
    if (!st || !buf) return -EINVAL;
    int ret = msc_call_begin(st);
    if (ret) return ret;
    if (!sectors || sectors > 0xffffU || lba > 0xffffffffU ||
        lba >= st->capacity || sectors > st->capacity - lba) {
        ret = -EIO;
    } else {
        uint8_t cdb[10] = { 0 };
        cdb[0] = write ? SCSI_CMD_WRITE10 : SCSI_CMD_READ10;
        msc_put_be32(&cdb[2], (uint32_t)lba);
        cdb[7] = (uint8_t)(sectors >> 8);
        cdb[8] = (uint8_t)sectors;
        ret = msc_command(st, cdb, 10, buf,
                          (uint32_t)sectors * st->sector_size, !write);
    }
    msc_call_end(st);
    return ret;
}

static int msc_read_sectors(block_dev_t *block, uint64_t lba, void *buf,
                            size_t sectors) {
    return msc_rw(block, lba, buf, sectors, 0);
}

static int msc_write_sectors(block_dev_t *block, uint64_t lba, const void *buf,
                             size_t sectors) {
    return msc_rw(block, lba, (void *)buf, sectors, 1);
}

/* Clear a stale unit attention with REQUEST SENSE (unparsed). */
static void msc_request_sense(usb_storage_dev_t *st) {
    uint8_t cdb[6] = { SCSI_CMD_REQUEST_SENSE, 0, 0, 0, 18, 0 };
    uint8_t sense[18];
    (void)msc_command(st, cdb, 6, sense, 18, 1);
}

/* ------------------------------------------------------------------ */
/* class ops (DEV_CLASS_BLOCK → /dev/diskN)                            */
/* ------------------------------------------------------------------ */

static int msc_class_read(device_t *dev, uint64_t sector, void *buf, size_t count) {
    usb_storage_dev_t *st = dev ? (usb_storage_dev_t *)dev->drv_priv : NULL;
    return st ? msc_read_sectors(&st->block, sector, buf, count) : -ENODEV;
}

static int msc_class_write(device_t *dev, uint64_t sector, const void *buf, size_t count) {
    usb_storage_dev_t *st = dev ? (usb_storage_dev_t *)dev->drv_priv : NULL;
    return st ? msc_write_sectors(&st->block, sector, buf, count) : -ENODEV;
}

static int msc_class_flush(device_t *dev) {
    (void)dev;
    return 0;
}

static uint64_t msc_class_capacity(device_t *dev) {
    usb_storage_dev_t *st = dev ? (usb_storage_dev_t *)dev->drv_priv : NULL;
    return st ? st->capacity : 0;
}

static uint32_t msc_class_sector_size(device_t *dev) {
    usb_storage_dev_t *st = dev ? (usb_storage_dev_t *)dev->drv_priv : NULL;
    return st ? st->sector_size : 512;
}

static const block_dev_ops_t usb_storage_ops = {
    .read = msc_class_read,
    .write = msc_class_write,
    .flush = msc_class_flush,
    .capacity = msc_class_capacity,
    .sector_size = msc_class_sector_size,
};

/* ------------------------------------------------------------------ */
/* probe / remove                                                      */
/* ------------------------------------------------------------------ */

static int usb_storage_probe(device_t *dev) {
    usb_interface_t *iface = (usb_interface_t *)dev->plat_data;
    if (!iface || !iface->dev || !iface->dev->hcd || !iface->dev->hcd->ops)
        return -EINVAL;
    usb_device_t *udev = iface->dev;

    usb_endpoint_t *bulk_out = NULL, *bulk_in = NULL;
    for (uint8_t i = 0; i < iface->ep_count; i++) {
        uint8_t xfer = iface->eps[i].attrs & 3U;
        if (xfer != USB_XFER_BULK)
            continue;
        if ((iface->eps[i].addr & USB_DIR_IN) && !bulk_in)
            bulk_in = &iface->eps[i];
        if (!(iface->eps[i].addr & USB_DIR_IN) && !bulk_out)
            bulk_out = &iface->eps[i];
    }
    if (!bulk_out || !bulk_in)
        return -ENODEV;

    if (!udev->hcd->ops->configure_endpoint || !udev->hcd->ops->release_endpoint)
        return -EOPNOTSUPP;
    int r = device_probe_use_remove(dev);
    if (r) return r;
    usb_storage_dev_t *st = msc_claim(dev);
    if (!st) return -ENOSPC;
    st->lock = (mutex_t)MUTEX_INIT;
    st->iface = iface;
    st->dev = udev;
    st->bulk_out = bulk_out;
    st->bulk_in = bulk_in;
    st->sector_size = 512;
    st->tag = 0;
    st->probing = 1;

    r = device_add_execution_action(dev, usb_storage_stop, NULL, st);
    if (r) return r;
    /* Reset device-side endpoint state before creating new host contexts,
     * including when a previously stopped interface is rebound. */
    r = usb_control_msg(udev, USB_TYPE_STANDARD | USB_RECIP_INTERFACE,
                        USB_REQ_SET_INTERFACE, iface->alt_setting,
                        iface->interface_number, NULL, 0);
    if (r) return r;

    /* A failed configure may already have published a host cleanup owner. */
    st->endpoints_live = 1;
    r = udev->hcd->ops->configure_endpoint(udev->hcd, udev, bulk_out->addr,
                                         USB_XFER_BULK, bulk_out->max_packet, 0);
    if (r) return r;
    r = udev->hcd->ops->configure_endpoint(udev->hcd, udev, bulk_in->addr,
                                         USB_XFER_BULK, bulk_in->max_packet, 0);
    if (r) return r;

    /* TEST UNIT READY with a couple of retries (unit attention). */
    uint8_t tur[6] = { SCSI_CMD_TEST_UNIT_READY };
    for (int i = 0; i < 5; i++) {
        if (msc_command(st, tur, 6, NULL, 0, 0) == 0)
            break;
        msc_request_sense(st);
    }

    /* READ CAPACITY(10). */
    uint8_t cap[8];
    uint8_t rccdb[10] = { SCSI_CMD_READ_CAPACITY10 };
    if (msc_command(st, rccdb, 10, cap, 8, 1) < 0) {
        kinfo("[USB-STORAGE] READ CAPACITY failed; no disk\n");
        return -ENODEV;
    }
    uint64_t last_lba = msc_be32(cap);
    uint32_t blk_size = msc_be32(cap + 4);
    if (blk_size < 512 || blk_size > 65536 || (blk_size & (blk_size - 1))) {
        kinfo("[USB-STORAGE] unsupported sector size %u\n", blk_size);
        return -ENODEV;
    }
    st->capacity = last_lba + 1;
    st->sector_size = blk_size;

    st->block.read_sector = msc_read_sectors;
    st->block.write_sector = msc_write_sectors;
    st->block.capacity = st->capacity;
    st->block.sector_size = st->sector_size;
    st->block.priv = st;
    st->block.release = msc_release;
    st->probing = 0;
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    st->ready = 1;
    spin_unlock_irqrestore(&g_storage_lock, flags);

    kinfo("[USB-STORAGE] disk ready: %llu sectors (%llu MiB), sector=%u\n",
          (unsigned long long)st->capacity,
          (unsigned long long)(st->capacity * blk_size / (1024U * 1024U)),
          st->sector_size);
    return 0;
}

static int usb_storage_stop(void *context) {
    usb_storage_dev_t *st = context;
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    __atomic_store_n(&st->closing, 1, __ATOMIC_RELEASE);
    st->ready = 0;
    int busy = st->active_calls != 0;
    spin_unlock_irqrestore(&g_storage_lock, flags);
    /* Do not wait under lifecycle: an active transfer may need USB progress. */
    if (busy) return -EBUSY;
    if (st->endpoints_live) {
        int ret = usb_interface_release_endpoints(st->iface);
        if (ret) return ret;
        st->endpoints_live = 0;
    }
    return 0;
}

static int usb_storage_remove(device_t *dev) {
    usb_storage_dev_t *st = dev ? dev->drv_priv : NULL;
    if (!st) return 0;
    int ret = usb_storage_stop(st);
    if (ret) return ret;
    /* Endpoints are stopped and class calls drained. Consumer references
     * retain only the old closed block identity until their final release. */
    st->iface = NULL;
    st->dev = NULL;
    st->bulk_in = st->bulk_out = NULL;
    dev->drv_priv = NULL;
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    st->owner = NULL;
    spin_unlock_irqrestore(&g_storage_lock, flags);
    return 0;
}

static const device_id_t usb_storage_ids[] = {
    { .vendor = USB_CLASS_MASS_STORAGE,
      .device = (USB_SUBCLASS_SCSI << 8) | USB_PROTOCOL_BULK_ONLY,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t usb_storage_driver = {
    .name = "usb-storage",
    .id_table = usb_storage_ids,
    .bus = NULL,               /* bound via usb bus match on interface */
    .probe = usb_storage_probe,
    .remove = usb_storage_remove,
    .class_ops = &usb_storage_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(usb_storage_driver);

/* ------------------------------------------------------------------ */
/* Getter for mount_block_devices()                                    */
/* ------------------------------------------------------------------ */

block_dev_t *usb_storage_get_dev(int index) {
    if (index < 0 || index >= USB_STORAGE_MAX_DEVS) return NULL;
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    usb_storage_dev_t *st = &g_storage[index];
    block_dev_t *block = NULL;
    if (st->owner && st->ready && !st->closing && st->consumer_refs != UINT32_MAX) {
        st->consumer_refs++;
        block = &st->block;
    }
    spin_unlock_irqrestore(&g_storage_lock, flags);
    return block;
}

int usb_storage_ready(int index) {
    if (index < 0 || index >= USB_STORAGE_MAX_DEVS) return 0;
    uint64_t flags = spin_lock_irqsave(&g_storage_lock);
    usb_storage_dev_t *st = &g_storage[index];
    int ready = st->owner && st->ready && !st->closing;
    spin_unlock_irqrestore(&g_storage_lock, flags);
    return ready;
}
