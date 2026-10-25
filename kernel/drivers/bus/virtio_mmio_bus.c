/* VirtIO MMIO transport discovery. Firmware describes individual transports;
 * static machine presets may explicitly scan a physical slot window. Both
 * paths publish the same bus devices and CPU-physical resource records. */
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/virtio_mmio_hal.h"
#include "drivers/bus/platform_bus.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "mm/mm.h"

typedef struct virtio_mmio_device {
    device_t device;
    resource_t resources[2];
    char name[64];
} virtio_mmio_device_t;

static int virtio_mmio_match(device_t *dev, const driver_t *drv) {
    if ((drv->bus && drv->bus != dev->bus) || !drv->id_table)
        return 0;
    uint32_t type = (uint32_t)(uintptr_t)dev->plat_data;
    for (const device_id_t *id = drv->id_table; id->vendor || id->device; id++)
        if ((id->vendor == VENDOR_ANY || id->vendor == 0) &&
            (id->device == DEVICE_ANY || id->device == type)) {
            dev->matched_id = id;
            return 1;
        }
    return 0;
}
static bus_type_t virtio_mmio_bus = {.name = "virtio-mmio",
                                     .match = virtio_mmio_match};
bus_type_t *get_virtio_mmio_bus(void) {
    return &virtio_mmio_bus;
}
static void transport_release(device_t *dev) {
    kfree((virtio_mmio_device_t *)dev);
}
static void remove_transport(void *data) {
    device_t *dev = data;
    device_unregister(dev);
    device_put(dev);
}
static int register_transport(uint64_t physical, size_t size, int irq,
                              uint32_t irq_flags, device_t *parent,
                              device_t **out) {
    if (out)
        *out = NULL;
    if (size < 0x100 || physical > (uint64_t)-1 - (size - 1))
        return -EINVAL;
    if (irq >= 0 && (unsigned)irq >= PLATFORM_IRQ_MAX_LINES)
        return -ERANGE;
    extern int udriver_mmio_user_owned(uint64_t phys);
    if (udriver_mmio_user_owned(physical))
        return 0;
    void *mapping = arch_device_map_mmio(physical, size, 0);
    if (!mapping)
        return -EFAULT;
    uintptr_t base = (uintptr_t)mapping;
    uint32_t magic = readl((const volatile void *)base);
    uint32_t version = readl((const volatile void *)(base + 4));
    uint32_t type = readl((const volatile void *)(base + 8));
    arch_device_unmap_mmio(mapping, size);
    if (magic != 0x74726976U || (version != 1 && version != 2))
        return -ENODEV;
    if (!type)
        return 0;
    virtio_mmio_device_t *transport = kcalloc(1, sizeof(*transport));
    if (!transport)
        return -ENOMEM;
    snprintf(transport->name, sizeof(transport->name), "virtio-%u@%llx", type,
             (unsigned long long)physical);
    transport->resources[0] = (resource_t){.type = RES_MMIO,
                                           .start = physical,
                                           .end = physical + size - 1,
                                           .flags = IORESOURCE_MMIO_32BIT};
    if (irq >= 0)
        transport->resources[1] = (resource_t){.type = RES_IRQ,
                                               .start = (uint32_t)irq,
                                               .end = (uint32_t)irq,
                                               .flags = irq_flags};
    device_t *dev = &transport->device;
    dev->name = transport->name;
    dev->bus = &virtio_mmio_bus;
    dev->parent = parent;
    dev->res = transport->resources;
    dev->res_count = irq >= 0 ? 2 : 1;
    dev->plat_data = (void *)(uintptr_t)type;
    dev->release = transport_release;
    int rc = bus_register(&virtio_mmio_bus);
    if (rc && rc != -EEXIST) {
        kfree(transport);
        return rc;
    }
    rc = device_register(dev);
    if (rc) {
        kfree(transport);
        return rc;
    }
    if (out)
        *out = dev;
    return 1;
}
static int firmware_transport_probe(device_t *dev) {
    resource_t *memory = device_get_resource(dev, RES_MMIO, 0);
    if (!memory || memory->end < memory->start ||
        memory->end - memory->start >= SIZE_MAX)
        return -EINVAL;
    int irq = platform_device_irq(dev);
    if (irq < 0 && irq != -ENODEV)
        return irq;
    device_t *child = NULL;
    resource_t *irq_resource = device_get_resource(dev, RES_IRQ, 0);
    int rc = register_transport(
        memory->start, (size_t)(memory->end - memory->start + 1), irq,
        irq_resource ? irq_resource->flags : 0, dev, &child);
    if (rc < 0)
        return rc;
    if (!child)
        return 0; /* A valid empty VirtIO slot has DeviceID == 0. */
    rc = device_add_resource_action(dev, remove_transport, child);
    if (rc)
        remove_transport(child);
    return rc;
}
static const firmware_device_id_t firmware_ids[] = {
    {.id = "virtio,mmio"}, {.id = "LNRO0005"}, {0}};
static driver_t firmware_transport = {.name = "virtio-mmio-transport",
                                      .bus = &platform_bus,
                                      .firmware_ids = firmware_ids,
                                      .probe = firmware_transport_probe};
DRIVER_REGISTER(firmware_transport);

void virtio_mmio_enumerate(uintptr_t physical, int max_slots, int irq_base) {
    size_t slot_size = arch_virtio_mmio_slot_size();
    if (!slot_size || max_slots < 0 || irq_base < 0)
        return;
    int count = 0;
    for (int i = 0; i < max_slots; i++) {
        if ((uint64_t)i > ((uint64_t)-1 - physical) / slot_size)
            break;
        if ((unsigned)i > (unsigned)0x7fffffffU - (unsigned)irq_base)
            break;
        int rc = register_transport(
            (uint64_t)physical + (uint64_t)i * slot_size, slot_size,
            irq_base + i, IORESOURCE_IRQ_LEVEL, NULL, NULL);
        if (rc == -ENODEV)
            continue;
        if (rc < 0) {
            kerr("[BUS] virtio-mmio slot %d discovery failed: %d\n", i, rc);
            break;
        }
        count += rc;
    }
    kinfo("[BUS] virtio-mmio: found %d devices (physical=0x%lx irq_base=%d)\n",
          count, (unsigned long)physical, irq_base);
}
