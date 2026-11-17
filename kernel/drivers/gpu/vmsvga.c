#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "drivers/gpu/gpu_core.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/sync.h"
#include "core/string.h"
#include "mm/mm.h"

/* SVGA-II uses indexed registers plus a VRAM FIFO; SVGAv3 uses direct
 * MMIO registers and command buffers. Keep their layouts and queues distinct. */
#define VMSVGA_VENDOR_ID       0x15ADU
#define VMSVGA2_DEVICE_ID      0x0405U
#define VMSVGA3_DEVICE_ID      0x0406U

/* Legacy SVGA-II register indices.  VBox still exposes this device ID on
 * several ARM builds even when the display controller is called VMSVGA. */
#define SVGA2_REG_ID           0U
#define SVGA2_REG_ENABLE       1U
#define SVGA2_REG_WIDTH        2U
#define SVGA2_REG_HEIGHT       3U
#define SVGA2_REG_BPP          7U
#define SVGA2_REG_BYTES_PER_LINE 12U
#define SVGA2_REG_FB_START     13U
#define SVGA2_REG_FB_OFFSET    14U
#define SVGA2_REG_FB_SIZE      16U
#define SVGA2_REG_MEM_SIZE     19U
#define SVGA2_REG_CONFIG_DONE  20U
#define SVGA2_REG_SYNC         21U
#define SVGA2_REG_BUSY         22U
#define SVGA2_FIFO_MIN         0U
#define SVGA2_FIFO_MAX         4U
#define SVGA2_FIFO_NEXT        8U
#define SVGA2_FIFO_STOP        12U
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
    device_t *owner;
    uintptr_t regs;
    uintptr_t fb_virt;
    uintptr_t fb_phys;
    size_t fb_size;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t pitch;
    int legacy, port_io;
    uintptr_t fifo;
    uint32_t fifo_bytes, fifo_min, fifo_max;
    int fifo_ready;
    mutex_t lock;
    int closing, pending, pending_stop, context_live;
    int pci_touched, configured, published, stopped;
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
    kinfo("[GPU] SVGA%s scanout test: pixels=%08x,%08x,%08x,%08x update=%s\n",
          svga->legacy ? "-II" : "v3", samples[0], samples[1], samples[2], samples[3],
          update == 0 ? "completed" : "failed");
    return update;
}

static void vmsvga_write(vmsvga_device_t *svga, uint32_t offset, uint32_t value) {
    if (svga->legacy) {
        if (svga->port_io) {
            ioport_write32((uint16_t)svga->regs, offset);
            ioport_write32((uint16_t)(svga->regs + 1U), value);
        } else {
            writel(offset, (volatile void *)svga->regs);
            writel(value, (volatile void *)(svga->regs + 4U));
        }
        return;
    }
    writel(value, (volatile void *)(svga->regs + offset));
}

static uint32_t vmsvga_read(vmsvga_device_t *svga, uint32_t offset) {
    if (svga->legacy) {
        if (svga->port_io) {
            ioport_write32((uint16_t)svga->regs, offset);
            return ioport_read32((uint16_t)(svga->regs + 1U));
        }
        writel(offset, (volatile void *)svga->regs);
        return readl((const volatile void *)(svga->regs + 4U));
    }
    return readl((const volatile void *)(svga->regs + offset));
}

/* A timeout/cancel is not device retirement. Preserve this page verbatim
 * until the host reports a known terminal status; no later submit may reuse it. */
static int vmsvga_wait_pending(vmsvga_device_t *svga, int cleanup) {
    if (!svga->pending)
        return 0;
    svga3_command_buffer_t *header = (void *)svga->command_page;
    uint64_t start = clock_get_ticks();
    uint64_t limit = clock_ticks_per_sec() / 10U;
    if (!limit) limit = 1;
    for (uint32_t i = 0; i < SVGA3_CMD_WAIT_LOOPS; i++) {
        if (!cleanup && __atomic_load_n(&svga->closing, __ATOMIC_ACQUIRE))
            return -ESHUTDOWN;
        arch_dma_sync_for_cpu(header, sizeof(*header));
        uint32_t status = header->status;
        if (status != SVGA3_CB_STATUS_NONE) {
            /* 1..6 are defined terminal statuses in the base protocol.
             * Unknown/partial status cannot authorize reuse or release. */
            if (status > 6U)
                return -EIO;
            svga->pending = 0;
            if (status == SVGA3_CB_STATUS_COMPLETED && svga->pending_stop)
                svga->context_live = 0;
            svga->pending_stop = 0;
            return status == SVGA3_CB_STATUS_COMPLETED ? 0 : -EIO;
        }
        if (clock_get_ticks() - start >= limit)
            return -ETIMEDOUT;
        arch_cpu_relax();
    }
    return -ETIMEDOUT;
}

static int vmsvga_submit(vmsvga_device_t *svga, const void *command,
                         size_t command_size, uint32_t context, int cleanup) {
    if (!svga || !command || command_size == 0 ||
        command_size > PAGE_SIZE - sizeof(svga3_command_buffer_t))
        return -EINVAL;
    if (!cleanup && __atomic_load_n(&svga->closing, __ATOMIC_ACQUIRE))
        return -ESHUTDOWN;
    if (svga->pending)
        return -EBUSY;

    svga3_command_buffer_t *header = (void *)svga->command_page;
    uint8_t *payload = svga->command_page + sizeof(*header);
    memset(svga->command_page, 0, sizeof(svga->command_page));
    memcpy(payload, command, command_size);
    header->flags = SVGA3_CB_FLAG_NO_IRQ;
    header->length = (uint32_t)command_size;
    header->physical_address = va_to_pa(payload);
    if (context == SVGA3_CB_CONTEXT_DEVICE && command_size == sizeof(svga3_start_context_t)) {
        svga3_start_context_t control;
        memcpy(&control, command, sizeof(control));
        if (control.command == SVGA3_CMD_DC_START_STOP) {
            if (control.enable) svga->context_live = 1; /* may be accepted */
            svga->pending_stop = !control.enable;
        }
    }
    arch_dma_sync_for_device(svga->command_page, sizeof(*header) + command_size);
    wmb();
    uint64_t header_pa = va_to_pa(header);
    svga->pending = 1;
    vmsvga_write(svga, SVGA3_REG_COMMAND_HI, (uint32_t)(header_pa >> 32));
    vmsvga_write(svga, SVGA3_REG_COMMAND_LO, (uint32_t)header_pa | context);
    int ret = vmsvga_wait_pending(svga, cleanup);
    if (ret && !cleanup)
        __atomic_store_n(&svga->closing, 1, __ATOMIC_RELEASE);
    return ret;
}

static int vmsvga_fifo_state(vmsvga_device_t *svga, uint32_t *next, uint32_t *stop) {
    uint32_t min = readl((const volatile void *)(svga->fifo + SVGA2_FIFO_MIN));
    uint32_t max = readl((const volatile void *)(svga->fifo + SVGA2_FIFO_MAX));
    *next = readl((const volatile void *)(svga->fifo + SVGA2_FIFO_NEXT));
    *stop = readl((const volatile void *)(svga->fifo + SVGA2_FIFO_STOP));
    if ((min | max | *next | *stop) == 0xffffffffU)
        return -ENODEV;
    if (min != svga->fifo_min || max != svga->fifo_max ||
        ((min | max | *next | *stop) & 3U) ||
        *next < min || *next >= max || *stop < min || *stop >= max)
        return -EIO;
    return 0;
}

static int vmsvga_fifo_drain(vmsvga_device_t *svga, int cleanup) {
    if (!svga->fifo_ready)
        return 0;
    uint64_t start = clock_get_ticks(), limit = clock_ticks_per_sec() / 10U;
    if (!limit) limit = 1;
    vmsvga_write(svga, SVGA2_REG_SYNC, 1);
    for (uint32_t i = 0; i < SVGA3_CMD_WAIT_LOOPS; i++) {
        if (!cleanup && __atomic_load_n(&svga->closing, __ATOMIC_ACQUIRE))
            return -ESHUTDOWN;
        uint32_t next, stop;
        int ret = vmsvga_fifo_state(svga, &next, &stop);
        if (ret) return ret;
        uint32_t busy = vmsvga_read(svga, SVGA2_REG_BUSY);
        if (busy == 0xffffffffU) return -ENODEV;
        if (!busy && next == stop) return 0;
        if (clock_get_ticks() - start >= limit) return -ETIMEDOUT;
        arch_cpu_relax();
    }
    return -ETIMEDOUT;
}

static int vmsvga_fifo_update(vmsvga_device_t *svga, uint32_t x, uint32_t y,
                              uint32_t width, uint32_t height) {
    if (__atomic_load_n(&svga->closing, __ATOMIC_ACQUIRE))
        return -ESHUTDOWN;
    /* One synchronous producer, one whole UPDATE packet, one spare word.
     * A timed-out packet remains in place for stop/drain, never overwritten. */
    int ret = vmsvga_fifo_drain(svga, 0);
    if (!ret) {
        uint32_t next, stop;
        ret = vmsvga_fifo_state(svga, &next, &stop);
        if (!ret) {
            const uint32_t packet[] = {1U, x, y, width, height};
            for (unsigned i = 0; i < sizeof(packet) / sizeof(packet[0]); i++) {
                writel(packet[i], (volatile void *)(svga->fifo + next));
                next += 4U;
                if (next == svga->fifo_max) next = svga->fifo_min;
            }
            wmb();
            writel(next, (volatile void *)(svga->fifo + SVGA2_FIFO_NEXT));
            ret = vmsvga_fifo_drain(svga, 0);
        }
    }
    if (ret) __atomic_store_n(&svga->closing, 1, __ATOMIC_RELEASE);
    return ret;
}

static int vmsvga_update_locked(vmsvga_device_t *svga, uint32_t x, uint32_t y,
                                uint32_t width, uint32_t height) {
    if (svga->legacy)
        return vmsvga_fifo_update(svga, x, y, width, height);
    svga3_update_t update = {
        .command = SVGA3_CMD_UPDATE,
        .x = x, .y = y, .width = width, .height = height,
    };
    return vmsvga_submit(svga, &update, sizeof(update), SVGA3_CB_CONTEXT_0, 0);
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

static int vmsvga_get_layout(device_t *dev, gpu_scanout_layout_t *out) {
    vmsvga_device_t *svga = dev ? dev->drv_priv : NULL;
    if (!svga || !out) return -EINVAL;
    *out = (gpu_scanout_layout_t){svga->pitch, 0x00ff0000U, 0x0000ff00U,
                                 0x000000ffU, 0xff000000U};
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

    if (__atomic_load_n(&svga->closing, __ATOMIC_ACQUIRE))
        return -ESHUTDOWN;
    mutex_lock(&svga->lock);
    int result = vmsvga_update_locked(svga, x, y, width, height);
    mutex_unlock(&svga->lock);
    return result;
}

static int vmsvga_ioctl(device_t *dev, unsigned long req, void *arg) {
    (void)dev;
    (void)req;
    (void)arg;
    return -1;
}

static int vmsvga_get_features(struct device *dev, uint32_t *out_3d,
                               uint32_t *out_context_init)
{
    (void)dev;
    if (out_3d)
        *out_3d = 0;
    if (out_context_init)
        *out_context_init = 0;
    return 0;
}

static int vmsvga_get_capabilities(device_t *dev, struct gpu_capabilities *out) {
    if (!out) return -EINVAL;
    *out = (struct gpu_capabilities){0};
    if (!dev || !dev->drv_priv) return -ENODEV;
    out->flags = GPU_CAP_SCANOUT;
    return 0;
}

static const gpu_dev_ops_t vmsvga_ops = {
    .get_capabilities = vmsvga_get_capabilities,
    .get_info = vmsvga_get_info,
    .get_fb = vmsvga_get_fb,
    .get_layout = vmsvga_get_layout,
    .flush = vmsvga_flush,
    .ioctl = vmsvga_ioctl,
    .get_features = vmsvga_get_features,
};

static int vmsvga_stop(void *context) {
    vmsvga_device_t *svga = context;
    __atomic_store_n(&svga->closing, 1, __ATOMIC_RELEASE);
    if (!mutex_trylock(&svga->lock))
        return -EBUSY;
    int ret = vmsvga_wait_pending(svga, 1);
    if (ret) goto out;
    if (svga->configured) {
        uint32_t id_reg = svga->legacy ? SVGA2_REG_ID : SVGA3_REG_ID;
        uint32_t id = svga->legacy ? SVGA2_ID : SVGA3_ID;
        if (vmsvga_read(svga, id_reg) != id) { ret = -ENODEV; goto out; }
        if (svga->legacy) {
            if (!svga->fifo_ready && vmsvga_read(svga, SVGA2_REG_BUSY)) {
                ret = -EIO;
                goto out;
            }
            ret = vmsvga_fifo_drain(svga, 1);
            if (ret) goto out;
        } else {
            vmsvga_write(svga, SVGA3_REG_IRQ_MASK, 0);
            if (vmsvga_read(svga, SVGA3_REG_IRQ_MASK) != 0) { ret = -EIO; goto out; }
            if (svga->context_live) {
                svga3_start_context_t stop = {SVGA3_CMD_DC_START_STOP, 0, SVGA3_CB_CONTEXT_0};
                ret = vmsvga_submit(svga, &stop, sizeof(stop), SVGA3_CB_CONTEXT_DEVICE, 1);
                if (ret) goto out;
            }
        }
        uint32_t enable = svga->legacy ? SVGA2_REG_ENABLE : SVGA3_REG_ENABLE;
        uint32_t done = svga->legacy ? SVGA2_REG_CONFIG_DONE : SVGA3_REG_CONFIG_DONE;
        vmsvga_write(svga, enable, 0);
        vmsvga_write(svga, done, 0);
        if (vmsvga_read(svga, enable) != 0 || vmsvga_read(svga, done) != 0) {
            ret = -EIO;
            goto out;
        }
    }
    if (svga->pci_touched) {
        uint16_t command = pci_cfg_read16(svga->owner, 0x04);
        if (command == 0xffffU) { ret = -ENODEV; goto out; }
        command = (command & (uint16_t)~0x04U) | 0x400U;
        pci_cfg_write16(svga->owner, 0x04, command);
        command = pci_cfg_read16(svga->owner, 0x04);
        if (command == 0xffffU || (command & 0x404U) != 0x400U) {
            ret = -EIO;
            goto out;
        }
    }
    svga->stopped = 1;
    if (svga->legacy && svga->configured)
        kinfo("[GPU] SVGA-II stopped: FIFO drained, scanout disabled\n");
out:
    mutex_unlock(&svga->lock);
    return ret;
}

static void vmsvga_release(void *context) {
    memset(context, 0, sizeof(vmsvga_device_t));
}

static int vmsvga_probe(device_t *dev) {
    /* Existing single-device capacity: never overwrite a retained binding. */
    if (g_vmsvga.owner) return -EBUSY;
    int ownership = gpu_device_physical_mapping_init(dev);
    if (ownership) return ownership;
    vmsvga_device_t *svga = &g_vmsvga;
    memset(svga, 0, sizeof(*svga));
    svga->owner = dev;
    dev->drv_priv = svga;
    mutex_init(&svga->lock);
    int ret = device_add_execution_action(dev, vmsvga_stop, vmsvga_release, svga);
    if (ret) { dev->drv_priv = NULL; memset(svga, 0, sizeof(*svga)); return ret; }
    svga->pci_touched = 1;
    ret = pci_enable_and_assign_bars(dev);
    if (ret) return ret;

    svga->legacy = pci_device_id(dev) ==
                   ((uint32_t)VMSVGA_VENDOR_ID << 16 | VMSVGA2_DEVICE_ID);
    resource_t *regs = pci_get_bar_resource(dev, 0);
    resource_t *fb = pci_get_bar_resource(dev, svga->legacy ? 1 : 2);
    resource_t *fifo = svga->legacy ? pci_get_bar_resource(dev, 2) : NULL;
    if (!regs || !fb || regs->end < regs->start || fb->end < fb->start ||
        fb->type != RES_MMIO || fb->end - fb->start >= SIZE_MAX)
        return -ENODEV;
    if (svga->legacy && (!fifo || fifo->type != RES_MMIO || fifo->end < fifo->start))
        return -ENODEV;
    svga->port_io = regs->type == RES_IOPORT;
    size_t reg_bytes = svga->legacy ? (svga->port_io ? 5U : 8U) : SVGA3_REG_IRQ_STATUS + 4U;
    if (regs->end - regs->start < reg_bytes - 1U ||
        (!svga->legacy && svga->port_io)) return -ENODEV;
    if (svga->port_io) {
        if (regs->end > 0xffffU) return -ERANGE;
        ret = device_claim_resource(dev, regs);
        if (ret) return ret;
        svga->regs = (uintptr_t)regs->start;
    } else {
        if (regs->type != RES_MMIO) return -ENODEV;
        svga->regs = (uintptr_t)device_map_resource(dev, regs);
        if (!svga->regs) return -EIO;
    }
    svga->fb_virt = (uintptr_t)device_map_resource(dev, fb);
    if (!svga->fb_virt) return -EIO;
    if (fifo) {
        svga->fifo = (uintptr_t)device_map_resource(dev, fifo);
        if (!svga->fifo) return -EIO;
    }
    svga->fb_phys = (uintptr_t)fb->start;
    svga->fb_size = (size_t)(fb->end - fb->start + 1U);

    if (svga->legacy) {
        vmsvga_write(svga, SVGA2_REG_ID, SVGA2_ID);
        if (vmsvga_read(svga, SVGA2_REG_ID) != SVGA2_ID) {
            kerr("[GPU] SVGAv2 ID negotiation failed\n");
            return -1;
        }
        svga->configured = 1;
        vmsvga_write(svga, SVGA2_REG_ENABLE, 0);
        vmsvga_write(svga, SVGA2_REG_CONFIG_DONE, 0);
        if (vmsvga_read(svga, SVGA2_REG_ENABLE) ||
            vmsvga_read(svga, SVGA2_REG_CONFIG_DONE) ||
            vmsvga_read(svga, SVGA2_REG_BUSY)) return -EIO;
        uint32_t bytes = vmsvga_read(svga, SVGA2_REG_MEM_SIZE);
        if (bytes == 0xffffffffU) return -ENODEV;
        if (bytes < 16U + 24U || bytes - 1U > fifo->end - fifo->start)
            return -EIO;
        svga->fifo_bytes = bytes;
        svga->fifo_min = 16U;
        svga->fifo_max = bytes & ~3U;
        writel(svga->fifo_min, (volatile void *)(svga->fifo + SVGA2_FIFO_MIN));
        writel(svga->fifo_max, (volatile void *)(svga->fifo + SVGA2_FIFO_MAX));
        writel(svga->fifo_min, (volatile void *)(svga->fifo + SVGA2_FIFO_NEXT));
        writel(svga->fifo_min, (volatile void *)(svga->fifo + SVGA2_FIFO_STOP));
        svga->fifo_ready = 1;
        wmb();
        vmsvga_write(svga, SVGA2_REG_WIDTH, 1024U);
        vmsvga_write(svga, SVGA2_REG_HEIGHT, 768U);
        vmsvga_write(svga, SVGA2_REG_BPP, 32U);
        vmsvga_write(svga, SVGA2_REG_ENABLE, 1U);
        vmsvga_write(svga, SVGA2_REG_CONFIG_DONE, 1U);

        svga->width = vmsvga_read(svga, SVGA2_REG_WIDTH);
        svga->height = vmsvga_read(svga, SVGA2_REG_HEIGHT);
        svga->bpp = vmsvga_read(svga, SVGA2_REG_BPP);
        svga->pitch = vmsvga_read(svga, SVGA2_REG_BYTES_PER_LINE);
        uint32_t offset = vmsvga_read(svga, SVGA2_REG_FB_OFFSET);
        uint32_t device_size = vmsvga_read(svga, SVGA2_REG_FB_SIZE);
        uint64_t visible = (uint64_t)svga->pitch * svga->height;
        if (!svga->width || !svga->height || svga->bpp != 32U ||
            (uint64_t)svga->pitch < (uint64_t)svga->width * 4U ||
            offset >= svga->fb_size || visible > svga->fb_size - offset ||
            device_size < visible || visible > SIZE_MAX)
            return -ENODEV;
        svga->fb_phys += offset;
        svga->fb_virt += offset;
        svga->fb_size = (size_t)visible;
        ret = vmsvga_scanout_self_test(dev, svga);
        if (ret) return ret;
        ret = gpu_device_register(dev);
        if (ret) return ret;
        svga->published = 1;
        kinfo("[GPU] SVGA-II ready: %ux%ux%u pitch=%u FIFO=%u bytes\n",
              svga->width, svga->height, svga->bpp, svga->pitch, svga->fifo_bytes);
        return 0;
    }

    if ((size_t)(regs->end - regs->start + 1U) < SVGA3_REG_IRQ_STATUS + 4U)
        return -1;

    vmsvga_write(svga, SVGA3_REG_ID, SVGA3_ID);
    if (vmsvga_read(svga, SVGA3_REG_ID) != SVGA3_ID) {
        kerr("[GPU] SVGAv3 ID negotiation failed\n");
        return -1;
    }
    svga->configured = 1;
    vmsvga_write(svga, SVGA3_REG_IRQ_MASK, 0);
    vmsvga_write(svga, SVGA3_REG_IRQ_STATUS, 0);

    /* SVGAv3 needs its device context running before it accepts update
     * commands.  This is the critical difference from the old VMSVGA path. */
    svga3_start_context_t start = {
        .command = SVGA3_CMD_DC_START_STOP,
        .enable = 1,
        .context = SVGA3_CB_CONTEXT_0,
    };
    mutex_lock(&svga->lock);
    ret = vmsvga_submit(svga, &start, sizeof(start), SVGA3_CB_CONTEXT_DEVICE, 0);
    mutex_unlock(&svga->lock);
    if (ret) return ret;

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
        return -1;
    }
    svga->published = 1;
    if (vmsvga_scanout_self_test(dev, svga) != 0) {
        return -1;
    }

    kinfo("[GPU] SVGAv3 ready: %ux%ux%u pitch=%u offset=0x%x (FB: %lu KiB at 0x%lx)\n",
          svga->width, svga->height, svga->bpp,
          svga->pitch, fb_offset, (unsigned long)(svga->fb_size / 1024U),
          (unsigned long)svga->fb_phys);
    return 0;
}

static int vmsvga_remove(device_t *dev) {
    vmsvga_device_t *svga = dev ? dev->drv_priv : NULL;
    if (!svga)
        return 0;
    if (!svga->stopped || svga->pending)
        return -EBUSY;
    if (svga->published)
        gpu_device_unregister(dev);
    dev->drv_priv = NULL;
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
    .probe = vmsvga_probe, .remove = vmsvga_remove,
    .class_ops = &vmsvga_ops,
    .class_type = DEV_CLASS_DISPLAY,
};

DRIVER_REGISTER(vmsvga_driver);
