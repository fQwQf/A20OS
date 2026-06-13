/*
 * VirtIO MMIO Bus — device enumeration for memory-mapped VirtIO
 *
 * Scans a configurable number of MMIO slots, creates device_t
 * for each valid VirtIO device found.
 */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/defs.h"
#include "core/stdio.h"
#include "core/klog.h"

#define VIRTIO_MMIO_MAGIC_VALUE  0x74726976
#define VIRTIO_VENDOR_ID         0x00000000

#define VIRTIO_DEV_BLK           2
#define VIRTIO_DEV_NET           1

/* DRIVER_ENUMERATION_FAILURE_MODEL: virtio-mmio registers only discovered
 * static slots; driver_core owns probe failure cleanup and leaves devices
 * unbound when a matching driver rejects a slot. */

typedef struct virtio_mmio_bus_data {
    uintptr_t base;
    int       max_slots;
} virtio_mmio_bus_data_t;

static virtio_mmio_bus_data_t g_virtio_mmio_data;

static int virtio_mmio_match(device_t *dev, const driver_t *drv) {
    if (drv->bus != dev->bus)
        return 0;
    if (!drv->id_table)
        return 0;

    uint32_t dev_vendor = (uint32_t)(uintptr_t)dev->plat_data;

    for (const device_id_t *id = drv->id_table; id->vendor != 0 || id->device != 0; id++) {
        if ((id->vendor == VENDOR_ANY || id->vendor == VIRTIO_VENDOR_ID) &&
            (id->device == DEVICE_ANY || id->device == dev_vendor))
            return 1;
    }
    return 0;
}

static bus_type_t virtio_mmio_bus = {
    .name  = "virtio-mmio",
    .match = virtio_mmio_match,
};

bus_type_t *get_virtio_mmio_bus(void) {
    return &virtio_mmio_bus;
}

/* VIRTIO_MMIO_IRQ_MODEL:
 * - Each MMIO slot is assigned a platform IRQ line relative to a base.
 * - The base is passed by the board because it is platform-specific
 *   (e.g. RISC-V QEMU virt starts at IRQ 1, AArch64 QEMU virt at IRQ 16).
 * - Drivers retrieve the IRQ through device_get_resource(dev, RES_IRQ, 0).
 */
void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base) {
    g_virtio_mmio_data.base      = base;
    g_virtio_mmio_data.max_slots = max_slots;

    bus_register(&virtio_mmio_bus);

    int dev_idx = 0;
    for (int slot = 0; slot < max_slots; slot++) {
        uintptr_t slot_base = base + (unsigned long)slot * 0x1000;
        uint32_t magic   = readl((const volatile void *)slot_base);
        uint32_t version = readl((const volatile void *)(slot_base + 0x004));
        uint32_t dev_id  = readl((const volatile void *)(slot_base + 0x008));

        if (magic != VIRTIO_MMIO_MAGIC_VALUE)
            continue;
        if (version != 1 && version != 2)
            continue;
        if (dev_id == 0)
            continue;

        static char dev_names[8][32];
        static resource_t vdev_res[8][2];
        static device_t vdevs[8];
        if (dev_idx >= 8) break;

        const char *type_name = "unknown";
        if (dev_id == VIRTIO_DEV_BLK) type_name = "blk";
        if (dev_id == VIRTIO_DEV_NET) type_name = "net";

        snprintf(dev_names[dev_idx], sizeof(dev_names[dev_idx]),
                 "virtio-%s%d", type_name, slot);

        resource_t *res = vdev_res[dev_idx];
        res[0].type  = RES_MMIO;
        res[0].start = slot_base;
        res[0].end   = slot_base + 0xFFF;
        res[0].flags = IORESOURCE_MMIO_32BIT;

        res[1].type  = RES_IRQ;
        res[1].start = (uint64_t)(irq_base + slot);
        res[1].end   = res[1].start;
        res[1].flags = IORESOURCE_IRQ_LEVEL;

        device_t *dev       = &vdevs[dev_idx];
        dev->name            = dev_names[dev_idx];
        dev->bus             = &virtio_mmio_bus;
        dev->plat_data       = (void *)(uintptr_t)dev_id;
        dev->res             = res;
        dev->res_count       = 2;
        dev->state           = DEV_STATE_UNINIT;

        device_register(dev);
        dev_idx++;
    }

    kinfo("[BUS] virtio-mmio: found %d devices (base=0x%lx irq_base=%d)\n",
          dev_idx, (unsigned long)base, irq_base);
}
