#include "drivers/block/virtio_scsi.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/timer.h"
#include "mm/mm.h"

#define VIRTIO_SCSI_QUEUE_SIZE 32
#define VIRTIO_SCSI_REQUEST_QUEUE 2U
#define VIRTIO_SCSI_MAX_DEVS 4
#define VIRTIO_SCSI_CDB_SIZE 32
#define VIRTIO_SCSI_SENSE_SIZE 96
#define VIRTIO_SCSI_TIMEOUT_TICKS (TICKS_PER_SEC * 10)
#define VIRTIO_SCSI_POLL_LIMIT    50000000U

#define SCSI_CMD_TEST_UNIT_READY 0x00U
#define SCSI_CMD_INQUIRY         0x12U
#define SCSI_CMD_READ_CAPACITY10 0x25U
#define SCSI_CMD_READ10          0x28U
#define SCSI_CMD_WRITE10         0x2AU
#define SCSI_STATUS_GOOD          0x00U
#define VIRTIO_SCSI_S_OK          0x00U

typedef struct {
    uint8_t lun[8];
    uint64_t tag;
    uint8_t task_attr;
    uint8_t prio;
    uint8_t crn;
    uint8_t cdb[VIRTIO_SCSI_CDB_SIZE];
} __attribute__((packed)) virtio_scsi_req_t;

typedef struct {
    uint32_t sense_len;
    uint32_t resid;
    uint16_t status_qualifier;
    uint8_t status;
    uint8_t response;
    uint8_t sense[VIRTIO_SCSI_SENSE_SIZE];
} __attribute__((packed)) virtio_scsi_resp_t;

typedef struct {
    virtq_desc_t desc[VIRTIO_SCSI_QUEUE_SIZE] ALIGNED(64);
    virtq_avail_t avail ALIGNED(64);
    virtq_used_t used ALIGNED(64);
} virtio_scsi_aux_queue_t;

typedef struct {
    virtio_transport_t vt;
    virtq_desc_t desc[VIRTIO_SCSI_QUEUE_SIZE] ALIGNED(64);
    virtq_avail_t avail ALIGNED(64);
    virtq_used_t used ALIGNED(64);
    virtio_scsi_req_t req ALIGNED(64);
    virtio_scsi_resp_t resp ALIGNED(64);
    virtio_scsi_aux_queue_t control;
    virtio_scsi_aux_queue_t event;
    spinlock_t lock;
    block_dev_t block;
    uint16_t last_used;
    uint64_t capacity;
    int slot;
    int ready;
} virtio_scsi_dev_t;

static virtio_scsi_dev_t g_scsi[VIRTIO_SCSI_MAX_DEVS];
static int g_scsi_count;

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static void put_be32(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)(value >> 24);
    p[1] = (uint8_t)(value >> 16);
    p[2] = (uint8_t)(value >> 8);
    p[3] = (uint8_t)value;
}

static int virtio_scsi_setup_queue(virtio_scsi_dev_t *dev, uint32_t queue,
                                   virtq_desc_t *desc, virtq_avail_t *avail,
                                   virtq_used_t *used) {
    virtio_transport_t *vt = &dev->vt;
    vt->write32(vt, VIRTIO_MMIO_QUEUE_SEL, queue);
    uint32_t queue_max = vt->read32(vt, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (queue_max < VIRTIO_SCSI_QUEUE_SIZE ||
        vt->read32(vt, VIRTIO_MMIO_QUEUE_READY)) {
        kerr("[VIRTIO-SCSI] queue %u unavailable (max=%u ready=%u)\n",
             queue, queue_max, vt->read32(vt, VIRTIO_MMIO_QUEUE_READY));
        return -1;
    }
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_SCSI_QUEUE_SIZE);
    memset(desc, 0, sizeof(virtq_desc_t) * VIRTIO_SCSI_QUEUE_SIZE);
    memset(avail, 0, sizeof(*avail));
    memset(used, 0, sizeof(*used));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)va_to_pa(desc));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_HIGH, (uint32_t)(va_to_pa(desc) >> 32));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)va_to_pa(avail));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_HIGH, (uint32_t)(va_to_pa(avail) >> 32));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)va_to_pa(used));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_HIGH, (uint32_t)(va_to_pa(used) >> 32));
    wmb();
    vt->write32(vt, VIRTIO_MMIO_QUEUE_READY, 1);
    return 0;
}

static int virtio_scsi_setup_queues(virtio_scsi_dev_t *dev) {
    if (virtio_scsi_setup_queue(dev, 0, dev->control.desc, &dev->control.avail,
                                &dev->control.used) != 0 ||
        virtio_scsi_setup_queue(dev, 1, dev->event.desc, &dev->event.avail,
                                &dev->event.used) != 0 ||
        virtio_scsi_setup_queue(dev, VIRTIO_SCSI_REQUEST_QUEUE, dev->desc,
                                &dev->avail, &dev->used) != 0)
        return -1;
    return 0;
}

static int virtio_scsi_command(virtio_scsi_dev_t *dev, const uint8_t *cdb,
                                void *data, size_t bytes, int data_in) {
    uint64_t flags = spin_lock_irqsave(&dev->lock);
    memset(&dev->req, 0, sizeof(dev->req));
    memset(&dev->resp, 0, sizeof(dev->resp));
    memcpy(dev->req.cdb, cdb, VIRTIO_SCSI_CDB_SIZE);
    /* VirtIO-SCSI's simple addressing: format 1, target 0, LUN 0. */
    dev->req.lun[0] = 1;
    dev->req.lun[1] = 0;
    dev->req.tag = (uint64_t)dev->last_used + 1;

    dev->desc[0].addr = va_to_pa(&dev->req);
    dev->desc[0].len = sizeof(dev->req);
    if (bytes && !data_in) {
        /* Device-readable descriptors must precede device-writable ones. */
        dev->desc[0].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[0].next = 1;
        dev->desc[1].addr = va_to_pa(data);
        dev->desc[1].len = (uint32_t)bytes;
        dev->desc[1].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[1].next = 2;
        dev->desc[2].addr = va_to_pa(&dev->resp);
        dev->desc[2].len = sizeof(dev->resp);
        dev->desc[2].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[2].next = 0;
    } else if (bytes) {
        /* VirtIO-SCSI input order is request, response, then data-in.  VBox
         * splits the chain at the first writable descriptor and therefore
         * interprets that first buffer as the response header. */
        dev->desc[0].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[0].next = 2;
        dev->desc[2].addr = va_to_pa(&dev->resp);
        dev->desc[2].len = sizeof(dev->resp);
        dev->desc[2].flags = VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE;
        dev->desc[2].next = 1;
        dev->desc[1].addr = va_to_pa(data);
        dev->desc[1].len = (uint32_t)bytes;
        dev->desc[1].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[1].next = 0;
    } else {
        dev->desc[0].flags = VIRTQ_DESC_F_NEXT;
        dev->desc[0].next = 2;
        dev->desc[2].addr = va_to_pa(&dev->resp);
        dev->desc[2].len = sizeof(dev->resp);
        dev->desc[2].flags = VIRTQ_DESC_F_WRITE;
        dev->desc[2].next = 0;
    }

    uint16_t used_before = dev->used.idx;
    uint16_t avail_slot = dev->avail.idx % VIRTIO_SCSI_QUEUE_SIZE;
    dev->avail.ring[avail_slot] = 0;
    wmb();
    dev->avail.idx++;
    arch_dma_sync_for_device(&dev->req, sizeof(dev->req));
    if (bytes)
        arch_dma_sync_for_device(data, bytes);
    arch_dma_sync_for_device(&dev->resp, sizeof(dev->resp));
    arch_dma_sync_for_device(dev->desc, sizeof(dev->desc));
    arch_dma_sync_for_device(&dev->avail, sizeof(dev->avail));
    arch_dma_sync_for_device(&dev->used, sizeof(dev->used));
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_QUEUE_NOTIFY, VIRTIO_SCSI_REQUEST_QUEUE);

    uint64_t deadline = timer_get_ticks() + VIRTIO_SCSI_TIMEOUT_TICKS;
    uint32_t spins = VIRTIO_SCSI_POLL_LIMIT;
    while (dev->used.idx == used_before) {
        arch_dma_sync_for_cpu(&dev->used, sizeof(dev->used));
        /* VirtualBox's early ARM timer is a software fallback.  Bound the
         * poll even if it is not advancing yet, so a failed controller does
         * not freeze the whole boot permanently. */
        if (timer_get_ticks() >= deadline || --spins == 0) {
            spin_unlock_irqrestore(&dev->lock, flags);
            kinfo("[VIRTIO-SCSI] request timeout\n");
            return -1;
        }
        arch_cpu_relax();
    }
    arch_dma_sync_for_cpu(&dev->resp, sizeof(dev->resp));
    if (bytes)
        arch_dma_sync_for_cpu(data, bytes);
    dev->last_used = dev->used.idx;
    int ok = dev->resp.response == VIRTIO_SCSI_S_OK && dev->resp.status == SCSI_STATUS_GOOD;
    if (!ok)
        kerr("[VIRTIO-SCSI] SCSI command %02x failed: response=%u status=%u sense=%u resid=%u\n",
             cdb[0], dev->resp.response, dev->resp.status, dev->resp.sense_len,
             dev->resp.resid);
    spin_unlock_irqrestore(&dev->lock, flags);
    return ok ? 0 : -1;
}

static int virtio_scsi_rw(block_dev_t *block, uint64_t lba, void *buf,
                          size_t sectors, int write) {
    virtio_scsi_dev_t *dev = (virtio_scsi_dev_t *)block->priv;
    if (!dev || !dev->ready || sectors == 0 || sectors > 0xffffU ||
        lba > 0xffffffffU || lba + sectors > dev->capacity)
        return -1;
    uint8_t cdb[VIRTIO_SCSI_CDB_SIZE] = { 0 };
    cdb[0] = write ? SCSI_CMD_WRITE10 : SCSI_CMD_READ10;
    put_be32(&cdb[2], (uint32_t)lba);
    cdb[7] = (uint8_t)(sectors >> 8);
    cdb[8] = (uint8_t)sectors;
    return virtio_scsi_command(dev, cdb, buf, sectors * 512U, !write);
}

static int virtio_scsi_read(block_dev_t *block, uint64_t lba, void *buf, size_t sectors) {
    return virtio_scsi_rw(block, lba, buf, sectors, 0);
}

static int virtio_scsi_write(block_dev_t *block, uint64_t lba, const void *buf, size_t sectors) {
    return virtio_scsi_rw(block, lba, (void *)buf, sectors, 1);
}

static int virtio_scsi_probe(device_t *pdev) {
    if (g_scsi_count >= VIRTIO_SCSI_MAX_DEVS)
        return -1;
    virtio_scsi_dev_t *dev = &g_scsi[g_scsi_count];
    memset(dev, 0, sizeof(*dev));
    spin_init(&dev->lock);
    if (pci_virtio_transport_init(pdev, VIRTIO_ID_SCSI, &dev->vt) != 0) {
        kerr("[VIRTIO-SCSI] PCI transport initialization failed\n");
        return -1;
    }

    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, 0);
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, status);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = dev->vt.read32(&dev->vt, VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(features_hi & VIRTIO_F_VERSION_1_BIT)) {
        kerr("[VIRTIO-SCSI] device lacks VIRTIO_F_VERSION_1\n");
        return -1;
    }
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_DRIVER_FEATURES, VIRTIO_F_VERSION_1_BIT);
    status |= VIRTIO_STATUS_FEATURES_OK;
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, status);
    if (!(dev->vt.read32(&dev->vt, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK)) {
        kerr("[VIRTIO-SCSI] device rejected feature negotiation\n");
        return -1;
    }
    if (virtio_scsi_setup_queues(dev) != 0)
        return -1;
    status |= VIRTIO_STATUS_DRIVER_OK;
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, status);

    uint8_t cdb[VIRTIO_SCSI_CDB_SIZE] = { 0 };
    uint8_t capacity[8] ALIGNED(64) = { 0 };
    cdb[0] = SCSI_CMD_TEST_UNIT_READY;
    if (virtio_scsi_command(dev, cdb, capacity, 0, 1) != 0)
        return -1;
    memset(cdb, 0, sizeof(cdb));
    cdb[0] = SCSI_CMD_READ_CAPACITY10;
    if (virtio_scsi_command(dev, cdb, capacity, sizeof(capacity), 1) != 0)
        return -1;
    dev->capacity = (uint64_t)be32(capacity) + 1U;
    if (!dev->capacity || be32(capacity + 4) != 512U)
        return -1;
    dev->block.read_sector = virtio_scsi_read;
    dev->block.write_sector = virtio_scsi_write;
    dev->block.capacity = dev->capacity;
    dev->block.sector_size = 512;
    dev->block.priv = dev;
    dev->slot = g_scsi_count;
    dev->ready = 1;
    pdev->drv_priv = dev;
    g_scsi_count++;
    kinfo("[VIRTIO-SCSI] disk ready: %lu sectors (%lu MiB)\n",
          (unsigned long)dev->capacity, (unsigned long)(dev->capacity / 2048U));
    return 0;
}

static int virtio_scsi_remove(device_t *pdev) {
    virtio_scsi_dev_t *dev = pdev ? pdev->drv_priv : NULL;
    if (!dev)
        return 0;
    dev->ready = 0;
    dev->vt.write32(&dev->vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    pdev->drv_priv = NULL;
    while (g_scsi_count > 0 && !g_scsi[g_scsi_count - 1].ready)
        g_scsi_count--;
    return 0;
}

static uint64_t virtio_scsi_class_capacity(device_t *dev) {
    virtio_scsi_dev_t *scsi = dev ? (virtio_scsi_dev_t *)dev->drv_priv : NULL;
    return scsi ? scsi->capacity : 0;
}

static int virtio_scsi_class_read(device_t *dev, uint64_t sector, void *buf, size_t count) {
    virtio_scsi_dev_t *scsi = dev ? (virtio_scsi_dev_t *)dev->drv_priv : NULL;
    return scsi ? virtio_scsi_read(&scsi->block, sector, buf, count) : -1;
}

static int virtio_scsi_class_write(device_t *dev, uint64_t sector, const void *buf, size_t count) {
    virtio_scsi_dev_t *scsi = dev ? (virtio_scsi_dev_t *)dev->drv_priv : NULL;
    return scsi ? virtio_scsi_write(&scsi->block, sector, buf, count) : -1;
}

static uint32_t virtio_scsi_class_sector_size(device_t *dev) { (void)dev; return 512; }
static const block_dev_ops_t virtio_scsi_ops = {
    .read = virtio_scsi_class_read, .write = virtio_scsi_class_write,
    .capacity = virtio_scsi_class_capacity, .sector_size = virtio_scsi_class_sector_size,
};

static const device_id_t virtio_scsi_ids[] = {
    { .vendor = 0x1AF4, .device = 0x1048,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1008, .subvendor = VENDOR_ANY, .subdevice = 8 },
    { 0 },
};

static driver_t virtio_scsi_driver = {
    .name = "virtio-scsi", .id_table = virtio_scsi_ids, .bus = &pci_bus,
    .probe = virtio_scsi_probe, .remove = virtio_scsi_remove,
    .class_ops = &virtio_scsi_ops,
    .class_type = DEV_CLASS_BLOCK,
};

DRIVER_REGISTER(virtio_scsi_driver);

block_dev_t *virtio_scsi_get_dev(int index) {
    if (index < 0 || index >= g_scsi_count || !g_scsi[index].ready)
        return NULL;
    return &g_scsi[index].block;
}

int virtio_scsi_ready(int index) {
    return virtio_scsi_get_dev(index) != NULL;
}
