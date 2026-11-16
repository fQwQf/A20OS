/* Firmware-retained linear scanout. No modesetting, GPU command submission or
 * firmware calls occur here. Ownership starts only after ExitBootServices. */
#include "boot/context.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/gpu/gpu_core.h"
#include "firmware/static.h"
#include "firmware/device.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/bootargs.h"

static fw_resource_t scanout_resource;
static const char compatible[] = "a20,boot-framebuffer";
static const fw_static_property_t properties[] = {
    {"compatible", {compatible, sizeof(compatible), FW_ORDER_NATIVE}},
};
static fw_static_node_t nodes[] = {
    {.name = "boot-display", .parent = SIZE_MAX},
    {.name = "framebuffer",
     .parent = 0,
     .properties = properties,
     .property_count = 1,
     .resources = &scanout_resource,
     .resource_count = 1},
};
static const fw_static_tree_t tree = {nodes, 2};
int boot_framebuffer_enumerate(void) {
    const boot_framebuffer_t *fb = &boot_context_get()->framebuffer;
    if (!fb->magic)
        return 0;
    int rc = boot_framebuffer_validate(fb);
    if (rc)
        return rc;
    const fw_node_t *selected = &boot_context_get()->framebuffer_node;
    if (fw_node_valid(selected)) {
        device_t *device = firmware_device_find(selected);
        if (!device) return -ENODEV;
        device_put(device);
        return 0;
    }
    scanout_resource = (fw_resource_t){FW_RESOURCE_MEMORY, fb->physical,
                                       fb->aperture_bytes, 0, "scanout"};
    fw_node_t root;
    rc = fw_static_init(&tree, &root);
    return rc ? rc : firmware_device_enumerate(&root);
}
static int info(device_t *dev, uint32_t *w, uint32_t *h, uint32_t *bpp) {
    const boot_framebuffer_t *fb = dev->plat_data;
    *w = fb->width;
    *h = fb->height;
    *bpp = fb->bits_per_pixel;
    return 0;
}
static int framebuffer(device_t *dev, uintptr_t *physical, size_t *size) {
    const boot_framebuffer_t *fb = dev->plat_data;
    *physical = (uintptr_t)fb->physical;
    *size = ((size_t)fb->stride * fb->height + 4095) & ~(size_t)4095;
    return 0;
}
static int layout(device_t *dev, gpu_scanout_layout_t *out) {
    const boot_framebuffer_t *fb = dev->plat_data;
    *out = (gpu_scanout_layout_t){fb->stride, fb->red_mask, fb->green_mask,
                                  fb->blue_mask, fb->reserved_mask};
    return 0;
}
static int flush(device_t *dev, uint32_t x, uint32_t y, uint32_t w,
                 uint32_t h) {
    const boot_framebuffer_t *fb = dev->plat_data;
    if (x > fb->width || y > fb->height || w > fb->width - x ||
        h > fb->height - y) return -EINVAL;
    return boot_framebuffer_flush();
}
static int probe(device_t *dev) {
    const boot_context_t *boot = boot_context_get();
    const boot_framebuffer_t *fb = &boot->framebuffer;
    if (fw_node_valid(&boot->framebuffer_node) &&
        !fw_node_equal(&dev->firmware_node, &boot->framebuffer_node))
        return -ENODEV;
    int rc = boot_framebuffer_validate(fb);
    if (rc)
        return rc;
    if (fb->physical > (uintptr_t)-1 || fb->aperture_bytes > (size_t)-1)
        return -EOVERFLOW;
    rc = gpu_device_physical_mapping_init(dev);
    if (rc) return rc;
    if (fw_node_valid(&boot->framebuffer_node)) {
        rc = firmware_device_retain_resources(dev);
        if (rc) return rc;
    }
    void *mapped = boot_framebuffer_map();
    if (!mapped) {
        printf("[GPU] firmware framebuffer: no safe device mapping for 0x%lx "
               "(%lu bytes)\n",
               (unsigned long)fb->physical, (unsigned long)fb->aperture_bytes);
        return -ENXIO;
    }
    dev->plat_data = (void *)fb;
    dev->drv_priv = mapped;
    rc = gpu_device_register(dev);
    if (rc)
        return rc;
    printf("[GPU] firmware framebuffer: %ux%ux%u pitch=%u physical=0x%lx\n",
           fb->width, fb->height, fb->bits_per_pixel, fb->stride,
           (unsigned long)fb->physical);
    /* Explicit optical test; normally preserve the firmware's existing image. */
    const char *args = bootargs_get();
    if (args && strstr(args, "simplefb.test=1")) {
        uint32_t masks[] = {fb->red_mask, fb->green_mask, fb->blue_mask};
        unsigned bytes = fb->bits_per_pixel / 8;
        for (uint32_t y = 0; y < fb->height; y++)
            for (uint32_t x = 0; x < fb->width; x++) {
                uint32_t pixel = masks[(uint64_t)x * 3 / fb->width];
                volatile uint8_t *p = (volatile uint8_t *)mapped +
                                      (size_t)y * fb->stride + x * bytes;
                for (unsigned n = 0; n < bytes; n++)
                    p[n] = pixel >> (8 * n);
            }
        if (boot_framebuffer_flush()) {
            gpu_device_unregister(dev);
            dev->plat_data = NULL; dev->drv_priv = NULL;
            return -EIO;
        }
        printf("[GPU] firmware framebuffer colour bars rendered\n");
    }
    return 0;
}
static int remove_device(device_t *dev) {
    gpu_device_unregister(dev);
    dev->plat_data = NULL;
    dev->drv_priv = NULL;
    return 0;
}
static const firmware_device_id_t ids[] = {{.id = "a20,boot-framebuffer"},
                                            {.id = "simple-framebuffer"}, {0}};
static int simplefb_get_capabilities(device_t *dev, struct gpu_capabilities *out) {
    if (!out) return -EINVAL;
    *out = (struct gpu_capabilities){0};
    if (!dev || !dev->drv_priv) return -ENODEV;
    out->flags = GPU_CAP_SCANOUT | GPU_CAP_BOOT_SCANOUT;
    return 0;
}

static const gpu_dev_ops_t ops = {
    .get_capabilities = simplefb_get_capabilities,
    .get_info = info,
    .get_fb = framebuffer,
    .flush = flush,
    .get_layout = layout,
};
static driver_t simplefb_driver = {
    .name = "simplefb",
    .bus = &platform_bus,
    .firmware_ids = ids,
    .probe = probe,
    .remove = remove_device,
    .class_type = DEV_CLASS_DISPLAY,
    .class_ops = &ops,
};
DRIVER_REGISTER(simplefb_driver);
