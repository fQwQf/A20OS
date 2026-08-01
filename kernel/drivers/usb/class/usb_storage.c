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

#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/timer.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "mm/slab.h"
#include "abi/linux/errno.h"

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

    spinlock_t lock;
    block_dev_t block;
    uint32_t tag;
    uint64_t capacity;
    uint32_t sector_size;
    int ready;

    /* DMA buffers (kernel heap, physically contiguous). */
    usb_msc_cbw_t  cbw;
    usb_msc_csw_t  csw;
    uint8_t        data[USB_MSC_IO_CHUNK];
} usb_storage_dev_t;

static usb_storage_dev_t g_storage[USB_STORAGE_MAX_DEVS];
static int g_storage_count;

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
                    void *buf, size_t len) {
    usb_urb_t urb;
    memset(&urb, 0, sizeof(urb));
    urb.dev = st->dev;
    urb.ep = ep;
    urb.transfer_type = USB_XFER_BULK;
    urb.direction = (ep->addr & USB_DIR_IN) ? USB_DIR_IN : USB_DIR_OUT;
    urb.buf = buf;
    urb.len = len;
    return usb_submit_urb(&urb);
}

/* Bulk-Only Mass Storage Reset (class control request). */
static void msc_reset(usb_storage_dev_t *st) {
    (void)usb_control_msg(st->dev, USB_TYPE_CLASS | USB_RECIP_INTERFACE,
                          USB_REQ_BULK_ONLY_RESET, 0,
                          st->iface->interface_number, NULL, 0);
}

/* Send a SCSI CDB, transfer the data phase, read the CSW. */
static int msc_command(usb_storage_dev_t *st, const uint8_t *cdb,
                       uint8_t cdblen, void *data, uint32_t datalen,
                       int data_in) {
    uint64_t flags = spin_lock_irqsave(&st->lock);

    uint32_t tag = ++st->tag;
    memset(&st->cbw, 0, sizeof(st->cbw));
    st->cbw.signature = USB_MSC_CBW_SIGNATURE;
    st->cbw.tag = tag;
    st->cbw.transfer_length = datalen;
    st->cbw.flags = data_in ? 0x80 : 0x00;
    st->cbw.lun = 0;
    st->cbw.command_length = cdblen;
    memcpy(st->cbw.command, cdb, cdblen > 16 ? 16 : cdblen);

    /* 1. CBW on bulk-OUT. */
    if (msc_bulk(st, st->bulk_out, &st->cbw, USB_MSC_CBW_SIZE) < 0) {
        spin_unlock_irqrestore(&st->lock, flags);
        msc_reset(st);
        return -EIO;
    }

    /* 2. Data phase (chunked). */
    uint32_t transferred = 0;
    if (datalen) {
        usb_endpoint_t *dep = data_in ? st->bulk_in : st->bulk_out;
        while (transferred < datalen) {
            uint32_t chunk = datalen - transferred;
            if (chunk > USB_MSC_IO_CHUNK)
                chunk = USB_MSC_IO_CHUNK;
            memcpy(st->data, (uint8_t *)data + transferred, chunk);
            int r = msc_bulk(st, dep, st->data, chunk);
            if (r < 0) {
                spin_unlock_irqrestore(&st->lock, flags);
                msc_reset(st);
                return -EIO;
            }
            if (data_in)
                memcpy((uint8_t *)data + transferred, st->data, chunk);
            transferred += chunk;
        }
    }

    /* 3. CSW on bulk-IN. */
    memset(&st->csw, 0, sizeof(st->csw));
    if (msc_bulk(st, st->bulk_in, &st->csw, USB_MSC_CSW_SIZE) < 0) {
        spin_unlock_irqrestore(&st->lock, flags);
        msc_reset(st);
        return -EIO;
    }

    int result = -EIO;
    if (st->csw.signature == USB_MSC_CSW_SIGNATURE &&
        st->csw.tag == tag && st->csw.status == USB_MSC_CSW_STATUS_PASS)
        result = 0;
    else if (st->csw.status == 1)
        result = -EIO;          /* command failed (see REQUEST SENSE) */

    spin_unlock_irqrestore(&st->lock, flags);
    return result;
}

static int msc_read_sectors(block_dev_t *block, uint64_t lba, void *buf,
                            size_t sectors) {
    usb_storage_dev_t *st = (usb_storage_dev_t *)block->priv;
    if (!st || !st->ready || sectors == 0 || sectors > 0xffffU ||
        lba > 0xffffffffU || lba + sectors > st->capacity)
        return -EIO;
    uint8_t cdb[10] = { 0 };
    cdb[0] = SCSI_CMD_READ10;
    msc_put_be32(&cdb[2], (uint32_t)lba);
    cdb[7] = (uint8_t)(sectors >> 8);
    cdb[8] = (uint8_t)sectors;
    return msc_command(st, cdb, 10, buf, (uint32_t)sectors * st->sector_size, 1);
}

static int msc_write_sectors(block_dev_t *block, uint64_t lba, const void *buf,
                             size_t sectors) {
    usb_storage_dev_t *st = (usb_storage_dev_t *)block->priv;
    if (!st || !st->ready || sectors == 0 || sectors > 0xffffU ||
        lba > 0xffffffffU || lba + sectors > st->capacity)
        return -EIO;
    uint8_t cdb[10] = { 0 };
    cdb[0] = SCSI_CMD_WRITE10;
    msc_put_be32(&cdb[2], (uint32_t)lba);
    cdb[7] = (uint8_t)(sectors >> 8);
    cdb[8] = (uint8_t)sectors;
    return msc_command(st, cdb, 10, (void *)buf,
                       (uint32_t)sectors * st->sector_size, 0);
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
    if (!iface || !iface->dev || !iface->dev->hcd)
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

    if (g_storage_count >= USB_STORAGE_MAX_DEVS)
        return -ENOMEM;
    usb_storage_dev_t *st = &g_storage[g_storage_count];
    memset(st, 0, sizeof(*st));
    spin_init(&st->lock);
    st->iface = iface;
    st->dev = udev;
    st->bulk_out = bulk_out;
    st->bulk_in = bulk_in;
    st->sector_size = 512;
    st->tag = 0;

    /* Configure both bulk endpoints (xHCI type 2 = OUT, 6 = IN). */
    int r = udev->hcd->ops->configure_endpoint(udev->hcd, udev, bulk_out->addr, 2,
                                               bulk_out->max_packet, 0);
    if (r) {
        kfree(st);
        memset(st, 0, sizeof(*st));
        return r;
    }
    r = udev->hcd->ops->configure_endpoint(udev->hcd, udev, bulk_in->addr, 6,
                                           bulk_in->max_packet, 0);
    if (r) {
        kfree(st);
        memset(st, 0, sizeof(*st));
        return r;
    }

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
        kfree(st);
        memset(st, 0, sizeof(*st));
        return -ENODEV;
    }
    uint64_t last_lba = msc_be32(cap);
    uint32_t blk_size = msc_be32(cap + 4);
    if (blk_size < 512 || blk_size > 65536 || (blk_size & (blk_size - 1))) {
        kinfo("[USB-STORAGE] unsupported sector size %u\n", blk_size);
        kfree(st);
        memset(st, 0, sizeof(*st));
        return -ENODEV;
    }
    st->capacity = last_lba + 1;
    st->sector_size = blk_size;

    st->block.read_sector = msc_read_sectors;
    st->block.write_sector = msc_write_sectors;
    st->block.capacity = st->capacity;
    st->block.sector_size = st->sector_size;
    st->block.priv = st;
    st->ready = 1;
    dev->drv_priv = st;
    g_storage_count++;

    kinfo("[USB-STORAGE] disk ready: %llu sectors (%llu MiB), sector=%u\n",
          (unsigned long long)st->capacity,
          (unsigned long long)(st->capacity * blk_size / (1024U * 1024U)),
          st->sector_size);
    return 0;
}

static int usb_storage_remove(device_t *dev) {
    usb_storage_dev_t *st = (usb_storage_dev_t *)dev->drv_priv;
    if (st) {
        st->ready = 0;
        dev->drv_priv = NULL;
    }
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
    if (index < 0 || index >= g_storage_count || !g_storage[index].ready)
        return NULL;
    return &g_storage[index].block;
}

int usb_storage_ready(int index) {
    return usb_storage_get_dev(index) != NULL;
}
