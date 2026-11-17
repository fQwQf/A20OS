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

#ifdef CONFIG_DRIVER_DEPLOYMENT_EMBEDDED
#define DRV_ENV_KERNEL 1
#include "drivers/core/driver_register.h"
#else
#define DRV_ENV_DRVMOD 1
#include "drvmod/drvmod.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_INPUT, "virtio-input", A20_DRIVER_ABI, A20_DRIVER_RES_MMIO | A20_DRIVER_RES_IRQ,
                      0, 2,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_MMIO, 0, 18),
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x1AF4, 0x1052));
#endif
#include "drivers/dual/virtio_mmio.h"
#include "drivers/input/virtio_input.h"

extern void input_mux_wake(void);
#include "drivers/bus/pci_bus.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "drivers/input/evdev.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "core/timer.h"
#include "proc/proc.h"

#ifdef CONFIG_DRIVER_DEPLOYMENT_EMBEDDED
#define drv_device_get_resource device_get_resource
#else
#undef kinfo
#undef kerr
#define kinfo(...) drv_log(__VA_ARGS__)
#define kerr(...) drv_log(__VA_ARGS__)
#endif

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
#define VIN_DMA_BYTES (VIN_DMA_EVENTS_OFF + VIRTIO_INPUT_QUEUE_SIZE * \
                       sizeof(virtio_input_event_slot_t))

_Static_assert(VIN_DMA_DESC_OFF + sizeof(vinput_desc_t) * VIRTIO_INPUT_QUEUE_SIZE <=
               VIN_DMA_AVAIL_OFF, "input descriptor/available overlap");
_Static_assert(VIN_DMA_AVAIL_OFF + sizeof(vinput_avail_t) <= VIN_DMA_USED_OFF,
               "input available/used overlap");
_Static_assert(VIN_DMA_USED_OFF + sizeof(vinput_used_t) <= VIN_DMA_EVENTS_OFF,
               "input used/event overlap");

typedef struct {
    virtio_transport_t vt;
    uint8_t           *dma;            /* coherent queue area */
    uint64_t           dma_phys;
    spinlock_t         lock ALIGNED(64);
    int                valid;
    device_t          *owner;
    int                closing;
    int                ready;
    int                transport_live;
    int                stopped;
    uint16_t           last_used;
    struct input_event user_ring[256];
    uint32_t           head;
    uint32_t           tail;
    uint32_t           irq;
    int                irq_registered;
} virtio_input_inst_t;

#define MAX_VIRTIO_INPUT_DEVS 4
static virtio_input_inst_t g_input_insts[MAX_VIRTIO_INPUT_DEVS];
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

static void vinput_prepare_queue(virtio_input_inst_t *inst) {
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
}

static int vinput_ack(virtio_input_inst_t *inst)
{
    uint32_t isr = inst->vt.read32(&inst->vt, VMMIO_INTR_STATUS);
    if (isr == 0xFFFFFFFFU) {
        __atomic_store_n(&inst->closing, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&inst->ready, 0, __ATOMIC_RELEASE);
        return -ENODEV;
    }
    inst->vt.write32(&inst->vt, VMMIO_INTR_ACK, isr);
    return 0;
}

static void vinput_handle_inst(virtio_input_inst_t *inst)
{
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    if (__atomic_load_n(&inst->closing, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&inst->ready, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&inst->lock, flags);
        return;
    }
    if (vinput_ack(inst)) {
        spin_unlock_irqrestore(&inst->lock, flags);
        return;
    }
    volatile vinput_used_t *used = vinput_used(inst);
    arch_dma_sync_for_cpu((void *)used, sizeof(*used));
    uint16_t used_idx = used->idx;
    unsigned completed = (uint16_t)(used_idx - inst->last_used);
    uint32_t seen = 0;
    /* Validate the complete snapshot before consuming or re-publishing any
     * buffer. Narrowing id to 16 bits could accept an invalid 32-bit id. */
    if (completed > VIRTIO_INPUT_QUEUE_SIZE)
        goto malformed;
    for (unsigned n = 0; n < completed; n++) {
        unsigned at = (inst->last_used + n) % VIRTIO_INPUT_QUEUE_SIZE;
        uint32_t id = used->ring[at].id;
        if (id >= VIRTIO_INPUT_QUEUE_SIZE ||
            used->ring[at].len != sizeof(struct virtio_input_event) ||
            (seen & (1U << id)))
            goto malformed;
        seen |= 1U << id;
    }
    int waked = 0;
    while (inst->last_used != used_idx) {
        unsigned at = inst->last_used % VIRTIO_INPUT_QUEUE_SIZE;
        uint32_t id = used->ring[at].id;
        struct virtio_input_event *evt = &vinput_events(inst)[id].event;
        arch_dma_sync_for_cpu(evt, sizeof(*evt));
        if (__atomic_fetch_add(&g_input_trace_events, 1, __ATOMIC_RELAXED) < 8) {
            kinfo("[INPUT] event type=%u code=%u value=%d\n",
                  evt->type, evt->code, (int)evt->value);
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
        avail->ring[avail->idx % VIRTIO_INPUT_QUEUE_SIZE] = (uint16_t)id;
        arch_dma_sync_for_device(evt, sizeof(*evt));
        wmb();
        avail->idx++;
        inst->last_used++;
    }
    if (completed) {
        wmb();
        arch_dma_sync_for_device(vinput_avail(inst), sizeof(vinput_avail_t));
        inst->vt.write32(&inst->vt, VMMIO_QUEUE_NOTIFY, 0);
        mb();
    }
    spin_unlock_irqrestore(&inst->lock, flags);
    if (waked)
        input_mux_wake();
    return;
malformed:
    __atomic_store_n(&inst->closing, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&inst->ready, 0, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&inst->lock, flags);
}

static int vinput_irq(int irq, void *priv)
{
    (void)irq;
    virtio_input_inst_t *inst = priv;
    if (!inst)
        return 0;
    if (__atomic_load_n(&inst->closing, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&inst->ready, __ATOMIC_ACQUIRE)) {
        /* A refused reset retains this handler and its transport. Quench late
         * causes without touching or returning any queue storage. */
        if (inst->transport_live)
            (void)vinput_ack(inst);
    } else {
        vinput_handle_inst(inst);
    }
    return 1;
}

static int vinput_stop(void *context)
{
    virtio_input_inst_t *inst = context;
    __atomic_store_n(&inst->closing, 1, __ATOMIC_RELEASE);
    uint64_t flags;
    if (!spin_trylock_irqsave(&inst->lock, &flags))
        return -EBUSY;
    __atomic_store_n(&inst->ready, 0, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&inst->lock, flags);
    /* No spinlock or IRQ-off wait across transport reset/free_irq drain. */
    if (inst->transport_live) {
        int ret = virtio_transport_reset(&inst->vt, 100);
        if (ret)
            return ret;
        inst->transport_live = 0;
    }
    if (inst->irq_registered) {
        free_irq(inst->irq, inst);
        inst->irq_registered = 0;
    }
    inst->stopped = 1;
    return 0;
}

static void vinput_release(void *context)
{
    virtio_input_inst_t *inst = context;
    /* Final devres release follows checked stop, IRQ drain, class active-call
     * drain and remove. Until then even a failed probe owns its exact slot. */
    if (inst->dma)
        dma_free_coherent_aligned(inst->dma, VIN_DMA_BYTES, inst->dma_phys);
    memset(inst, 0, sizeof(*inst));
}

static int vinput_init_transport(device_t *dev, virtio_input_inst_t *inst)
{
    virtio_transport_t *vt = &inst->vt;
    uint32_t magic = vt->read32(vt, VMMIO_MAGIC);
    uint32_t version = vt->read32(vt, VMMIO_VERSION);
    uint32_t device_id = vt->read32(vt, VMMIO_DEVICE_ID);
    if (magic != 0x74726976U || version != 2U || device_id != 18U)
        return -ENODEV;
    inst->transport_live = 1;
    int ret = virtio_transport_reset(vt, 100);
    if (ret)
        return ret;
    ret = dma_set_mask(dev, DMA_MASK_64BIT);
    if (ret)
        return ret;
    uint32_t status = VIRTIO_STATUS_ACK | VIRTIO_STATUS_DRIVER;
    vt->write32(vt, VMMIO_STATUS, status);
    vt->write32(vt, VMMIO_DRV_FEATURES_SEL, 0);
    vt->write32(vt, VMMIO_DRV_FEATURES, 0);
    vt->write32(vt, VMMIO_DEV_FEATURES_SEL, 1);
    uint32_t features_hi = vt->read32(vt, VMMIO_DEV_FEATURES);
    if (!(features_hi & VIRTIO_F_VERSION_1_BIT))
        return -EOPNOTSUPP;
    vt->write32(vt, VMMIO_DRV_FEATURES_SEL, 1);
    vt->write32(vt, VMMIO_DRV_FEATURES, VIRTIO_F_VERSION_1_BIT);
    status |= VIRTIO_STATUS_FEATURES_OK;
    vt->write32(vt, VMMIO_STATUS, status);
    mb();
    if (vt->read32(vt, VMMIO_STATUS) != status)
        return -EIO;
    vt->write32(vt, VMMIO_QUEUE_SEL, 0);
    uint32_t qmax = vt->read32(vt, VMMIO_QUEUE_NUM_MAX);
    if (qmax == 0xFFFFFFFFU || qmax < VIRTIO_INPUT_QUEUE_SIZE ||
        vt->read32(vt, VMMIO_QUEUE_READY) != 0)
        return -ENODEV;
    vt->write32(vt, VMMIO_QUEUE_NUM, VIRTIO_INPUT_QUEUE_SIZE);
    inst->dma = dma_alloc_coherent_aligned(dev, VIN_DMA_BYTES, PAGE_SIZE,
                                           &inst->dma_phys);
    if (!inst->dma)
        return -ENOMEM;
    memset(inst->dma, 0, VIN_DMA_BYTES);
    arch_dma_sync_for_device(inst->dma, VIN_DMA_BYTES);
    vinput_prepare_queue(inst);
    uint64_t desc_pa = inst->dma_phys + VIN_DMA_DESC_OFF;
    uint64_t avail_pa = inst->dma_phys + VIN_DMA_AVAIL_OFF;
    uint64_t used_pa = inst->dma_phys + VIN_DMA_USED_OFF;
    vt->write32(vt, VMMIO_QUEUE_DESC_LOW, (uint32_t)desc_pa);
    vt->write32(vt, VMMIO_QUEUE_DESC_HIGH, (uint32_t)(desc_pa >> 32));
    vt->write32(vt, VMMIO_QUEUE_DRIVER_LOW, (uint32_t)avail_pa);
    vt->write32(vt, VMMIO_QUEUE_DRIVER_HIGH, (uint32_t)(avail_pa >> 32));
    vt->write32(vt, VMMIO_QUEUE_DEVICE_LOW, (uint32_t)used_pa);
    vt->write32(vt, VMMIO_QUEUE_DEVICE_HIGH, (uint32_t)(used_pa >> 32));
    mb();
    vt->write32(vt, VMMIO_QUEUE_READY, 1);
    if (vt->read32(vt, VMMIO_QUEUE_READY) != 1)
        return -EIO;
    if (vt->irq >= 0) {
        unsigned long irq_flags = vt->shared_irq ? IRQF_SHARED : 0;
        ret = request_irq((uint32_t)vt->irq, vinput_irq, irq_flags, inst);
        if (ret)
            return ret;
        inst->irq = (uint32_t)vt->irq;
        inst->irq_registered = 1;
    }
    status |= VIRTIO_STATUS_DRIVER_OK;
    vt->write32(vt, VMMIO_STATUS, status);
    mb();
    if (vt->read32(vt, VMMIO_STATUS) != status)
        return -EIO;
    __atomic_store_n(&inst->ready, 1, __ATOMIC_RELEASE);
    vt->write32(vt, VMMIO_QUEUE_NOTIFY, 0);
    kinfo("[INPUT] virtio-input ready (irq=%d)\n", vt->irq);
    return 0;
}

static int vinput_probe(device_t *dev)
{
    int ret = device_probe_use_remove(dev);
    if (ret)
        return ret;
    virtio_input_inst_t *inst = NULL;
    for (int i = 0; i < MAX_VIRTIO_INPUT_DEVS; i++) {
        if (!g_input_insts[i].valid) {
            inst = &g_input_insts[i];
            break;
        }
    }
    if (!inst)
        return -ENOSPC;
    memset(inst, 0, sizeof(*inst));
    spin_init(&inst->lock);
    inst->valid = 1;
    inst->owner = dev;
    inst->vt.irq = -1;
    dev->drv_priv = inst;
    ret = device_add_execution_action(dev, vinput_stop, vinput_release, inst);
    if (ret) {
        dev->drv_priv = NULL;
        vinput_release(inst);
        return ret;
    }
    if (dev->bus == &pci_bus) {
        ret = pci_virtio_transport_init(dev, 18, &inst->vt);
        if (ret)
            return ret;
    } else {
        resource_t *mmio = drv_device_get_resource(dev, RES_MMIO, 0);
        resource_t *irq = drv_device_get_resource(dev, RES_IRQ, 0);
        if (!mmio || mmio->end < mmio->start ||
            mmio->end - mmio->start < VMMIO_CONFIG + 135)
            return -ENODEV;
        inst->vt.priv = device_map_mmio(dev, 0);
        if (!inst->vt.priv)
            return -ENODEV;
        inst->vt.read32 = vinput_mmio_read32;
        inst->vt.write32 = vinput_mmio_write32;
        inst->vt.irq = irq ? (int)irq->start : -1;
    }
    if (!inst->vt.priv)
        return -ENODEV;
    return vinput_init_transport(dev, inst);
}

static int vinput_remove(device_t *dev)
{
    virtio_input_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (inst && !inst->stopped)
        return -EBUSY;
    if (dev)
        dev->drv_priv = NULL;
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
    if (!inst || !buf || count < sizeof(struct input_event))
        return -EINVAL;

    vinput_handle_inst(inst);
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    if (__atomic_load_n(&inst->closing, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&inst->ready, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&inst->lock, flags);
        return -ESHUTDOWN;
    }
    size_t copied = 0;
    while (inst->head != inst->tail &&
           copied + sizeof(struct input_event) <= count) {
        memcpy((char *)buf + copied, &inst->user_ring[inst->tail], sizeof(struct input_event));
        inst->tail = (inst->tail + 1U) % ARRAY_SIZE(inst->user_ring);
        copied += sizeof(struct input_event);
    }
    spin_unlock_irqrestore(&inst->lock, flags);
    return copied ? (int)copied : -EAGAIN;
}

/* virtio_input_config selectors are transport-independent device config. */
static unsigned vinput_config(virtio_input_inst_t *inst, unsigned select,
                              unsigned subselect, void *out, unsigned capacity)
{
    if (__atomic_load_n(&inst->closing, __ATOMIC_ACQUIRE))
        return 0;
    virtio_transport_t *vt = &inst->vt;
    vt->write32(vt, VMMIO_CONFIG, select | (subselect << 8));
    uint32_t config = vt->read32(vt, VMMIO_CONFIG);
    if (config == 0xFFFFFFFFU) {
        __atomic_store_n(&inst->closing, 1, __ATOMIC_RELEASE);
        __atomic_store_n(&inst->ready, 0, __ATOMIC_RELEASE);
        return 0;
    }
    unsigned size = (config >> 16) & 0xff;
    if (size > capacity)
        size = capacity;
    for (unsigned i = 0; i < size; i += 4) {
        uint32_t word = vt->read32(vt, VMMIO_CONFIG + 8 + i);
        unsigned n = size - i < 4 ? size - i : 4;
        memcpy((uint8_t *)out + i, &word, n);
    }
    return size;
}

static int vinput_class_ioctl(device_t *dev, unsigned long req, void *arg)
{
    virtio_input_inst_t *inst = dev ? dev->drv_priv : NULL;
    if (req != INPUT_IOCTL_GET_INFO)
        return -ENOTTY;
    if (!inst || !arg)
        return -ENODEV;
    struct input_device_info *info = arg;
    memset(info, 0, sizeof(*info));
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    if (__atomic_load_n(&inst->closing, __ATOMIC_ACQUIRE) ||
        !__atomic_load_n(&inst->ready, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&inst->lock, flags);
        return -ESHUTDOWN;
    }
    vinput_config(inst, 1, 0, info->name, sizeof(info->name) - 1);
    vinput_config(inst, 3, 0, &info->id, sizeof(info->id));
    vinput_config(inst, 0x10, 0, info->properties, sizeof(info->properties));
    input_info_bit(info, EV_SYN, SYN_REPORT);
    for (unsigned type = 1; type < INPUT_EVENT_TYPES; type++) {
        unsigned n = vinput_config(inst, 0x11, type, info->bits[type],
                                    INPUT_BITMAP_BYTES);
        if (n)
            info->bits[0][type >> 3] |= 1U << (type & 7);
    }
    for (unsigned axis = 0; axis < 64; axis++) {
        if (!(info->bits[EV_ABS][axis >> 3] & (1U << (axis & 7))))
            continue;
        /* virtio absinfo omits Linux input_absinfo.value. */
        vinput_config(inst, 0x12, axis, &info->abs[axis].minimum, 20);
    }
    int ret = __atomic_load_n(&inst->closing, __ATOMIC_ACQUIRE) ? -ENODEV : 0;
    spin_unlock_irqrestore(&inst->lock, flags);
    return ret;
}

static int vinput_class_poll(device_t *dev, short events) {
    virtio_input_inst_t *inst = dev ? dev->drv_priv : NULL;
    (void)events;
    if (!inst)
        return 0;
    vinput_handle_inst(inst);
    uint64_t flags = spin_lock_irqsave(&inst->lock);
    int ready = !__atomic_load_n(&inst->closing, __ATOMIC_ACQUIRE) &&
        __atomic_load_n(&inst->ready, __ATOMIC_ACQUIRE) && inst->head != inst->tail;
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
    .irq_optional = 1,
    .id_table   = vinput_ids,
    .bus        = NULL,
    .probe      = vinput_probe,
    .class_ops  = &vinput_class_ops,
    .remove     = vinput_remove,
    .class_type = DEV_CLASS_INPUT,
};

#ifdef CONFIG_DRIVER_DEPLOYMENT_EMBEDDED
DRIVER_REGISTER(vinput_driver);
#else
uintptr_t DriverEntry(void)
{
    int r = drv_driver_register(&vinput_driver);
    drv_log("[VINPUT] driver registered in core: %d\n", r);
    return r == 0 ? 0 : 1;
}
#endif
