#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/gpu/gpu_core.h"
#include "core/klog.h"

#define VMSVGA_VENDOR_ID       0x15ADU
#define VMSVGA_DEVICE_ID       0x0405U
#define VMSVGA_SUBSYSTEM_ID    0x0405U

#define SVGA_REG_ID            0U
#define SVGA_REG_ENABLE        2U
#define SVGA_REG_WIDTH         3U
#define SVGA_REG_HEIGHT        4U
#define SVGA_REG_BITS_PER_PIXEL 7U
#define SVGA_REG_FB_START      13U
#define SVGA_REG_FB_SIZE       14U
#define SVGA_REG_CONFIG_DONE   20U

#define SVGA_ID_2              0x90000002U

typedef struct vmsvga_device {
    uintptr_t regs;
    uintptr_t fifo;
    uintptr_t fb_virt;
    uintptr_t fb_phys;
    size_t fb_size;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
} vmsvga_device_t;

static vmsvga_device_t g_vmsvga;

static void vmsvga_write_reg(vmsvga_device_t *svga, uint32_t index, uint32_t value) {
    writel(index, (volatile void *)svga->regs);
    writel(value, (volatile void *)(svga->regs + 4U));
}

static uint32_t vmsvga_read_reg(vmsvga_device_t *svga, uint32_t index) {
    writel(index, (volatile void *)svga->regs);
    return readl((const volatile void *)(svga->regs + 4U));
}

static int vmsvga_get_info(device_t *dev, uint32_t *width, uint32_t *height,
                           uint32_t *bpp) {
    vmsvga_device_t *svga = dev ? dev->drv_priv : NULL;
    if (!svga || !width || !height || !bpp)
        return -1;

    *width = svga->width;
    *height = svga->height;
    *bpp = svga->bpp;
    return 0;
}

static int vmsvga_get_fb(device_t *dev, uintptr_t *fb_paddr, size_t *fb_size) {
    vmsvga_device_t *svga = dev ? dev->drv_priv : NULL;
    if (!svga || !fb_paddr || !fb_size)
        return -1;

    *fb_paddr = svga->fb_phys;
    *fb_size = svga->fb_size;
    return 0;
}

static int vmsvga_flush(device_t *dev, uint32_t x, uint32_t y, uint32_t w,
                         uint32_t h) {
    (void)dev;
    (void)x;
    (void)y;
    (void)w;
    (void)h;
    /* The legacy linear framebuffer is scanout directly by the device. */
    return 0;
}

static int vmsvga_ioctl(device_t *dev, unsigned long req, void *arg) {
    (void)dev;
    (void)req;
    (void)arg;
    return -1;
}

static const gpu_dev_ops_t vmsvga_ops = {
    .get_info = vmsvga_get_info,
    .get_fb = vmsvga_get_fb,
    .flush = vmsvga_flush,
    .ioctl = vmsvga_ioctl,
};

static int vmsvga_probe(device_t *dev) {
    if (pci_enable_and_assign_bars(dev) < 0)
        return -1;

    resource_t *regs = device_get_resource(dev, RES_MMIO, 0);
    resource_t *fb = device_get_resource(dev, RES_MMIO, 1);
    if (!regs || !fb || fb->end < fb->start)
        return -1;

    vmsvga_device_t *svga = &g_vmsvga;
    svga->regs = (uintptr_t)regs->start;
    svga->fifo = svga->regs;
    svga->fb_virt = (uintptr_t)fb->start;
    svga->fb_size = (size_t)(fb->end - fb->start + 1U);

    vmsvga_write_reg(svga, SVGA_REG_ID, SVGA_ID_2);
    if (vmsvga_read_reg(svga, SVGA_REG_ID) != SVGA_ID_2)
        return -1;

    vmsvga_write_reg(svga, SVGA_REG_ENABLE, 0);
    vmsvga_write_reg(svga, SVGA_REG_WIDTH, 1024U);
    vmsvga_write_reg(svga, SVGA_REG_HEIGHT, 768U);
    vmsvga_write_reg(svga, SVGA_REG_BITS_PER_PIXEL, 32U);
    vmsvga_write_reg(svga, SVGA_REG_ENABLE, 1U);
    vmsvga_write_reg(svga, SVGA_REG_CONFIG_DONE, 1U);

    svga->width = vmsvga_read_reg(svga, SVGA_REG_WIDTH);
    svga->height = vmsvga_read_reg(svga, SVGA_REG_HEIGHT);
    svga->bpp = vmsvga_read_reg(svga, SVGA_REG_BITS_PER_PIXEL);
    svga->fb_phys = (uintptr_t)vmsvga_read_reg(svga, SVGA_REG_FB_START);
    size_t device_fb_size = (size_t)vmsvga_read_reg(svga, SVGA_REG_FB_SIZE);
    if (!svga->width || !svga->height || svga->bpp != 32U || !svga->fb_phys ||
        !device_fb_size)
        return -1;
    if (device_fb_size != svga->fb_size)
        return -1;
    if (svga->fb_size < (size_t)svga->width * svga->height * (svga->bpp / 8U))
        return -1;

    dev->drv_priv = svga;
    if (gpu_device_register(dev) < 0) {
        dev->drv_priv = NULL;
        return -1;
    }

    kinfo("[GPU] vmsvga ready: %ux%ux%u (FB: %lu MB at 0x%lx)\n",
          svga->width, svga->height, svga->bpp,
          (unsigned long)(svga->fb_size / 1024U / 1024U),
          (unsigned long)svga->fb_phys);
    return 0;
}

static const device_id_t vmsvga_ids[] = {
    {
        .vendor = VMSVGA_VENDOR_ID,
        .device = VMSVGA_DEVICE_ID,
        .subvendor = VENDOR_ANY,
        .subdevice = VMSVGA_SUBSYSTEM_ID,
    },
    { 0 },
};

static driver_t vmsvga_driver = {
    .name = "vmsvga",
    .id_table = vmsvga_ids,
    .bus = &pci_bus,
    .probe = vmsvga_probe,
    .class_ops = &vmsvga_ops,
    .class_type = DEV_CLASS_DISPLAY,
};

DRIVER_REGISTER(vmsvga_driver);
