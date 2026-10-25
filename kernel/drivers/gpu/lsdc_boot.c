/* SPDX-License-Identifier: Apache-2.0
 * Retain an already configured LS2K0300 DVO/LCD plane. No writes to DC, PLL,
 * panel or clock registers, no framebuffer allocation and no initial clear.
 */
#include "drivers/gpu/gpu_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/platform_bus.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"
#include "mm/slab.h"
#include "lsdc_boot.h"

struct lsdc_boot {
    volatile uint8_t *registers;
    void *scanout;
    lsdc_boot_snapshot_t snapshot;
    lsdc_boot_mode_t mode;
};

static void read_snapshot(struct lsdc_boot *s, lsdc_boot_snapshot_t *out)
{
    *out = (lsdc_boot_snapshot_t){0};
    out->config = readl(s->registers + LSDC_BOOT_CFG);
    out->origin = readl(s->registers + LSDC_BOOT_ORIGIN);
    out->stride = readl(s->registers + LSDC_BOOT_STRIDE);
    out->horizontal = readl(s->registers + LSDC_BOOT_HDISPLAY);
    out->vertical = readl(s->registers + LSDC_BOOT_VDISPLAY);
    out->low[0] = readl(s->registers + LSDC_BOOT_ADDR0);
    out->high[0] = readl(s->registers + LSDC_BOOT_HI0);
    out->low[1] = readl(s->registers + LSDC_BOOT_ADDR1);
    out->high[1] = readl(s->registers + LSDC_BOOT_HI1);
}

static int current(device_t *dev)
{
    struct lsdc_boot *s = dev->drv_priv;
    lsdc_boot_snapshot_t a, b;
    if (!s) return -ENODEV;
    read_snapshot(s, &a);
    read_snapshot(s, &b);
    return memcmp(&a, &s->snapshot, sizeof(a)) || memcmp(&a, &b, sizeof(a))
               ? -ENODEV : 0;
}
static int info(device_t *dev, uint32_t *w, uint32_t *h, uint32_t *bpp)
{
    int rc = current(dev);
    if (rc) return rc;
    const struct lsdc_boot *s = dev->drv_priv;
    *w = s->mode.width; *h = s->mode.height; *bpp = s->mode.bpp;
    return 0;
}
static int framebuffer(device_t *dev, uintptr_t *physical, size_t *bytes)
{
    int rc = current(dev);
    if (rc) return rc;
    const struct lsdc_boot *s = dev->drv_priv;
    *physical = (uintptr_t)s->mode.physical;
    *bytes = (size_t)s->mode.bytes;
    return 0;
}
static int layout(device_t *dev, gpu_scanout_layout_t *out)
{
    int rc = current(dev);
    if (rc) return rc;
    const struct lsdc_boot *s = dev->drv_priv;
    *out = (gpu_scanout_layout_t){s->mode.stride, s->mode.red, s->mode.green,
                                  s->mode.blue, s->mode.reserved};
    return 0;
}
static int flush(device_t *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    int rc = current(dev);
    if (rc) return rc;
    const struct lsdc_boot *s = dev->drv_priv;
    if (x > s->mode.width || y > s->mode.height || w > s->mode.width - x ||
        h > s->mode.height - y) return -EINVAL;
    /* Architecture maps this borrowed aperture uncached; fbdev's default
     * device mapping has the same attribute. No WB alias is introduced. */
    wmb();
    return 0;
}
static int probe(device_t *dev)
{
    uint64_t aperture[2];
    resource_t *res = device_get_resource(dev, RES_MMIO, 0);
    if (!res || res->end < res->start ||
        res->end - res->start < LSDC_BOOT_REG_BYTES - 1)
        return -ENODEV;
    const lsdc_boot_policy_t *policy = dev->plat_data;
    if (policy) {
        if (policy->magic != LSDC_BOOT_POLICY_MAGIC ||
            policy->version != LSDC_BOOT_POLICY_VERSION ||
            policy->register_base != res->start)
            return -EINVAL;
        aperture[0] = policy->aperture_base;
        aperture[1] = policy->aperture_bytes;
    } else if (fw_node_read_u64_array(&dev->firmware_node,
                                      "a20,scanout-aperture", aperture, 2))
        return -ENODEV;
    struct lsdc_boot *s = kcalloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    s->registers = arch_device_map_mmio(res->start, LSDC_BOOT_REG_BYTES, res->flags);
    if (!s->registers) { kfree(s); return -ENXIO; }
    lsdc_boot_snapshot_t verify;
    read_snapshot(s, &s->snapshot);
    read_snapshot(s, &verify);
    if (memcmp(&s->snapshot, &verify, sizeof(verify)) ||
        !lsdc_boot_decode(&verify, aperture[0], aperture[1], &s->mode)) {
        arch_device_unmap_mmio((void *)s->registers, LSDC_BOOT_REG_BYTES);
        kfree(s);
        printf("[GPU] lsdc boot: no stable supported firmware scanout\n");
        return -ENODEV;
    }
    s->scanout = arch_device_map_mmio(s->mode.physical, (size_t)s->mode.bytes, 0);
    if (!s->scanout) {
        arch_device_unmap_mmio((void *)s->registers, LSDC_BOOT_REG_BYTES);
        kfree(s); return -ENXIO;
    }
    dev->drv_priv = s;
    int rc = gpu_device_register(dev);
    if (rc) {
        dev->drv_priv = NULL;
        arch_device_unmap_mmio(s->scanout, (size_t)s->mode.bytes);
        arch_device_unmap_mmio((void *)s->registers, LSDC_BOOT_REG_BYTES);
        kfree(s); return rc;
    }
    printf("[GPU] lsdc boot: %ux%ux%u pitch=%u physical=0x%lx (firmware retained)\n",
           s->mode.width, s->mode.height, s->mode.bpp, s->mode.stride,
           (unsigned long)s->mode.physical);
    return 0;
}
static int remove_device(device_t *dev)
{
    struct lsdc_boot *s = dev->drv_priv;
    if (!s) return 0;
    gpu_device_unregister(dev);
    dev->drv_priv = NULL;
    arch_device_unmap_mmio(s->scanout, (size_t)s->mode.bytes);
    arch_device_unmap_mmio((void *)s->registers, LSDC_BOOT_REG_BYTES);
    kfree(s); /* Borrowed VRAM and the running display are never released. */
    return 0;
}
static const firmware_device_id_t ids[] = {
    {.id="a20,ls2k0300-boot-display"},
    {.id="loongson,la2k0300-dc"}, {0}
};
static const gpu_dev_ops_t ops = {
    .get_info=info, .get_fb=framebuffer, .get_layout=layout, .flush=flush,
};
static driver_t lsdc_boot_driver = {
    .name="lsdc-boot", .bus=&platform_bus, .firmware_ids=ids,
    .probe=probe, .remove=remove_device, .class_type=DEV_CLASS_DISPLAY,
    .class_ops=&ops,
};
DRIVER_REGISTER(lsdc_boot_driver);
