/* SPDX-License-Identifier: Apache-2.0
 * Sunxi SD platform/class adapter. Board power/clock/pinmux preparation is
 * explicitly supplied by arch; a matching compatible alone is insufficient.
 */
#include "drivers/block/sunxi_mmc.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/sync.h"
#include "mm/slab.h"

typedef struct sunxi_mmc_binding {
    uintptr_t base;
    mutex_t lock;
    sunxi_mmc_host_t host;
    int hardware_stopped;
} sunxi_mmc_binding_t;

static uint32_t mmio_read(void *context, uint32_t offset) {
    sunxi_mmc_binding_t *binding = context;
    return readl((const volatile void *)(binding->base + offset));
}
static void mmio_write(void *context, uint32_t offset, uint32_t value) {
    sunxi_mmc_binding_t *binding = context;
    writel(value, (volatile void *)(binding->base + offset));
}
static uint64_t mmio_ticks(void *context) {
    (void)context;
    return clock_get_ticks();
}
static int stop_binding(void *context) {
    sunxi_mmc_binding_t *binding = context;
    __atomic_store_n(&binding->host.closing, 1, __ATOMIC_RELEASE);
    if (!mutex_trylock(&binding->lock))
        return -EBUSY;
    int rc = 0;
    /* Before the engine accepts its callbacks/config, no host was touched. */
    if (!binding->hardware_stopped && binding->host.io.write)
        rc = sunxi_mmc_stop(&binding->host);
    if (!rc)
        binding->hardware_stopped = 1;
    mutex_unlock(&binding->lock);
    return rc;
}
static int sunxi_probe(device_t *dev) {
    const sunxi_mmc_platform_t *platform = dev->plat_data;
    resource_t *resource = device_get_resource(dev, RES_MMIO, 0);
    if (!platform)
        return -ENODEV; /* electrical setup has not been supplied */
    if (!resource || resource->end < resource->start ||
        resource->end - resource->start < (uint64_t)platform->fifo_offset + 3)
        return -EINVAL;
    int rc = device_probe_use_remove(dev);
    if (rc)
        return rc;
    sunxi_mmc_binding_t *binding = kcalloc(1, sizeof(*binding));
    if (!binding)
        return -ENOMEM;
    mutex_init(&binding->lock);
    dev->drv_priv = binding;
    rc = device_add_execution_action(dev, stop_binding, NULL, binding);
    if (rc)
        return rc;
    binding->base = (uintptr_t)device_map_mmio(dev, 0);
    if (!binding->base)
        return -EIO;
    sunxi_mmc_io_t io = {
        .context = binding,
        .read = mmio_read,
        .write = mmio_write,
        .ticks = mmio_ticks,
        .ticks_per_second = clock_ticks_per_sec(),
    };
    rc = sunxi_mmc_start(&binding->host, &io, platform);
    if (rc) {
        kinfo("[SUNXI-MMC] %s: SD initialization failed (%d)\n", dev->name, rc);
        return rc;
    }
    int read_only = fw_node_has_property(&dev->firmware_node, "read-only");
    if (read_only < 0)
        return read_only;
    if (read_only)
        binding->host.write_protected = 1;
#ifdef CONFIG_STORAGE_READ_ONLY
    binding->host.write_protected = 1;
#endif
    kinfo("[SUNXI-MMC] %s: %lu sectors, %s, 1-bit PIO %u Hz%s\n", dev->name,
          (unsigned long)binding->host.sectors,
          binding->host.high_capacity ? "SDHC/SDXC" : "SDSC",
          platform->clock_hz,
          binding->host.write_protected ? ", read-only" : "");
    return 0;
}
static int sunxi_remove(device_t *dev) {
    sunxi_mmc_binding_t *binding = dev->drv_priv;
    if (!binding)
        return 0;
    int rc = stop_binding(binding);
    if (rc)
        return rc;
    dev->drv_priv = NULL;
    kfree(binding);
    return 0;
}
static int transfer(device_t *dev, uint64_t lba, void *buffer, size_t sectors,
                    int write) {
    sunxi_mmc_binding_t *binding = dev->drv_priv;
    if (!binding)
        return -ENODEV;
    mutex_lock(&binding->lock);
    int rc = __atomic_load_n(&binding->host.closing, __ATOMIC_ACQUIRE) ? -ENODEV :
             sunxi_mmc_transfer(&binding->host, lba, buffer, sectors, write);
    mutex_unlock(&binding->lock);
    return rc;
}
static int sunxi_read(device_t *dev, uint64_t lba, void *buffer,
                      size_t sectors) {
    return transfer(dev, lba, buffer, sectors, 0);
}
static int sunxi_write(device_t *dev, uint64_t lba, const void *buffer,
                       size_t sectors) {
    return transfer(dev, lba, (void *)buffer, sectors, 1);
}
static int sunxi_flush(device_t *dev) {
    sunxi_mmc_binding_t *binding = dev->drv_priv;
    if (!binding)
        return -ENODEV;
    mutex_lock(&binding->lock);
    int rc = __atomic_load_n(&binding->host.closing, __ATOMIC_ACQUIRE) ? -ENODEV :
             sunxi_mmc_flush(&binding->host);
    mutex_unlock(&binding->lock);
    return rc;
}
static uint64_t sunxi_capacity(device_t *dev) {
    sunxi_mmc_binding_t *binding = dev->drv_priv;
    return binding && !__atomic_load_n(&binding->host.closing, __ATOMIC_ACQUIRE) ?
           binding->host.sectors : 0;
}
static uint32_t sunxi_sector_size(device_t *dev) {
    (void)dev;
    return 512;
}
static block_dev_ops_t sunxi_block_ops = {
    .read = sunxi_read,
    .write = sunxi_write,
    .flush = sunxi_flush,
    .capacity = sunxi_capacity,
    .sector_size = sunxi_sector_size,
};
static const firmware_device_id_t sunxi_ids[] = {
    {.id = "allwinner,sun50i-a64-mmc"},
    {.id = "allwinner,sun7i-a20-mmc"},
    {0},
};
static driver_t sunxi_mmc_driver = {
    .name = "sunxi-mmc",
    .bus = &platform_bus,
    .firmware_ids = sunxi_ids,
    .irq_optional = 1, /* completion is polled, not routed through an IRQ */
    .probe = sunxi_probe,
    .remove = sunxi_remove,
    .class_ops = &sunxi_block_ops,
    .class_type = DEV_CLASS_BLOCK,
};
DRIVER_REGISTER(sunxi_mmc_driver);
