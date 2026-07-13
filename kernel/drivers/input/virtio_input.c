#include "drivers/input/virtio_input.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
#include "fs/devfs.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "proc/proc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "core/lock.h"
#include "abi/linux/errno.h"

#include "abi/linux/errno.h"
#include "drivers/block/virtio_blk.h"
#include "fs/devfs.h"

#define VIRTIO_INPUT_QUEUE_SIZE VIRTIO_QUEUE_SIZE

typedef struct {
    virtio_transport_t vt;
    virtq_desc_t       desc[VIRTIO_INPUT_QUEUE_SIZE]  ALIGNED(16);
    virtq_avail_t      avail;
    virtq_used_t       used;
    
    struct virtio_input_event events[VIRTIO_INPUT_QUEUE_SIZE];
    
    spinlock_t         lock;
    int                valid;
    uint16_t           desc_idx;
    uint16_t           last_used;
    
    // Ring buffer for userspace
    struct input_event user_ring[256];
    uint32_t           head;
    uint32_t           tail;
    task_t            *waiter;
} virtio_input_inst_t;

#define MAX_VIRTIO_INPUT_DEVS 4
static virtio_input_inst_t g_input_insts[MAX_VIRTIO_INPUT_DEVS];
static int g_ninputs = 0;

static virtio_input_inst_t *virtio_input_get_inst(device_t *dev) {
    return (virtio_input_inst_t *)dev->drv_priv;
}

static void virtio_input_mmio_write32(virtio_transport_t *t, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)((uintptr_t)t->priv + off));
}

static uint32_t virtio_input_mmio_read32(virtio_transport_t *t, uint32_t off) {
    return readl((const volatile void *)((uintptr_t)t->priv + off));
}

static void virtio_input_submit_all(virtio_input_inst_t *inst) {
    // Fill eventq with all available slots
    for (int i = 0; i < VIRTIO_INPUT_QUEUE_SIZE; i++) {
        inst->desc[i].addr = va_to_pa(&inst->events[i]);
        inst->desc[i].len = sizeof(struct virtio_input_event);
        inst->desc[i].flags = VIRTQ_DESC_F_WRITE;
        inst->desc[i].next = 0;
        
        inst->avail.ring[inst->avail.idx % VIRTIO_INPUT_QUEUE_SIZE] = (uint16_t)i;
        inst->avail.idx++;
    }
    wmb();
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();
}

static int virtio_input_irq(int irq, void *priv) {
    (void)irq;
    virtio_input_inst_t *inst = (virtio_input_inst_t *)priv;
    if (!inst->valid) return 0;
    
    uint32_t isr = inst->vt.read32(&inst->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!inst->vt.legacy)
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);
        
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    
    volatile virtq_used_t *used = &inst->used;
    arch_dma_sync_for_cpu((void *)&used->idx, sizeof(uint16_t));
    
    int waked = 0;
    while (inst->last_used != used->idx) {
        uint16_t ring_idx = inst->last_used % VIRTIO_INPUT_QUEUE_SIZE;
        arch_dma_sync_for_cpu((void *)&used->ring[ring_idx], sizeof(virtq_used_elem_t));
        uint16_t id = (uint16_t)used->ring[ring_idx].id;
        
        struct virtio_input_event *evt = &inst->events[id];
        arch_dma_sync_for_cpu(evt, sizeof(*evt));
        
        // Add to user ring
        uint32_t next_head = (inst->head + 1) % 256;
        if (next_head != inst->tail) {
            inst->user_ring[inst->head].time_sec = 0;
            inst->user_ring[inst->head].time_usec = 0;
            inst->user_ring[inst->head].type = evt->type;
            inst->user_ring[inst->head].code = evt->code;
            inst->user_ring[inst->head].value = evt->value;
            inst->head = next_head;
            waked = 1;
        }
        
        // Re-submit
        inst->avail.ring[inst->avail.idx % VIRTIO_INPUT_QUEUE_SIZE] = id;
        inst->avail.idx++;
        
        inst->last_used++;
    }
    
    if (waked) {
        wmb();
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
        mb();
        if (inst->waiter && inst->waiter->state == PROC_BLOCKED) {
            proc_make_ready(inst->waiter);
        }
    }
    
    spin_unlock_irqrestore(&inst->lock, flags);
    return 0;
}

static int input_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    if (count < sizeof(struct input_event)) return -EINVAL;
    
    while (1) {
        for (int i = 0; i < g_ninputs; i++) {
            virtio_input_inst_t *inst = &g_input_insts[i];
            if (!inst->valid) continue;
            
            uint64_t flags = spin_lock_irqsave(&inst->lock);
            if (inst->head != inst->tail) {
                size_t copied = 0;
                while (inst->head != inst->tail && copied + sizeof(struct input_event) <= count) {
                    struct input_event *evt = (struct input_event *)(buf + copied);
                    *evt = inst->user_ring[inst->tail];
                    inst->tail = (inst->tail + 1) % 256;
                    copied += sizeof(struct input_event);
                }
                spin_unlock_irqrestore(&inst->lock, flags);
                return copied;
            }
            spin_unlock_irqrestore(&inst->lock, flags);
        }
        
        // No data, block on the first device's waiter
        if (g_ninputs > 0) {
            virtio_input_inst_t *inst = &g_input_insts[0];
            uint64_t flags = spin_lock_irqsave(&inst->lock);
            if (inst->head == inst->tail) {
                inst->waiter = proc_current();
                spin_unlock_irqrestore(&inst->lock, flags);
                sched();
                flags = spin_lock_irqsave(&inst->lock);
                inst->waiter = NULL;
                spin_unlock_irqrestore(&inst->lock, flags);
            } else {
                spin_unlock_irqrestore(&inst->lock, flags);
            }
        } else {
            return -EAGAIN;
        }
    }
}

static int input_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    (void)vf; (void)req; (void)arg;
    return -ENOSYS;
}

vfile_ops_t g_devfs_input_ops = {
    .read  = input_read,
    .ioctl = input_ioctl,
};

static int virtio_input_probe(device_t *dev) {
    if (g_ninputs >= MAX_VIRTIO_INPUT_DEVS) return -1;
    virtio_input_inst_t *inst = &g_input_insts[g_ninputs++];

    resource_t *mmio_res = device_get_resource(dev, RES_MMIO, 0);
    if (!mmio_res) return -1;
    
    
    memset(inst, 0, sizeof(*inst));
    spin_init(&inst->lock);
    
    inst->vt.read32  = virtio_input_mmio_read32;
    inst->vt.write32 = virtio_input_mmio_write32;
    inst->vt.priv    = (void *)(uintptr_t)mmio_res->start;
    inst->vt.legacy  = 0;
    
    virtio_transport_t *vt = &inst->vt;
    
    vt->write32(vt, VIRTIO_MMIO_STATUS, 0);
    mb();
    uint32_t status = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    
    vt->write32(vt, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
    uint32_t features_hi = vt->read32(vt, VIRTIO_MMIO_DEVICE_FEATURES);
    uint32_t driver_hi = features_hi & VIRTIO_F_VERSION_1_BIT;
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
    vt->write32(vt, VIRTIO_MMIO_DRIVER_FEATURES, driver_hi);
    mb();
    
    status |= VIRTIO_STATUS_FEATURES_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    
    // Setup eventq (queue 0)
    vt->write32(vt, VIRTIO_MMIO_QUEUE_SEL, 0);
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_INPUT_QUEUE_SIZE);
    
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
    
    resource_t *irq_res = device_get_resource(dev, RES_IRQ, 0);
    if (irq_res) {
        if (request_irq((uint32_t)irq_res->start, virtio_input_irq, 0, inst) != 0) {
            kinfo("[INPUT] Failed to register IRQ %d\n", irq_res->start);
        }
    }
    
    virtio_input_submit_all(inst);
    
    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    
    inst->valid = 1;
    dev->drv_priv = inst;

    kinfo("[INPUT] virtio-input ready at 0x%lx\n", mmio_res->start);
    return 0;
}

static const device_id_t virtio_input_ids[] = {
    { .vendor = VENDOR_ANY, .device = 18 }, // VIRTIO_ID_INPUT
    { 0 },
};

static driver_t virtio_input_driver = {
    .name       = "virtio-input",
    .id_table   = virtio_input_ids,
    .bus        = NULL,
    .probe      = virtio_input_probe,
};

DRIVER_REGISTER(virtio_input_driver);
