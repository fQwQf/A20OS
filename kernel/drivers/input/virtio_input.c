#include "drivers/input/virtio_input.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/bus/pci_bus.h"
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
#include "drivers/block/virtio_blk.h"

#define VIRTIO_INPUT_QUEUE_SIZE VIRTIO_QUEUE_SIZE
#define VIRTIO_INPUT_DMA_LINE   64

typedef struct {
    struct virtio_input_event event;
    uint8_t padding[VIRTIO_INPUT_DMA_LINE - sizeof(struct virtio_input_event)];
} virtio_input_event_slot_t;

typedef struct {
    virtio_transport_t vt;
    virtq_desc_t       desc[VIRTIO_INPUT_QUEUE_SIZE] ALIGNED(64);
    virtq_avail_t      avail ALIGNED(64);
    virtq_used_t       used ALIGNED(64);
    
    virtio_input_event_slot_t events[VIRTIO_INPUT_QUEUE_SIZE] ALIGNED(64);
    
    spinlock_t         lock ALIGNED(64);
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
static device_t g_input_pci_devices[MAX_VIRTIO_INPUT_DEVS];
static uint8_t g_input_irq_registered[256];
static unsigned g_input_trace_events;

static void virtio_input_mmio_write32(virtio_transport_t *t, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)((uintptr_t)t->priv + off));
}

static uint32_t virtio_input_mmio_read32(virtio_transport_t *t, uint32_t off) {
    return readl((const volatile void *)((uintptr_t)t->priv + off));
}

static void virtio_input_submit_all(virtio_input_inst_t *inst) {
    // Fill eventq with all available slots
    for (int i = 0; i < VIRTIO_INPUT_QUEUE_SIZE; i++) {
        inst->desc[i].addr = va_to_pa(&inst->events[i].event);
        inst->desc[i].len = sizeof(struct virtio_input_event);
        inst->desc[i].flags = VIRTQ_DESC_F_WRITE;
        inst->desc[i].next = 0;
        
        inst->avail.ring[inst->avail.idx % VIRTIO_INPUT_QUEUE_SIZE] = (uint16_t)i;
        inst->avail.idx++;
    }
    wmb();
    arch_dma_sync_for_device(inst->desc, sizeof(inst->desc));
    arch_dma_sync_for_device(inst->events, sizeof(inst->events));
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    arch_dma_sync_for_device(&inst->used, sizeof(inst->used));
    inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
    mb();
}

static void virtio_input_handle_inst(virtio_input_inst_t *inst) {
    if (!inst->valid)
        return;
    
    uint32_t isr = inst->vt.read32(&inst->vt, VIRTIO_MMIO_INTERRUPT_STATUS);
    if (!inst->vt.legacy)
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_INTERRUPT_ACK, isr);
        
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    
    volatile virtq_used_t *used = &inst->used;
    arch_dma_sync_for_cpu((void *)used, sizeof(*used));
    uint16_t used_idx = used->idx;
    
    int waked = 0;
    int resubmitted = 0;
    while (inst->last_used != used_idx) {
        uint16_t ring_idx = inst->last_used % VIRTIO_INPUT_QUEUE_SIZE;
        uint16_t id = (uint16_t)used->ring[ring_idx].id;
        if (id >= VIRTIO_INPUT_QUEUE_SIZE)
            break;
        
        struct virtio_input_event *evt = &inst->events[id].event;
        arch_dma_sync_for_cpu(evt, sizeof(*evt));

        if (g_input_trace_events < 8) {
            kinfo("[INPUT] event type=%u code=%u value=%d\n",
                  evt->type, evt->code, (int)evt->value);
            g_input_trace_events++;
        }
        
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
        arch_dma_sync_for_device(evt, sizeof(*evt));
        inst->avail.idx++;
        
        inst->last_used++;
        resubmitted = 1;
    }
    
    if (resubmitted) {
        wmb();
        arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
        inst->vt.write32(&inst->vt, VIRTIO_MMIO_QUEUE_NOTIFY, 0);
        mb();
    }
    if (waked) {
        if (inst->waiter && inst->waiter->state == PROC_BLOCKED) {
            proc_make_ready(inst->waiter);
        }
    }
    
    spin_unlock_irqrestore(&inst->lock, flags);
}

static int virtio_input_irq(int irq, void *priv) {
    (void)priv;

    /* QEMU's PCI virtio keyboard and mouse commonly share one INTx line.
     * The core IRQ table has one handler per line, so drain every input queue
     * attached to the asserted line instead of replacing the first handler. */
    for (int i = 0; i < MAX_VIRTIO_INPUT_DEVS; i++) {
        virtio_input_inst_t *inst = &g_input_insts[i];
        if (inst->valid && inst->vt.irq == irq)
            virtio_input_handle_inst(inst);
    }
    return 0;
}

static void virtio_input_poll(void) {
    for (int i = 0; i < g_ninputs; i++)
        virtio_input_handle_inst(&g_input_insts[i]);
}

static int input_read(vfile_t *vf, char *buf, size_t count) {
    if (count < sizeof(struct input_event)) return -EINVAL;
    
    while (1) {
        /* Keep the GUI responsive on QEMU variants whose virtio IRQ route is
         * masked or unavailable even though the queue itself is operational. */
        virtio_input_poll();
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

        if (vf->flags & O_NONBLOCK)
            return -EAGAIN;
        
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

static int virtio_input_init_transport(device_t *dev,
                                       const virtio_transport_t *transport) {
    if (g_ninputs >= MAX_VIRTIO_INPUT_DEVS) return -1;
    virtio_input_inst_t *inst = &g_input_insts[g_ninputs];

    memset(inst, 0, sizeof(*inst));
    spin_init(&inst->lock);
    inst->vt = *transport;
    
    virtio_transport_t *vt = &inst->vt;
    int registered_irq = 0;

    uint32_t magic = vt->read32(vt, VIRTIO_MMIO_MAGIC);
    uint32_t version = vt->read32(vt, VIRTIO_MMIO_VERSION);
    uint32_t device_id = vt->read32(vt, VIRTIO_MMIO_DEVICE_ID);
    if (magic != 0x74726976U || version != 2U || device_id != 18U)
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
    
    // Setup eventq (queue 0)
    vt->write32(vt, VIRTIO_MMIO_QUEUE_SEL, 0);
    uint32_t qmax = vt->read32(vt, VIRTIO_MMIO_QUEUE_NUM_MAX);
    if (qmax < VIRTIO_INPUT_QUEUE_SIZE)
        goto fail;
    if (vt->read32(vt, VIRTIO_MMIO_QUEUE_READY) != 0)
        goto fail;
    vt->write32(vt, VIRTIO_MMIO_QUEUE_NUM, VIRTIO_INPUT_QUEUE_SIZE);

    memset(inst->desc, 0, sizeof(inst->desc));
    memset(&inst->avail, 0, sizeof(inst->avail));
    memset(&inst->used, 0, sizeof(inst->used));
    memset(inst->events, 0, sizeof(inst->events));
    arch_dma_sync_for_device(inst->desc, sizeof(inst->desc));
    arch_dma_sync_for_device(&inst->avail, sizeof(inst->avail));
    arch_dma_sync_for_device(&inst->used, sizeof(inst->used));
    arch_dma_sync_for_device(inst->events, sizeof(inst->events));
    
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
    
    if (vt->irq >= 0 && vt->irq < (int)sizeof(g_input_irq_registered) &&
        !g_input_irq_registered[vt->irq]) {
        if (request_irq((uint32_t)vt->irq, virtio_input_irq, 0, NULL) != 0) {
            kinfo("[INPUT] Failed to register IRQ\n");
            goto fail;
        }
        g_input_irq_registered[vt->irq] = 1;
        registered_irq = 1;
    }
    
    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VIRTIO_MMIO_STATUS, status);
    mb();
    
    inst->valid = 1;
    dev->drv_priv = inst;
    virtio_input_submit_all(inst);
    g_ninputs++;

    kinfo("[INPUT] virtio-input ready (irq=%d)\n", vt->irq);
    return 0;

fail:
    if (registered_irq) {
        free_irq((uint32_t)vt->irq, inst);
        g_input_irq_registered[vt->irq] = 0;
    }
    vt->write32(vt, VIRTIO_MMIO_STATUS,
                vt->read32(vt, VIRTIO_MMIO_STATUS) | VIRTIO_STATUS_FAILED);
    memset(inst, 0, sizeof(*inst));
    return -1;
}

static int virtio_input_probe(device_t *dev) {
    if (dev->bus == &pci_bus) {
        virtio_transport_t vt;
        if (pci_virtio_transport_init(dev, 18, &vt) != 0)
            return -1;
        return virtio_input_init_transport(dev, &vt);
    }

    resource_t *mmio_res = device_get_resource(dev, RES_MMIO, 0);
    resource_t *irq_res = device_get_resource(dev, RES_IRQ, 0);
    if (!mmio_res || !irq_res)
        return -1;

    virtio_transport_t vt = {
        .read32 = virtio_input_mmio_read32,
        .write32 = virtio_input_mmio_write32,
        .priv = (void *)(uintptr_t)mmio_res->start,
        .legacy = 0,
        .irq = (int)irq_res->start,
    };
    return virtio_input_init_transport(dev, &vt);
}

int virtio_input_init(void) {
    if (g_ninputs > 0)
        return 0;

    int initialized = 0;
    for (int index = 0; index < MAX_VIRTIO_INPUT_DEVS; index++) {
        virtio_transport_t vt;
        if (arch_virtio_input_probe(index, &vt) != 0)
            break;

        device_t *dev = &g_input_pci_devices[index];
        memset(dev, 0, sizeof(*dev));
        dev->name = "virtio-input-pci";
        if (virtio_input_init_transport(dev, &vt) != 0)
            return initialized ? 0 : -1;
        initialized++;
    }
    return initialized ? 0 : -1;
}

static const device_id_t virtio_input_ids[] = {
    { .vendor = 0x1AF4, .device = 0x1052,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1012, .subvendor = VENDOR_ANY, .subdevice = 18 },
    { 0 },
};

static driver_t virtio_input_driver = {
    .name       = "virtio-input",
    .id_table   = virtio_input_ids,
    .bus        = NULL,
    .probe      = virtio_input_probe,
};

DRIVER_REGISTER(virtio_input_driver);
