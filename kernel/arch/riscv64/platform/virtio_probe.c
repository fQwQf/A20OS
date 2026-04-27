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

static int arch_virtio_probe_type(int device_type, int index, virtio_transport_t *vt) {
    int seen = 0;
    for (int slot = 0; slot < 8; slot++) {
        uintptr_t base = VIRTIO_BASE + (unsigned long)slot * 0x1000;

        uint32_t magic   = *(volatile uint32_t *)(base + VIRTIO_MMIO_MAGIC);
        uint32_t version = *(volatile uint32_t *)(base + VIRTIO_MMIO_VERSION);
        uint32_t dev_id  = *(volatile uint32_t *)(base + VIRTIO_MMIO_DEVICE_ID);

        if (magic != 0x74726976 || (version != 1 && version != 2))
            continue;
        if ((int)dev_id != device_type)
            continue;
        if (seen++ != index)
            continue;

        vt->read32 = mmio_read;
        vt->write32 = mmio_write;
        vt->priv = (void *)base;
        vt->legacy = (version == 1);
        return 0;
    }
    return -1;
}

int arch_virtio_blk_probe(int index, virtio_transport_t *vt) {
    return arch_virtio_probe_type(2, index, vt);
}

int arch_virtio_net_probe(int index, virtio_transport_t *vt) {
    return arch_virtio_probe_type(1, index, vt);
}
