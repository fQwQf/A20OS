#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/gpu/gpu_core.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "mm/mm.h"

/* VMware SVGAv3, as exposed by VirtualBox ARM.  This is not the legacy
 * SVGA-II index/value I/O device used by an x86 VirtualBox guest. */
#define VMSVGA_VENDOR_ID       0x15ADU
#define VMSVGA2_DEVICE_ID      0x0405U
#define VMSVGA3_DEVICE_ID      0x0406U

/* Legacy SVGA-II register indices.  VBox still exposes this device ID on
 * several ARM builds even when the display controller is called VMSVGA. */
#define SVGA2_REG_ID           0U
#define SVGA2_REG_ENABLE       2U
#define SVGA2_REG_WIDTH        3U
#define SVGA2_REG_HEIGHT       4U
#define SVGA2_REG_BPP          7U
#define SVGA2_REG_FB_START     13U
#define SVGA2_REG_FB_SIZE      14U
#define SVGA2_REG_CONFIG_DONE  20U
#define SVGA2_ID               0x90000002U

#define SVGA3_REG_ID           0x000U
#define SVGA3_REG_ENABLE       0x004U
#define SVGA3_REG_WIDTH        0x008U
#define SVGA3_REG_HEIGHT       0x00CU
#define SVGA3_REG_BPP          0x01CU
#define SVGA3_REG_BYTES_PER_LINE 0x030U
#define SVGA3_REG_FB_OFFSET    0x038U
#define SVGA3_REG_FB_SIZE      0x040U
#define SVGA3_REG_CONFIG_DONE  0x050U
#define SVGA3_REG_TRACES       0x0B4U
#define SVGA3_REG_COMMAND_LO   0x0C0U
#define SVGA3_REG_COMMAND_HI   0x0C4U
#define SVGA3_REG_IRQ_MASK     0x084U
#define SVGA3_REG_IRQ_STATUS   0x148U

#define SVGA3_ID               0x90000003U
#define SVGA3_CB_STATUS_NONE       0U
#define SVGA3_CB_STATUS_COMPLETED  1U
#define SVGA3_CB_FLAG_NO_IRQ       (1U << 0)
#define SVGA3_CB_CONTEXT_DEVICE    0x3FU
#define SVGA3_CB_CONTEXT_0         0U
#define SVGA3_CMD_UPDATE           1U
#define SVGA3_CMD_DC_START_STOP    1U
#define SVGA3_CMD_WAIT_LOOPS       10000000U

typedef struct {
    volatile uint32_t status;
    volatile uint32_t error_offset;
    uint64_t id;
    uint32_t flags;
    uint32_t length;
    uint64_t physical_address;
    uint32_t must_be_zero[8];
} __attribute__((packed)) svga3_command_buffer_t;

_Static_assert(sizeof(svga3_command_buffer_t) == 64,
               "SVGAv3 command-buffer header must be 64 bytes");

typedef struct {
    uint32_t command;
    uint32_t enable;
    uint32_t context;
} __attribute__((packed)) svga3_start_context_t;

typedef struct {
    uint32_t command;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
} __attribute__((packed)) svga3_update_t;

typedef struct vmsvga_device {
    uintptr_t regs;
    uintptr_t fb_virt;
    uintptr_t fb_phys;
    size_t fb_size;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    int legacy;
    spinlock_t lock;
    /* One page is sufficient for the command-buffer header and the small
     * device-context/update commands used by this framebuffer driver. */
    uint8_t command_page[PAGE_SIZE] ALIGNED(PAGE_SIZE);
} vmsvga_device_t;

static vmsvga_device_t g_vmsvga;

static int vmsvga_flush(device_t *dev, uint32_t x, uint32_t y,
                        uint32_t width, uint32_t height);

static uint32_t vmsvga_test_color(uint32_t x, uint32_t width) {
    uint32_t band = width ? (x * 4U) / width : 0;
    static const uint32_t colors[4] = {
        0x00ff0000U, 0x0000ff00U, 0x000000ffU, 0x00ffffffU,
    };
    return colors[band < 4U ? band : 3U];
}

static int vmsvga_scanout_self_test(device_t *dev, vmsvga_device_t *svga) {
    volatile uint32_t *fb = (volatile uint32_t *)svga->fb_virt;
    uint32_t stride = svga->pitch / sizeof(uint32_t);

    for (uint32_t y = 0; y < svga->height; y++) {
        for (uint32_t x = 0; x < svga->width; x++)
            fb[(size_t)y * stride + x] = vmsvga_test_color(x, svga->width);
    }
    wmb();

    uint32_t samples[4] = {
        fb[svga->width / 8U],
        fb[svga->width * 3U / 8U],
        fb[svga->width * 5U / 8U],
        fb[svga->width * 7U / 8U],
    };
    int update = vmsvga_flush(dev, 0, 0, svga->width, svga->height);
    kinfo("[GPU] SVGAv3 scanout test: pixels=%08x,%08x,%08x,%08x update=%s\n",
          samples[0], samples[1], samples[2], samples[3],
          update == 0 ? "completed" : "failed");
    return update;
}

static void vmsvga_write(vmsvga_device_t *svga, uint32_t offset, uint32_t value) {
    if (svga->legacy) {
        writel(offset, (volatile void *)svga->regs);
        writel(value, (volatile void *)(svga->regs + 4U));
        return;
    }
    writel(value, (volatile void *)(svga->regs + offset));
}

static uint32_t vmsvga_read(vmsvga_device_t *svga, uint32_t offset) {
    if (svga->legacy) {
        writel(offset, (volatile void *)svga->regs);
        return readl((const volatile void *)(svga->regs + 4U));
    }
    return readl((const volatile void *)(svga->regs + offset));
}

static int vmsvga_submit(vmsvga_device_t *svga, const void *command,
                         size_t command_size, uint32_t context) {
    if (!svga || !command || command_size == 0 ||
        sizeof(svga3_command_buffer_t) + command_size > PAGE_SIZE)
        return -1;

    svga3_command_buffer_t *header = (svga3_command_buffer_t *)svga->command_page;
    uint8_t *payload = svga->command_page + sizeof(*header);
    memset(svga->command_page, 0, sizeof(svga->command_page));
    memcpy(payload, command, command_size);
    header->status = SVGA3_CB_STATUS_NONE;
    header->flags = SVGA3_CB_FLAG_NO_IRQ;
    header->length = (uint32_t)command_size;
    header->physical_address = va_to_pa(payload);

    arch_dma_sync_for_device(svga->command_page,
                             sizeof(*header) + command_size);
    wmb();
    uint64_t header_pa = va_to_pa(header);
    vmsvga_write(svga, SVGA3_REG_COMMAND_HI, (uint32_t)(header_pa >> 32));
    vmsvga_write(svga, SVGA3_REG_COMMAND_LO,
                 (uint32_t)header_pa | context);

    for (uint32_t i = 0; i < SVGA3_CMD_WAIT_LOOPS; i++) {
        arch_dma_sync_for_cpu(header, sizeof(*header));
        if (header->status != SVGA3_CB_STATUS_NONE) {
            if (header->status == SVGA3_CB_STATUS_COMPLETED)
                return 0;
            kerr("[GPU] SVGAv3 command rejected: context=%u command=%u "
                 "status=%u error-offset=%u\n",
                 context, *(const uint32_t *)command, header->status,
                 header->error_offset);
            return -1;
        }
        arch_cpu_relax();
    }
    kerr("[GPU] SVGAv3 command timeout (context=%u, command=%u)\n",
         context, *(const uint32_t *)command);
    return -1;
}

static int vmsvga_update_locked(vmsvga_device_t *svga, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
    svga3_update_t update = {
        .command = SVGA3_CMD_UPDATE,
        .x = x, .y = y, .width = width, .height = height,
    };
    return vmsvga_submit(svga, &update, sizeof(update), SVGA3_CB_CONTEXT_0);
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

static int vmsvga_flush(device_t *dev, uint32_t x, uint32_t y, uint32_t width,
                         uint32_t height) {
    vmsvga_device_t *svga = dev ? dev->drv_priv : NULL;
    if (!svga)
        return -1;
    if (width == 0 || height == 0) {
        x = y = 0;
        width = svga->width;
        height = svga->height;
    }
    if (x >= svga->width || y >= svga->height)
        return -1;
    if (width > svga->width - x)
        width = svga->width - x;
    if (height > svga->height - y)
        height = svga->height - y;

    uint64_t flags = spin_lock_irqsave(&svga->lock);
    int result = vmsvga_update_locked(svga, x, y, width, height);
    spin_unlock_irqrestore(&svga->lock, flags);
    return result;
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

    /* The SVGAv3 PCI layout is fixed: BAR0 is its MMIO register aperture and
     * BAR2 is VRAM.  Looking them up by BAR number is essential on ARM. */
    resource_t *regs = pci_get_bar_resource(dev, 0);
    resource_t *fb = pci_get_bar_resource(dev, 2);
    if (!fb)
        fb = pci_get_bar_resource(dev, 1);
    if (!regs || !fb || regs->end < regs->start || fb->end < fb->start)
        return -1;

    vmsvga_device_t *svga = &g_vmsvga;
    memset(svga, 0, sizeof(*svga));
    spin_init(&svga->lock);
    svga->legacy = pci_device_id(dev) ==
                   ((uint32_t)VMSVGA_VENDOR_ID << 16 | VMSVGA2_DEVICE_ID);
    svga->regs = (uintptr_t)regs->start;
    svga->fb_virt = (uintptr_t)fb->start;
    svga->fb_phys = (uintptr_t)fb->start - PAGE_OFFSET;
    svga->fb_size = (size_t)(fb->end - fb->start + 1U);

    if (svga->legacy) {
        vmsvga_write(svga, SVGA2_REG_ID, SVGA2_ID);
        if (vmsvga_read(svga, SVGA2_REG_ID) != SVGA2_ID) {
            kerr("[GPU] SVGAv2 ID negotiation failed\n");
            return -1;
        }
        vmsvga_write(svga, SVGA2_REG_ENABLE, 0);
        vmsvga_write(svga, SVGA2_REG_WIDTH, 1024U);
        vmsvga_write(svga, SVGA2_REG_HEIGHT, 768U);
        vmsvga_write(svga, SVGA2_REG_BPP, 32U);
        vmsvga_write(svga, SVGA2_REG_ENABLE, 1U);
        vmsvga_write(svga, SVGA2_REG_CONFIG_DONE, 1U);

        svga->width = vmsvga_read(svga, SVGA2_REG_WIDTH);
        svga->height = vmsvga_read(svga, SVGA2_REG_HEIGHT);
        svga->bpp = vmsvga_read(svga, SVGA2_REG_BPP);
        uintptr_t device_fb = (uintptr_t)vmsvga_read(svga, SVGA2_REG_FB_START);
        size_t device_fb_size = (size_t)vmsvga_read(svga, SVGA2_REG_FB_SIZE);
        if (!svga->width || !svga->height || svga->bpp != 32U || !device_fb ||
            device_fb_size < (size_t)svga->width * svga->height * 4U)
            return -1;
        svga->fb_phys = device_fb;
        svga->fb_virt = device_fb + PAGE_OFFSET;
        svga->fb_size = device_fb_size;
        dev->drv_priv = svga;
        if (gpu_device_register(dev) < 0) {
            dev->drv_priv = NULL;
            return -1;
        }
        kinfo("[GPU] SVGAv2 ready: %ux%ux%u (VRAM: %lu MiB at 0x%lx)\n",
              svga->width, svga->height, svga->bpp,
              (unsigned long)(svga->fb_size / 1024U / 1024U),
              (unsigned long)svga->fb_phys);
        return 0;
    }

    if ((size_t)(regs->end - regs->start + 1U) < SVGA3_REG_IRQ_STATUS + 4U)
        return -1;

    vmsvga_write(svga, SVGA3_REG_ID, SVGA3_ID);
    if (vmsvga_read(svga, SVGA3_REG_ID) != SVGA3_ID) {
        kerr("[GPU] SVGAv3 ID negotiation failed\n");
        return -1;
    }
    vmsvga_write(svga, SVGA3_REG_IRQ_MASK, 0);
    vmsvga_write(svga, SVGA3_REG_IRQ_STATUS, 0);

    /* SVGAv3 needs its device context running before it accepts update
     * commands.  This is the critical difference from the old VMSVGA path. */
    svga3_start_context_t start = {
        .command = SVGA3_CMD_DC_START_STOP,
        .enable = 1,
        .context = SVGA3_CB_CONTEXT_0,
    };
    if (vmsvga_submit(svga, &start, sizeof(start), SVGA3_CB_CONTEXT_DEVICE) != 0)
        return -1;

    vmsvga_write(svga, SVGA3_REG_ENABLE, 0);
    vmsvga_write(svga, SVGA3_REG_CONFIG_DONE, 0);
    vmsvga_write(svga, SVGA3_REG_WIDTH, 1024U);
    vmsvga_write(svga, SVGA3_REG_HEIGHT, 768U);
    vmsvga_write(svga, SVGA3_REG_BPP, 32U);
    vmsvga_write(svga, SVGA3_REG_ENABLE, 1U);
    vmsvga_write(svga, SVGA3_REG_CONFIG_DONE, 1U);
    /* Enable dirty-VRAM tracking too: it makes VBox refresh direct framebuffer
     * writes even between explicit FBIO_FLUSH calls. */
    vmsvga_write(svga, SVGA3_REG_TRACES, 1U);

    svga->width = vmsvga_read(svga, SVGA3_REG_WIDTH);
    svga->height = vmsvga_read(svga, SVGA3_REG_HEIGHT);
    svga->bpp = vmsvga_read(svga, SVGA3_REG_BPP);
    svga->pitch = vmsvga_read(svga, SVGA3_REG_BYTES_PER_LINE);
    uint32_t fb_offset = vmsvga_read(svga, SVGA3_REG_FB_OFFSET);
    size_t device_fb_size = vmsvga_read(svga, SVGA3_REG_FB_SIZE);
    size_t vram_size = svga->fb_size;
    size_t visible = (size_t)svga->pitch * svga->height;
    if (!svga->width || !svga->height || svga->bpp != 32U ||
        svga->pitch < svga->width * 4U || fb_offset >= vram_size ||
        visible > vram_size - fb_offset ||
        (device_fb_size && device_fb_size < visible))
        return -1;

    /* BAR2 is the complete VRAM aperture.  The primary scanout starts at the
     * device-selected FB_OFFSET, which is not guaranteed to be zero on VBox.
     * Only expose the visible scanout to userspace; handing LVGL all of VRAM
     * makes it infer an invalid direct-render buffer layout. */
    svga->fb_phys += fb_offset;
    svga->fb_virt += fb_offset;
    svga->fb_size = visible;

    dev->drv_priv = svga;
    if (gpu_device_register(dev) < 0) {
        dev->drv_priv = NULL;
        return -1;
    }
    if (vmsvga_scanout_self_test(dev, svga) != 0) {
        dev->drv_priv = NULL;
        return -1;
    }

    kinfo("[GPU] SVGAv3 ready: %ux%ux%u pitch=%u offset=0x%x (FB: %lu KiB at 0x%lx)\n",
          svga->width, svga->height, svga->bpp,
          svga->pitch, fb_offset, (unsigned long)(svga->fb_size / 1024U),
          (unsigned long)svga->fb_phys);
    return 0;
}

static const device_id_t vmsvga_ids[] = {
    { .vendor = VMSVGA_VENDOR_ID, .device = VMSVGA2_DEVICE_ID,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = VMSVGA_VENDOR_ID, .device = VMSVGA3_DEVICE_ID,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { 0 },
};

static driver_t vmsvga_driver = {
    .name = "vmsvga", .id_table = vmsvga_ids, .bus = &pci_bus,
    .probe = vmsvga_probe, .class_ops = &vmsvga_ops,
    .class_type = DEV_CLASS_DISPLAY,
};

DRIVER_REGISTER(vmsvga_driver);
