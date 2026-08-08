/*
 * virtio-input full driver — drvmod module (riscv64/aarch64/x86_64/
 * loongarch64).  Migrated from kernel/drivers/input/virtio_input.c
 * (removed): the complete device init (status transitions, event
 * virtqueue) now runs as a module and publishes an input class device
 * through the unified driver core; the /dev/event0 multiplexer service
 * lives in kernel/drivers/input/input_mux.c and consumes the class
 * device.  The slot-5 dual-placement sample stays user-owned for
 * uinputd (udriver whitelist), so this driver binds every other
 * virtio-input device.
 *
 * The event queue DMA area (desc/avail/used/events) is one coherent
 * allocation, so descriptor addresses are computed from the DMA handle
 * instead of va_to_pa().
 */

#define DRV_ENV_DRVMOD 1
#include "drvmod/drvmod.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_INPUT, "virtio-input", A20_DRIVER_ABI, A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ,
                      0, 2,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_MMIO, 0, 18),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x1AF4, 0x1052));
#include "drivers/dual/virtio_mmio.h"
#include "drivers/input/virtio_input.h"

extern void input_mux_wake(void);
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"

#define kinfo(...) drv_log(__VA_ARGS__)
#define kerr(...) drv_log(__VA_ARGS__)

#define VIRTIO_INPUT_QUEUE_SIZE 32
#define VIRTIO_INPUT_DMA_LINE   64
#define VIRTQ_DESC_F_WRITE      2u

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} vinput_desc_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[VIRTIO_INPUT_QUEUE_SIZE];
    uint16_t used_event;
} vinput_avail_t;

typedef struct {
    uint32_t id;
    uint32_t len;
} vinput_used_elem_t;

typedef struct {
    uint16_t flags;
    uint16_t idx;
    vinput_used_elem_t ring[VIRTIO_INPUT_QUEUE_SIZE];
    uint16_t avail_event;
} vinput_used_t;

/* v2 queue address registers + feature bit (virtio_mmio.h keeps the
 * shared subset; these are the driver-only pieces). */
#define VMMIO_QUEUE_DESC_LOW    0x080u
#define VMMIO_QUEUE_DESC_HIGH   0x084u
#define VMMIO_QUEUE_DRIVER_LOW  0x090u
#define VMMIO_QUEUE_DRIVER_HIGH 0x094u
#define VMMIO_QUEUE_DEVICE_LOW  0x0a0u
#define VMMIO_QUEUE_DEVICE_HIGH 0x0a4u
#define VIRTIO_F_VERSION_1_BIT  0x1u
#define VIRTIO_STATUS_FAILED    128u

typedef struct {
    struct virtio_input_event event;
    uint8_t padding[VIRTIO_INPUT_DMA_LINE - sizeof(struct virtio_input_event)];
} virtio_input_event_slot_t;

/* Event queue DMA layout inside one coherent allocation (offsets). */
#define VIN_DMA_DESC_OFF   0x000U
#define VIN_DMA_AVAIL_OFF  0x200U
#define VIN_DMA_USED_OFF   0x280U
#define VIN_DMA_EVENTS_OFF 0x400U
#define VIN_DMA_BYTES      0x600U

typedef struct {
    virtio_transport_t vt;
    uint8_t           *dma;            /* coherent queue area */
    uint64_t           dma_phys;
    spinlock_t         lock ALIGNED(64);
    int                valid;
    uint16_t           last_used;
    struct input_event user_ring[256];
    uint32_t           head;
    uint32_t           tail;
    uint32_t           irq;
    int                irq_registered;
} virtio_input_inst_t;

#define MAX_VIRTIO_INPUT_DEVS 4
static virtio_input_inst_t g_input_insts[MAX_VIRTIO_INPUT_DEVS];
static int g_ninputs = 0;
static uint8_t g_input_irq_registered[256];
static unsigned g_input_trace_events;

static inline vinput_desc_t *vinput_desc(virtio_input_inst_t *inst) {
    return (vinput_desc_t *)(inst->dma + VIN_DMA_DESC_OFF);
}
static inline vinput_avail_t *vinput_avail(virtio_input_inst_t *inst) {
    return (vinput_avail_t *)(inst->dma + VIN_DMA_AVAIL_OFF);
}
static inline vinput_used_t *vinput_used(virtio_input_inst_t *inst) {
    return (vinput_used_t *)(inst->dma + VIN_DMA_USED_OFF);
}
static inline virtio_input_event_slot_t *vinput_events(virtio_input_inst_t *inst) {
    return (virtio_input_event_slot_t *)(inst->dma + VIN_DMA_EVENTS_OFF);
}

static uint32_t vinput_mmio_read32(virtio_transport_t *t, uint32_t off) {
    return readl((const volatile void *)((uintptr_t)t->priv + off));
}
static void vinput_mmio_write32(virtio_transport_t *t, uint32_t off, uint32_t val) {
    writel(val, (volatile void *)((uintptr_t)t->priv + off));
}

static void vinput_submit_all(virtio_input_inst_t *inst) {
    vinput_desc_t *desc = vinput_desc(inst);
    vinput_avail_t *avail = vinput_avail(inst);
    virtio_input_event_slot_t *events = vinput_events(inst);
    for (int i = 0; i < VIRTIO_INPUT_QUEUE_SIZE; i++) {
        desc[i].addr = inst->dma_phys + VIN_DMA_EVENTS_OFF +
                       (uint64_t)i * sizeof(virtio_input_event_slot_t);
        desc[i].len = sizeof(struct virtio_input_event);
        desc[i].flags = VIRTQ_DESC_F_WRITE;
        desc[i].next = 0;
        avail->ring[avail->idx % VIRTIO_INPUT_QUEUE_SIZE] = (uint16_t)i;
        avail->idx++;
    }
    wmb();
    arch_dma_sync_for_device(desc, sizeof(vinput_desc_t) * VIRTIO_INPUT_QUEUE_SIZE);
    arch_dma_sync_for_device(events, sizeof(virtio_input_event_slot_t) *
                                     VIRTIO_INPUT_QUEUE_SIZE);
    arch_dma_sync_for_device(avail, sizeof(*avail));
    arch_dma_sync_for_device(vinput_used(inst), sizeof(vinput_used_t));
    inst->vt.write32(&inst->vt, VMMIO_QUEUE_NOTIFY, 0);
    mb();
}

static void vinput_handle_inst(virtio_input_inst_t *inst) {
    if (!inst->valid)
        return;

    uint32_t isr = inst->vt.read32(&inst->vt, VMMIO_INTR_STATUS);
    if (!inst->vt.legacy)
        inst->vt.write32(&inst->vt, VMMIO_INTR_ACK, isr);

    uint64_t flags = spin_lock_irqsave(&inst->lock);

    volatile vinput_used_t *used = vinput_used(inst);
    arch_dma_sync_for_cpu((void *)used, sizeof(*used));
    uint16_t used_idx = used->idx;

    int waked = 0;
    int resubmitted = 0;
    while (inst->last_used != used_idx) {
        uint16_t ring_idx = inst->last_used % VIRTIO_INPUT_QUEUE_SIZE;
        uint16_t id = (uint16_t)used->ring[ring_idx].id;
        if (id >= VIRTIO_INPUT_QUEUE_SIZE)
            break;

        struct virtio_input_event *evt =
            &vinput_events(inst)[id].event;
        arch_dma_sync_for_cpu(evt, sizeof(*evt));

        if (g_input_trace_events < 8) {
            kinfo("[INPUT] event type=%u code=%u value=%d\n",
                  evt->type, evt->code, (int)evt->value);
            g_input_trace_events++;
        }

        uint32_t next_head = (inst->head + 1) % ARRAY_SIZE(inst->user_ring);
        if (next_head != inst->tail) {
            uint64_t now = timer_get_ticks();
            inst->user_ring[inst->head].time_sec = now / TICKS_PER_SEC;
            inst->user_ring[inst->head].time_usec =
                (now % TICKS_PER_SEC) * 1000000ULL / TICKS_PER_SEC;
            inst->user_ring[inst->head].type = evt->type;
            inst->user_ring[inst->head].code = evt->code;
            inst->user_ring[inst->head].value = evt->value;
            inst->head = next_head;
            waked = 1;
        }

        vinput_avail_t *avail = vinput_avail(inst);
        avail->ring[avail->idx % VIRTIO_INPUT_QUEUE_SIZE] = id;
        arch_dma_sync_for_device(evt, sizeof(*evt));
        avail->idx++;

        inst->last_used++;
        resubmitted = 1;
    }

    if (resubmitted) {
        wmb();
        arch_dma_sync_for_device(vinput_avail(inst), sizeof(vinput_avail_t));
        inst->vt.write32(&inst->vt, VMMIO_QUEUE_NOTIFY, 0);
        mb();
    }
    spin_unlock_irqrestore(&inst->lock, flags);
    if (waked)
        input_mux_wake();
}

static int vinput_irq(int irq, void *priv) {
    (void)irq;
    virtio_input_inst_t *inst = priv;
    if (inst && inst->valid)
        vinput_handle_inst(inst);
    return 1;
}

static int vinput_init_transport(device_t *dev,
                                 const virtio_transport_t *transport) {
    if (g_ninputs >= MAX_VIRTIO_INPUT_DEVS)
        return -1;
    virtio_input_inst_t *inst = NULL;
    for (int i = 0; i < MAX_VIRTIO_INPUT_DEVS; i++) {
        if (!g_input_insts[i].valid && !g_input_insts[i].irq_registered) {
            inst = &g_input_insts[i];
            break;
        }
    }
    if (!inst)
        return -1;

    memset(inst, 0, sizeof(*inst));
    spin_init(&inst->lock);
    inst->vt = *transport;

    virtio_transport_t *vt = &inst->vt;
    int registered_irq = 0;

    uint32_t magic = vt->read32(vt, VMMIO_MAGIC);
    uint32_t version = vt->read32(vt, VMMIO_VERSION);
    uint32_t device_id = vt->read32(vt, VMMIO_DEVICE_ID);
    if (magic != 0x74726976U || version != 2U || device_id != 18U)
        return -1;

    vt->write32(vt, VMMIO_STATUS, 0);
    mb();
    uint32_t status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
    vt->write32(vt, VMMIO_STATUS, status);
    mb();

    vt->write32(vt, VMMIO_DEV_FEATURES_SEL, 0);
    vt->read32(vt, VMMIO_DEV_FEATURES);
    vt->write32(vt, VMMIO_DRV_FEATURES_SEL, 0);
    vt->write32(vt, VMMIO_DRV_FEATURES, 0);

    vt->write32(vt, VMMIO_DEV_FEATURES_SEL, 1);
    uint32_t features_hi = vt->read32(vt, VMMIO_DEV_FEATURES);
    if (!(features_hi & VIRTIO_F_VERSION_1_BIT))
        goto fail;
    uint32_t driver_hi = features_hi & VIRTIO_F_VERSION_1_BIT;
    vt->write32(vt, VMMIO_DRV_FEATURES_SEL, 1);
    vt->write32(vt, VMMIO_DRV_FEATURES, driver_hi);
    mb();

    status |= VIRTIO_STATUS_FEATURES_OK;
    vt->write32(vt, VMMIO_STATUS, status);
    mb();
    if (!(vt->read32(vt, VMMIO_STATUS) & VIRTIO_STATUS_FEATURES_OK))
        goto fail;

    vt->write32(vt, VMMIO_QUEUE_SEL, 0);
    uint32_t qmax = vt->read32(vt, VMMIO_QUEUE_NUM_MAX);
    if (qmax < VIRTIO_INPUT_QUEUE_SIZE)
        goto fail;
    if (vt->read32(vt, VMMIO_QUEUE_READY) != 0)
        goto fail;
    vt->write32(vt, VMMIO_QUEUE_NUM, VIRTIO_INPUT_QUEUE_SIZE);

    inst->dma = dma_alloc_coherent_aligned(VIN_DMA_BYTES, PAGE_SIZE,
                                           &inst->dma_phys);
    if (!inst->dma)
        goto fail;
    memset(inst->dma, 0, VIN_DMA_BYTES);
    arch_dma_sync_for_device(inst->dma, VIN_DMA_BYTES);

    uint64_t desc_pa  = inst->dma_phys + VIN_DMA_DESC_OFF;
    uint64_t avail_pa = inst->dma_phys + VIN_DMA_AVAIL_OFF;
    uint64_t used_pa  = inst->dma_phys + VIN_DMA_USED_OFF;
    vt->write32(vt, VMMIO_QUEUE_DESC_LOW,   (uint32_t)(desc_pa));
    vt->write32(vt, VMMIO_QUEUE_DESC_HIGH,  (uint32_t)(desc_pa >> 32));
    vt->write32(vt, VMMIO_QUEUE_DRIVER_LOW, (uint32_t)(avail_pa));
    vt->write32(vt, VMMIO_QUEUE_DRIVER_HIGH,(uint32_t)(avail_pa >> 32));
    vt->write32(vt, VMMIO_QUEUE_DEVICE_LOW, (uint32_t)(used_pa));
    vt->write32(vt, VMMIO_QUEUE_DEVICE_HIGH,(uint32_t)(used_pa >> 32));
    mb();
    vt->write32(vt, VMMIO_QUEUE_READY, 1);
    mb();

    if (vt->irq >= 0 && vt->irq < (int)sizeof(g_input_irq_registered) &&
        !g_input_irq_registered[vt->irq]) {
        unsigned long irq_flags = vt->shared_irq ? IRQF_SHARED : 0;
        if (request_irq((uint32_t)vt->irq, vinput_irq, irq_flags, inst) != 0) {
            kinfo("[INPUT] Failed to register IRQ\n");
            goto fail;
        }
        g_input_irq_registered[vt->irq] = 1;
        registered_irq = 1;
    }
    inst->irq = vt->irq >= 0 ? (uint32_t)vt->irq : 0;
    inst->irq_registered = registered_irq;

    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VMMIO_STATUS, status);
    mb();

    inst->valid = 1;
    dev->drv_priv = inst;
    vinput_submit_all(inst);
    g_ninputs++;

    kinfo("[INPUT] virtio-input ready (irq=%d)\n", vt->irq);
    return 0;

fail:
    if (registered_irq) {
        free_irq((uint32_t)vt->irq, inst);
        g_input_irq_registered[vt->irq] = 0;
    }
    if (inst->dma)
        dma_free_coherent_aligned(inst->dma, VIN_DMA_BYTES, inst->dma_phys);
    vt->write32(vt, VMMIO_STATUS,
                vt->read32(vt, VMMIO_STATUS) | VIRTIO_STATUS_FAILED);
    memset(inst, 0, sizeof(*inst));
    return -1;
}

static int vinput_probe(device_t *dev) {
    if (dev->bus == &pci_bus) {
        virtio_transport_t vt;
        if (pci_virtio_transport_init(dev, 18, &vt) != 0)
            return -1;
        return vinput_init_transport(dev, &vt);
    }

    resource_t *mmio_res = drv_device_get_resource(dev, RES_MMIO, 0);
    resource_t *irq_res = drv_device_get_resource(dev, RES_IRQ, 0);
    if (!mmio_res || !irq_res)
        return -1;

    virtio_transport_t vt = {
        .read32 = vinput_mmio_read32,
        .write32 = vinput_mmio_write32,
        .priv = (void *)(uintptr_t)mmio_res->start,
        .legacy = 0,
        .irq = (int)irq_res->start,
    };
    return vinput_init_transport(dev, &vt);
}

static int vinput_remove(device_t *dev) {
    virtio_input_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst)
        return 0;
    inst->valid = 0;
    if (inst->irq_registered && inst->irq < sizeof(g_input_irq_registered)) {
        virtio_input_inst_t *new_owner = NULL;
        for (int i = 0; i < MAX_VIRTIO_INPUT_DEVS; i++) {
            if (&g_input_insts[i] != inst && g_input_insts[i].valid &&
                g_input_insts[i].vt.irq == (int)inst->irq) {
                new_owner = &g_input_insts[i];
                break;
            }
        }
        if (new_owner) {
            new_owner->irq_registered = 1;
        } else {
            free_irq(inst->irq, inst);
            g_input_irq_registered[inst->irq] = 0;
        }
    }
    inst->vt.write32(&inst->vt, VMMIO_STATUS, 0);
    mb();
    if (g_ninputs > 0)
        g_ninputs--;
    if (inst->dma)
        dma_free_coherent_aligned(inst->dma, VIN_DMA_BYTES, inst->dma_phys);
    dev->drv_priv = NULL;
    memset(inst, 0, sizeof(*inst));
    return 0;
}

static const device_id_t vinput_ids[] = {
    /* VirtIO-MMIO matches the protocol device type, not a PCI device ID. */
    { .vendor = 0, .device = 18,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1052,
      .subvendor = VENDOR_ANY, .subdevice = DEVICE_ANY },
    { .vendor = 0x1AF4, .device = 0x1012, .subvendor = VENDOR_ANY, .subdevice = 18 },
    { 0 },
};

static int vinput_class_read(device_t *dev, void *buf, size_t count) {
    virtio_input_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (!inst || !inst->valid || !buf || count < sizeof(struct input_event))
        return -EINVAL;

    vinput_handle_inst(inst);
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    size_t copied = 0;
    while (inst->head != inst->tail &&
           copied + sizeof(struct input_event) <= count) {
        *(struct input_event *)((char *)buf + copied) = inst->user_ring[inst->tail];
        inst->tail = (inst->tail + 1U) % ARRAY_SIZE(inst->user_ring);
        copied += sizeof(struct input_event);
    }
    spin_unlock_irqrestore(&inst->lock, flags);
    return copied ? (int)copied : -EAGAIN;
}

static int vinput_class_ioctl(device_t *dev, unsigned long req, void *arg) {
    (void)dev;
    (void)req;
    (void)arg;
    return -ENOSYS;
}

static int vinput_class_poll(device_t *dev, short events) {
    virtio_input_inst_t *inst = dev ? dev->drv_priv : NULL;
    (void)events;
    if (!inst || !inst->valid)
        return 0;
    vinput_handle_inst(inst);
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    int ready = inst->head != inst->tail;
    spin_unlock_irqrestore(&inst->lock, flags);
    return ready;
}

static const input_dev_ops_t vinput_class_ops = {
    .read = vinput_class_read,
    .ioctl = vinput_class_ioctl,
    .poll = vinput_class_poll,
};

static driver_t vinput_driver = {
    .name       = "virtio-input",
    .id_table   = vinput_ids,
    .bus        = NULL,
    .probe      = vinput_probe,
    .class_ops  = &vinput_class_ops,
    .remove     = vinput_remove,
    .class_type = DEV_CLASS_INPUT,
};

uintptr_t DriverEntry(void)
{
    int r = drv_driver_register(&vinput_driver);
    drv_log("[VINPUT] driver registered in core: %d\n", r);
    return r == 0 ? 0 : 1;
}
