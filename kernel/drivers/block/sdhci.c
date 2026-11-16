/* SPDX-License-Identifier: Apache-2.0 */
#include "drivers/block/sdhci.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "mm/slab.h"

typedef struct {
    sdhci_host_t host;
    void *base;
    device_t *device;
    const sdhci_platform_data_t *platform;
    mutex_t lock;
    int prepared;
    int hardware_stopped;
} sdhci_device_t;

static uint32_t mmio_read(void *opaque, unsigned offset, unsigned width) {
    sdhci_device_t *p = opaque;
    const volatile void *address = (volatile unsigned char *)p->base + offset;
    return width == 1   ? readb(address)
           : width == 2 ? readw(address)
                        : readl(address);
}
static void mmio_write(void *opaque, unsigned offset, unsigned width,
                       uint32_t value) {
    sdhci_device_t *p = opaque;
    volatile void *address = (volatile unsigned char *)p->base + offset;
    if (width == 1)
        writeb((uint8_t)value, address);
    else if (width == 2)
        writew((uint16_t)value, address);
    else
        writel(value, address);
}
static uint64_t time_ms(void *opaque) {
    (void)opaque;
    uint64_t ticks = clock_get_ticks(), hz = clock_ticks_per_sec();
    return ticks / hz * 1000 + ticks % hz * 1000 / hz;
}
static void delay_us(void *opaque, unsigned usecs) {
    (void)opaque;
    udelay(usecs);
}
static int before_idle(void *opaque) {
    sdhci_device_t *p = opaque;
    return p->platform->before_idle
               ? p->platform->before_idle(p->device, p->base)
               : 0;
}
static int sdhci_stop(void *opaque) {
    sdhci_device_t *p = opaque;
    __atomic_store_n(&p->host.closing, 1, __ATOMIC_RELEASE);
    if (!mutex_trylock(&p->lock))
        return -EBUSY;
    int rc = 0;
    /* The protocol has not touched a host whose clocks/pins were not prepared.
     * In particular, do not access an unmapped or unclocked host on rollback. */
    if (p->prepared && !p->hardware_stopped)
        rc = sdhci_card_stop(&p->host);
    if (!rc)
        p->hardware_stopped = 1;
    mutex_unlock(&p->lock);
    return rc;
}

static int sdhci_probe(device_t *dev) {
    const sdhci_platform_data_t *platform = dev->plat_data;
    resource_t *resource = device_get_resource(dev, RES_MMIO, 0);
    if (!platform || !platform->prepare || !platform->input_hz ||
        !platform->max_hz)
        return -ENODEV;
    if (!resource || resource->end < resource->start ||
        resource->end - resource->start < 0xff)
        return -EINVAL;
    int rc = device_probe_use_remove(dev);
    if (rc)
        return rc;
    sdhci_device_t *p = kmalloc(sizeof(*p));
    if (!p)
        return -ENOMEM;
    memset(p, 0, sizeof(*p));
    p->device = dev;
    p->platform = platform;
    mutex_init(&p->lock);
    dev->drv_priv = p;
    rc = device_add_execution_action(dev, sdhci_stop, NULL, p);
    if (rc)
        return rc;
    p->base = device_map_mmio(dev, 0);
    if (!p->base)
        return -ENOMEM;
    p->host = (sdhci_host_t){
        .io = {p, mmio_read, mmio_write, time_ms, delay_us},
        .input_hz = platform->input_hz,
        .max_hz = platform->max_hz,
        .spec_version = platform->spec_version,
        .before_idle = before_idle,
        .vendor_context = p,
    };
    rc = platform->prepare(dev, p->base);
    p->prepared = !rc;
    if (p->prepared)
        rc = sdhci_card_initialize(&p->host);
    if (rc) {
        kwarn("[SDHCI] %s: SD initialization failed: %d\n", dev->name, rc);
        return rc;
    }
    int read_only = fw_node_has_property(&dev->firmware_node, "read-only");
    if (read_only < 0)
        return read_only;
    if (read_only)
        p->host.write_protected = 1;
#ifdef CONFIG_STORAGE_READ_ONLY
    p->host.write_protected = 1;
#endif
    kinfo("[SDHCI] %s: %lu sectors, %u Hz, 1-bit PIO (no DMA)%s\n", dev->name,
          (unsigned long)p->host.sectors, p->host.actual_hz,
          p->host.write_protected ? ", read-only" : "");
    return 0;
}
static int sdhci_remove(device_t *dev) {
    sdhci_device_t *p = dev->drv_priv;
    if (p) {
        int rc = sdhci_stop(p);
        if (rc)
            return rc;
        dev->drv_priv = NULL;
        kfree(p);
    }
    return 0;
}
static int block_transfer(device_t *dev, uint64_t lba, void *buf, size_t count,
                          int write) {
    sdhci_device_t *p = dev->drv_priv;
    if (!p)
        return -ENODEV;
    mutex_lock(&p->lock);
    int rc = __atomic_load_n(&p->host.closing, __ATOMIC_ACQUIRE) ? -ENODEV :
             sdhci_card_transfer(&p->host, lba, buf, count, write);
    mutex_unlock(&p->lock);
    return rc;
}
static int block_read(device_t *dev, uint64_t lba, void *buf, size_t count) {
    return block_transfer(dev, lba, buf, count, 0);
}
static int block_write(device_t *dev, uint64_t lba, const void *buf,
                       size_t count) {
    return block_transfer(dev, lba, (void *)buf, count, 1);
}
static uint64_t block_capacity(device_t *dev) {
    sdhci_device_t *p = dev->drv_priv;
    return p && !__atomic_load_n(&p->host.closing, __ATOMIC_ACQUIRE) ?
           p->host.sectors : 0;
}
static uint32_t block_sector_size(device_t *dev) {
    (void)dev;
    return 512;
}
static const block_dev_ops_t sdhci_block_ops = {
    .read = block_read,
    .write = block_write,
    .capacity = block_capacity,
    .sector_size = block_sector_size,
};
static const firmware_device_id_t sdhci_ids[] = {
    {.id = "sophgo,cv1800b-dwcmshc"},
    {.id = "sophgo,sg2002-dwcmshc"},
    {.id = "bflb,bl808-sdhci"},
    {0},
};
static driver_t sdhci_driver = {
    .name = "sdhci",
    .bus = &platform_bus,
    .firmware_ids = sdhci_ids,
    .probe = sdhci_probe,
    .remove = sdhci_remove,
    .class_ops = &sdhci_block_ops,
    .class_type = DEV_CLASS_BLOCK,
    .irq_optional = 1,
};
DRIVER_REGISTER(sdhci_driver);
