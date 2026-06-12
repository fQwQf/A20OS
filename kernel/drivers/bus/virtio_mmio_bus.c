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

void virtio_mmio_enumerate(uintptr_t base, int max_slots) {
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
        if (dev_idx >= 8) break;

        const char *type_name = "unknown";
        if (dev_id == VIRTIO_DEV_BLK) type_name = "blk";
        if (dev_id == VIRTIO_DEV_NET) type_name = "net";

        snprintf(dev_names[dev_idx], sizeof(dev_names[dev_idx]),
                 "virtio-%s%d", type_name, slot);

        static resource_t vdev_res[8];
        vdev_res[dev_idx].type  = RES_MMIO;
        vdev_res[dev_idx].start = slot_base;
        vdev_res[dev_idx].end   = slot_base + 0xFFF;
        vdev_res[dev_idx].flags = IORESOURCE_MMIO_32BIT;

        static device_t vdevs[8];
        device_t *dev       = &vdevs[dev_idx];
        dev->name            = dev_names[dev_idx];
        dev->bus             = &virtio_mmio_bus;
        dev->plat_data       = (void *)(uintptr_t)dev_id;
        dev->res             = &vdev_res[dev_idx];
        dev->res_count       = 1;
        dev->state           = DEV_STATE_UNINIT;

        device_register(dev);
        dev_idx++;
    }

    kinfo("[BUS] virtio-mmio: found %d devices (base=0x%lx)\n",
          dev_idx, (unsigned long)base);
}
