#include "drv/virtio_transport.h"
#include "drv/virtio_blk.h"
#include "platform.h"
#include "core/stdio.h"

static uint32_t mmio_read(virtio_transport_t *t, uint32_t off) {
    return *(volatile uint32_t *)((uintptr_t)t->priv + off);
}

static void mmio_write(virtio_transport_t *t, uint32_t off, uint32_t val) {
    *(volatile uint32_t *)((uintptr_t)t->priv + off) = val;
}

int arch_virtio_blk_probe(int index, virtio_transport_t *vt) {
    uintptr_t base = VIRTIO_BASE + (unsigned long)index * 0x1000;

    uint32_t magic   = *(volatile uint32_t *)(base + VIRTIO_MMIO_MAGIC);
    uint32_t version = *(volatile uint32_t *)(base + VIRTIO_MMIO_VERSION);
    uint32_t dev_id  = *(volatile uint32_t *)(base + VIRTIO_MMIO_DEVICE_ID);

    if (magic != 0x74726976) {
        printf("[VIRTIO%d] Bad magic: 0x%x\n", index, magic);
        return -1;
    }
    if (version != 1 && version != 2) {
        printf("[VIRTIO%d] Unsupported version: %d\n", index, version);
        return -1;
    }
    if (dev_id != 2) {
        printf("[VIRTIO%d] Not a block device: dev_id=%d\n", index, dev_id);
        return -1;
    }

    vt->read32 = mmio_read;
    vt->write32 = mmio_write;
    vt->priv = (void *)base;
    vt->legacy = (version == 1);
    return 0;
}
