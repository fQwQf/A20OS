#include "drivers/gpu/virtio_gpu.h"
#include "drivers/gpu/gpu_core.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/lock.h"

#include "mm/mm.h"
#include "mm/frame.h"
#include "drivers/block/virtio_blk.h"

#define VIRTIO_GPU_QUEUE_SIZE VIRTIO_QUEUE_SIZE

typedef struct {
    virtio_transport_t vt;
    virtq_desc_t       desc[VIRTIO_GPU_QUEUE_SIZE] ALIGNED(64);
    virtq_avail_t      avail ALIGNED(64);
    virtq_used_t       used ALIGNED(64);
    
    spinlock_t         lock ALIGNED(64);
    int                slot;
    
    uint32_t           width;
    uint32_t           height;
    uint32_t           bpp;
    uintptr_t          fb_phys;
    size_t             fb_size;
    
    uint16_t           desc_idx;
    uint16_t           last_used;
    int                valid;
} virtio_gpu_inst_t;

static virtio_gpu_inst_t g_gpu_inst;
static device_t g_gpu_pci_device;
static driver_t virtio_gpu_driver;

static void virtio_gpu_mmio_write32(virtio_transport_t *t, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)((uintptr_t)t->priv + off));
}

static uint32_t virtio_gpu_mmio_read32(virtio_transport_t *t, uint32_t off) {
    return readl((const volatile void *)((uintptr_t)t->priv + off));
}

static int virtio_gpu_send_cmd(virtio_gpu_inst_t *inst, void *req, size_t req_len, void *resp, size_t resp_len) {
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    int completed = 0;
    
    uint16_t head = inst->desc_idx % VIRTIO_GPU_QUEUE_SIZE;
    uint16_t slot = head;
    uint16_t resp_slot = (slot + 1) % VIRTIO_GPU_QUEUE_SIZE;
    
    // Descriptor for request (device-read)
    inst->desc[slot].addr  = va_to_pa(req);
    inst->desc[slot].len   = (uint32_t)req_len;
    inst->desc[slot].flags = VIRTQ_DESC_F_NEXT;
    inst->desc[slot].next  = resp_slot;
    
    // Descriptor for response (device-write)
    inst->desc[resp_slot].addr  = va_to_pa(resp);
    inst->desc[resp_slot].len   = (uint32_t)resp_len;
    inst->desc[resp_slot].flags = VIRTQ_DESC_F_WRITE;
    inst->desc[resp_slot].next  = 0;
    
    // Flush descriptors to device
    arch_dma_sync_for_device(&inst->desc[slot], sizeof(virtq_desc_t));
    arch_dma_sync_for_device(&inst->desc[resp_slot], sizeof(virtq_desc_t));
    arch_dma_sync_for_device(req, req_len);
    arch_dma_sync_for_device(resp, resp_len);
    
    // Put request descriptor into avail ring
    uint16_t avail_slot = inst->avail.idx % VIRTIO_GPU_QUEUE_SIZE;
    inst->avail.ring[avail_slot] = slot;
    wmb();
    inst->avail.idx++;
    wmb();
    
    // Sync avail ring updates to device
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    
    inst->desc_idx += 2;
    
    // Snapshot current used->idx BEFORE notifying device
    arch_dma_sync_for_cpu(&inst->used, sizeof(inst->used));
    uint16_t used_before = ((volatile virtq_used_t *)&inst->used)->idx;
    
    // Notify device (queue 0 = controlq)
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();
    
    // Poll until used->idx advances beyond what it was before submit
    volatile virtq_used_t *used = &inst->used;
    uint32_t timeout = 5000000; // ~5M iterations, enough for slow emulation
    while (timeout--) {
        arch_dma_sync_for_cpu((void *)used, sizeof(*used));
        if (used->idx != used_before) {
            completed = 1;
            break;
        }
    }
    
    if (!completed) {
        kinfo("[GPU] send_cmd TIMEOUT! cmd=%x\n", ((struct virtio_gpu_ctrl_hdr *)req)->type);
        spin_unlock_irqrestore(&inst->lock, flags);
        return -1;
    }
    
    // Consume the used entry
    uint16_t ring_idx = (uint16_t)(used_before % VIRTIO_GPU_QUEUE_SIZE);
    arch_dma_sync_for_cpu(resp, resp_len);
    if (used->ring[ring_idx].id != slot) {
        spin_unlock_irqrestore(&inst->lock, flags);
        return -1;
    }
    
    inst->last_used = used->idx;
    
    spin_unlock_irqrestore(&inst->lock, flags);
    return 0;
}

static int gpu_get_info(struct device *dev, uint32_t *width, uint32_t *height, uint32_t *bpp) {
    virtio_gpu_inst_t *inst = dev->drv_priv;
    *width = inst->width;
    *height = inst->height;
    *bpp = inst->bpp;
    return 0;
}

static int gpu_get_fb(struct device *dev, uintptr_t *fb_paddr, size_t *fb_size) {
    virtio_gpu_inst_t *inst = dev->drv_priv;
    *fb_paddr = inst->fb_phys;
    *fb_size = inst->fb_size;
    return 0;
}

static int gpu_flush(struct device *dev, uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    virtio_gpu_inst_t *inst = dev->drv_priv;
    
    if (w == 0 || h == 0) {
        x = 0; y = 0;
        w = inst->width; h = inst->height;
    }

    if (x >= inst->width || y >= inst->height)
        return -1;
    if (w > inst->width - x)
        w = inst->width - x;
    if (h > inst->height - y)
        h = inst->height - y;

    size_t offset = ((size_t)y * inst->width + x) * (inst->bpp / 8);
    size_t bytes = ((size_t)(h - 1) * inst->width + w) * (inst->bpp / 8);
    pfn_t fb_pfn = phys_to_pfn(inst->fb_phys);
    void *fb_virt = pfn_to_virt(fb_pfn);
    if (!fb_virt)
        return -1;
    arch_dma_sync_for_device((uint8_t *)fb_virt + offset, bytes);
    
    struct virtio_gpu_transfer_to_host_2d t2h ALIGNED(64);
    memset(&t2h, 0, sizeof(t2h));
    t2h.hdr.type = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    t2h.r.x = x;
    t2h.r.y = y;
    t2h.r.width = w;
    t2h.r.height = h;
    t2h.offset = offset;
    t2h.resource_id = 1;
    
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&resp, 0, sizeof(resp));
    
    if (virtio_gpu_send_cmd(inst, &t2h, sizeof(t2h), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    
    struct virtio_gpu_resource_flush flush ALIGNED(64);
    memset(&flush, 0, sizeof(flush));
    flush.hdr.type = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.x = x;
    flush.r.y = y;
    flush.r.width = w;
    flush.r.height = h;
    flush.resource_id = 1;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &flush, sizeof(flush), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        return -1;
    
    return 0;
}

static int gpu_ioctl(struct device *dev, unsigned long req, void *arg) {
    (void)dev; (void)req; (void)arg;
    return -1;
}

static const gpu_dev_ops_t gpu_ops = {
    .get_info = gpu_get_info,
    .get_fb   = gpu_get_fb,
    .flush    = gpu_flush,
    .ioctl    = gpu_ioctl,
};

static int virtio_gpu_init_transport(device_t *dev, const virtio_transport_t *transport) {
    virtio_gpu_inst_t *inst = &g_gpu_inst;
    int order = 0;
    pfn_t fb_pfn = PFN_NONE;
    memset(inst, 0, sizeof(*inst));
    spin_init(&inst->lock);
    
    inst->vt = *transport;
    
    virtio_transport_t *vt = &inst->vt;

    uint32_t magic = vt->read32(vt, VIRTIO_MMIO_MAGIC);
    uint32_t version = vt->read32(vt, VIRTIO_MMIO_VERSION);
    uint32_t device_id = vt->read32(vt, VIRTIO_MMIO_DEVICE_ID);
    if (magic != 0x74726976U || version != 2U || device_id != 16U)
        return -1;
    
    vt->write32(vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    
    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
    vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, 0);
    
    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    if (!(features_hi & VIRTIO_F_VERSION_1_BIT))
        goto fail;
    uint32_t driver_hi = features_hi & VIRTIO_F_VERSION_1_BIT;
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, driver_hi);
    mb();
    
    status |= VIRTIO_STATUS_FEATURES_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    if (!(vt->read32(vt, VIRTIO_MMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
        goto fail;
    
    // Setup controlq (queue 0)
    vt->write32(vt, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = vt->read32(vt, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax < VIRTIO_GPU_QUEUE_SIZE)
        goto fail;
    if (vt->read32(vt, VIRTIO_MMIO_QUEUE_READY) != 0)
        goto fail;
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_GPU_QUEUE_SIZE);

    memset(inst->desc, 0, sizeof(inst->desc));
    memset(&inst->avail, 0, sizeof(inst->avail));
    memset(&inst->used, 0, sizeof(inst->used));
    arch_dma_sync_for_device(inst->desc, sizeof(inst->desc));
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    arch_dma_sync_for_device(&inst->used, sizeof(inst->used));
    
    uint64_t desc_pa  = va_to_pa(inst->desc);
    uint64_t avail_pa = va_to_pa(&inst->avail);
    uint64_t used_pa  = va_to_pa(&inst->used);
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_LOW,   (uint32_t)(desc_pa));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DESC_HIGH,  (uint32_t)(desc_pa >> 32));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_LOW, (uint32_t)(avail_pa));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DRIVER_HIGH,(uint32_t)(avail_pa >> 32));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_LOW, (uint32_t)(used_pa));
    vt->write32(vt, VIRTIO_MMIO_QUEUE_DEVICE_HIGH,(uint32_t)(used_pa >> 32));
    mb();
    vt->write32(vt, VIRTIO_MMIO_QUEUE_READY, 1);
    mb();
    
    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    
    inst->width = 1024;
    inst->height = 768;
    inst->bpp = 32;
    inst->fb_size = inst->width * inst->height * (inst->bpp / 8);
    
    // Allocate framebuffer as continuous physical memory
    size_t req_pages = inst->fb_size / PAGE_SIZE + ((inst->fb_size % PAGE_SIZE) ? 1 : 0);
    while ((1UL << order) < req_pages) {
        order++;
    }
    fb_pfn = pfa_alloc(order);
    if (fb_pfn == PFN_NONE) {
        goto fail;
    }
    inst->fb_phys = pfn_to_phys(fb_pfn);
    memset(pfn_to_virt(fb_pfn), 0, (size_t)PAGE_SIZE << order);
    arch_dma_sync_for_device(pfn_to_virt(fb_pfn), inst->fb_size);
    
    // CREATE_2D
    struct virtio_gpu_resource_create_2d create ALIGNED(64);
    memset(&create, 0, sizeof(create));
    create.hdr.type = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create.resource_id = 1;
    create.format = VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
    create.width = inst->width;
    create.height = inst->height;
    
    struct virtio_gpu_ctrl_hdr resp ALIGNED(64);
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &create, sizeof(create), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        goto fail;
    
    // ATTACH_BACKING
    struct {
        struct virtio_gpu_resource_attach_backing req;
        struct virtio_gpu_mem_entry entry;
    } attach ALIGNED(64);
    memset(&attach, 0, sizeof(attach));
    attach.req.hdr.type = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach.req.resource_id = 1;
    attach.req.nr_entries = 1;
    attach.entry.addr = inst->fb_phys;
    attach.entry.length = inst->fb_size;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &attach, sizeof(attach), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        goto fail;
    
    // SET_SCANOUT
    struct virtio_gpu_set_scanout scanout ALIGNED(64);
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.x = 0;
    scanout.r.y = 0;
    scanout.r.width = inst->width;
    scanout.r.height = inst->height;
    scanout.scanout_id = 0;
    scanout.resource_id = 1;
    
    memset(&resp, 0, sizeof(resp));
    if (virtio_gpu_send_cmd(inst, &scanout, sizeof(scanout), &resp, sizeof(resp)) < 0 ||
        resp.type != VIRTIO_GPU_RESP_OK_NODATA)
        goto fail;
    
    dev->drv_priv = inst;

    // Initial FLUSH to transfer data and trigger QEMU window resize
    if (gpu_flush(dev, 0, 0, inst->width, inst->height) < 0) {
        dev->drv_priv = NULL;
        goto fail;
    }

    inst->valid = 1;
    if (gpu_device_register(dev) < 0)
        goto fail;
    kinfo("[GPU] virtio-gpu ready: %dx%d (FB: %lu MB at 0x%lx)\n", 
          inst->width, inst->height, inst->fb_size/1024/1024, inst->fb_phys);
    return 0;

fail:
    vt->write32(vt, VIRTIO_MMIO_STATUS,
                vt->read32(vt, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED);
    if (fb_pfn != PFN_NONE)
        pfa_free(fb_pfn, order);
    memset(inst, 0, sizeof(*inst));
    return -1;
}

static int virtio_gpu_probe(device_t *dev) {
    resource_t *mmio_res = device_get_resource(dev, RES_MMIO, 0);
    if (!mmio_res)
        return -1;

    virtio_transport_t vt = {
        .read32 = virtio_gpu_mmio_read32,
        .write32 = virtio_gpu_mmio_write32,
        .priv = (void *)(uintptr_t)mmio_res->start,
        .legacy = 0,
        .irq = -1,
    };
    return virtio_gpu_init_transport(dev, &vt);
}

int virtio_gpu_init(void) {
    if (g_gpu_inst.valid)
        return 0;

    virtio_transport_t vt;
    if (arch_virtio_gpu_probe(0, &vt) != 0)
        return -1;

    memset(&g_gpu_pci_device, 0, sizeof(g_gpu_pci_device));
    g_gpu_pci_device.name = "virtio-gpu-pci";
    g_gpu_pci_device.drv = &virtio_gpu_driver;
    return virtio_gpu_init_transport(&g_gpu_pci_device, &vt);
}

static const device_id_t virtio_gpu_ids[] = {
    { .vendor = VENDOR_ANY, .device = 16 }, // VIRTIO_ID_GPU
    { 0 },
};

static driver_t virtio_gpu_driver = {
    .name       = "virtio-gpu",
    .id_table   = virtio_gpu_ids,
    .bus        = NULL,
    .probe      = virtio_gpu_probe,
    .class_ops  = &gpu_ops,
    .class_type = DEV_CLASS_DISPLAY,
};

DRIVER_REGISTER(virtio_gpu_driver);

struct device *virtio_gpu_get_dev(void) {
    return gpu_device_get_default();
}
