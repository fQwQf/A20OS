#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_DRIVER_DEPLOYMENT_EMBEDDED 1
#define CONFIG_64BIT 1
#define PAGE_SIZE 4096
#define TICKS_PER_SEC 1000
#define ALIGNED(n) __attribute__((aligned(n)))
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define DMA_MASK_64BIT UINT64_MAX
#define IRQF_SHARED 1
#define RES_MMIO 1
#define RES_IRQ 2
#define DEV_CLASS_INPUT 4
#define VENDOR_ANY 0xffff
#define DEVICE_ANY 0xffff
#define DRIVER_REGISTER(d)
#define mb() ((void)0)
#define wmb() ((void)0)
typedef struct { int held; } spinlock_t;
static unsigned irq_depth;
static void spin_init(spinlock_t *lock) { lock->held = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!lock->held && !irq_depth); lock->held = 1; irq_depth++; return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags; assert(lock->held && irq_depth == 1); lock->held = 0; irq_depth--;
}
static int spin_trylock_irqsave(spinlock_t *lock, uint64_t *flags) {
    if (lock->held) return 0;
    *flags = spin_lock_irqsave(lock); return 1;
}
typedef struct { uint64_t start, end; } resource_t;
typedef struct device {
    void *drv_priv, *bus;
    int number, managed, mapped, stopped;
    int (*stop)(void *);
    void (*release)(void *);
    void *context;
    resource_t mmio, irq;
} device_t;
typedef struct { uint16_t vendor, device, subvendor, subdevice; } device_id_t;
typedef struct {
    int (*read)(device_t *, void *, size_t);
    int (*ioctl)(device_t *, unsigned long, void *);
    int (*poll)(device_t *, short);
} input_dev_ops_t;
typedef struct {
    const char *name;
    const device_id_t *id_table;
    void *bus;
    int irq_optional;
    int (*probe)(device_t *), (*remove)(device_t *);
    const input_dev_ops_t *class_ops;
    int class_type;
} driver_t;
static int pci_bus;
static void host_log(const char *format, ...) { (void)format; }
#define kinfo host_log
#define kerr host_log
/* TRANSPORT */
/* EVENTS */
/* INPUT */
/* EVDEV */
/* REGISTERS */
#define VIRTIO_MMIO_STATUS VMMIO_STATUS
static uint64_t timer_get_ticks(void);
static uint64_t clock_get_ticks(void);
static uint64_t clock_ticks_per_sec(void) { return 1000; }
static void arch_cpu_relax(void) { assert(!irq_depth); }
static void *proc_current(void) { return NULL; }
static void proc_yield(void) { assert(!irq_depth); }
static uint32_t readl(const volatile void *);
static void writel(uint32_t, volatile void *);
static resource_t *device_get_resource(device_t *, int, int);
static void *device_map_mmio(device_t *, int);
static int device_probe_use_remove(device_t *dev) { dev->managed = 1; return 0; }
static int device_add_execution_action(device_t *, int (*)(void *), void (*)(void *), void *);
static int pci_virtio_transport_init(device_t *, int, virtio_transport_t *);
static int dma_set_mask(device_t *, uint64_t);
static void *dma_alloc_coherent_aligned(device_t *, size_t, size_t, uint64_t *);
static void dma_free_coherent_aligned(void *, size_t, uint64_t);
static void arch_dma_sync_for_device(const void *, size_t);
static void arch_dma_sync_for_cpu(const void *, size_t);
static int request_irq(uint32_t, int (*)(int, void *), unsigned long, void *);
static void free_irq(uint32_t, void *);
/* RESET */
/* PRODUCTION */

#define BASE UINT64_C(0x10000000)
typedef struct {
    uint32_t status, ready, feature_select, selector;
    uint64_t desc, avail, used;
    unsigned notifications, resets;
    int hold_reset, dead, refuse_features, refuse_queue, refuse_driver;
    int fail_map, fail_pci, fail_alloc, fail_irq, fail_gate;
    int mismatch, active;
    uint8_t *dma;
    size_t bytes;
    unsigned isr;
} wire_t;
static wire_t wires[4];
static device_t devices[4];
typedef struct { int (*handler)(int, void *); void *context; } irq_slot_t;
static irq_slot_t handlers[4];
static uint64_t ticks;
static unsigned allocated, freed, irq_freed, releases, unmaps, reads, writes, syncs, wakes;
static int fail_action;
static uint64_t timer_get_ticks(void) { return ticks++; }
static uint64_t clock_get_ticks(void) { assert(!irq_depth); return ticks++; }
void input_mux_wake(void) { wakes++; }
static wire_t *wire_for_pointer(const volatile void *pointer, unsigned *off) {
    uintptr_t address = (uintptr_t)pointer; assert(address >= BASE);
    unsigned number = (address - BASE) / 0x1000;
    assert(number < 4 && devices[number].mapped);
    *off = (address - BASE) % 0x1000; return &wires[number];
}
static uint32_t readl(const volatile void *pointer) {
    unsigned off; wire_t *wire = wire_for_pointer(pointer, &off); reads++;
    if (wire->dead) return UINT32_MAX;
    switch (off) {
    case VMMIO_MAGIC: return wire->mismatch ? 0 : 0x74726976U;
    case VMMIO_VERSION: return 2;
    case VMMIO_DEVICE_ID: return 18;
    case VMMIO_STATUS: return wire->status;
    case VMMIO_DEV_FEATURES: return wire->feature_select ? 1 : 0;
    case VMMIO_QUEUE_NUM_MAX: return 32;
    case VMMIO_QUEUE_READY: return wire->ready;
    case VMMIO_INTR_STATUS: return wire->isr;
    case VMMIO_CONFIG: return wire->selector | ((wire->selector == 1 ? 5U : 0U) << 16);
    case VMMIO_CONFIG + 8: return 0x65646f6dU; /* mode */
    case VMMIO_CONFIG + 12: return 'l';
    default: return 0;
    }
}
static void writel(uint32_t value, volatile void *pointer) {
    unsigned off; wire_t *wire = wire_for_pointer(pointer, &off); writes++;
    if (wire->dead) return;
    switch (off) {
    case VMMIO_STATUS:
        if (!value) {
            assert(!irq_depth); wire->resets++;
            if (wire->hold_reset) return;
            wire->ready = wire->active = wire->isr = 0;
        }
        if (wire->refuse_features && (value & 8)) value &= ~8U;
        if (wire->refuse_driver && (value & 4)) value &= ~4U;
        wire->status = value; break;
    case VMMIO_DEV_FEATURES_SEL: wire->feature_select = value; break;
    case VMMIO_QUEUE_READY: if (!wire->refuse_queue) wire->ready = value; break;
    case VMMIO_QUEUE_DESC_LOW: wire->desc = (wire->desc & ~UINT64_C(0xffffffff)) | value; break;
    case VMMIO_QUEUE_DESC_HIGH: wire->desc = (wire->desc & UINT32_MAX) | ((uint64_t)value << 32); break;
    case VMMIO_QUEUE_DRIVER_LOW: wire->avail = (wire->avail & ~UINT64_C(0xffffffff)) | value; break;
    case VMMIO_QUEUE_DRIVER_HIGH: wire->avail = (wire->avail & UINT32_MAX) | ((uint64_t)value << 32); break;
    case VMMIO_QUEUE_DEVICE_LOW: wire->used = (wire->used & ~UINT64_C(0xffffffff)) | value; break;
    case VMMIO_QUEUE_DEVICE_HIGH: wire->used = (wire->used & UINT32_MAX) | ((uint64_t)value << 32); break;
    case VMMIO_INTR_ACK: wire->isr &= ~value; break;
    case VMMIO_CONFIG: wire->selector = value; break;
    case VMMIO_QUEUE_NOTIFY: {
        assert((wire->status & 15) == 15 && wire->ready);
        assert(wire->bytes == 0xc00 && wire->desc == (uintptr_t)wire->dma);
        vinput_desc_t *desc = (void *)(uintptr_t)wire->desc;
        for (unsigned i = 0; i < 32; i++) {
            assert(desc[i].addr == (uintptr_t)wire->dma + 0x400 + i * 64);
            assert(desc[i].addr + desc[i].len <= (uintptr_t)wire->dma + wire->bytes);
            assert(desc[i].len == 8 && desc[i].flags == 2);
        }
        wire->active = 1; wire->notifications++; break;
    }
    }
}
static resource_t *device_get_resource(device_t *dev, int type, int index) {
    assert(!index); return type == RES_MMIO ? &dev->mmio : &dev->irq;
}
static void *device_map_mmio(device_t *dev, int index) {
    assert(!index && dev->stop && dev->context && dev->managed);
    if (wires[dev->number].fail_map) return NULL;
    dev->mapped = 1; return (void *)(uintptr_t)dev->mmio.start;
}
static int pci_virtio_transport_init(device_t *dev, int type, virtio_transport_t *vt) {
    assert(type == 18 && dev->stop);
    if (wires[dev->number].fail_pci) return -EIO;
    vt->priv = device_map_mmio(dev, 0);
    vt->read32 = vinput_mmio_read32; vt->write32 = vinput_mmio_write32;
    vt->irq = 9; vt->shared_irq = 1; return 0;
}
static int device_add_execution_action(device_t *dev, int (*stop)(void *),
                                        void (*release)(void *), void *context) {
    assert(dev->managed && !dev->mapped && dev->drv_priv == context);
    if (fail_action) return -ENOMEM;
    dev->stop = stop; dev->release = release; dev->context = context; return 0;
}
static int dma_set_mask(device_t *dev, uint64_t mask) {
    assert(mask == UINT64_MAX); return wires[dev->number].fail_gate ? -EOPNOTSUPP : 0;
}
static void *dma_alloc_coherent_aligned(device_t *dev, size_t bytes, size_t alignment, uint64_t *pa) {
    wire_t *wire = &wires[dev->number];
    if (wire->fail_alloc) return NULL;
    assert(!wire->dma && !posix_memalign((void **)&wire->dma, alignment, bytes));
    wire->bytes = bytes; *pa = (uintptr_t)wire->dma; allocated++; return wire->dma;
}
static void dma_free_coherent_aligned(void *dma, size_t bytes, uint64_t pa) {
    wire_t *wire = NULL;
    for (unsigned i = 0; i < 4; i++) if (wires[i].dma == dma) wire = &wires[i];
    assert(wire && !wire->status && !wire->active && bytes == wire->bytes && pa == (uintptr_t)dma);
    for (unsigned i = 0; i < 4; i++) if (handlers[i].context)
        assert(((virtio_input_inst_t *)handlers[i].context)->dma != dma);
    free(dma); wire->dma = NULL; freed++;
}
static void sync_span(const void *pointer, size_t bytes) {
    uintptr_t start = (uintptr_t)pointer;
    for (unsigned i = 0; i < 4; i++) if (wires[i].dma) {
        uintptr_t base = (uintptr_t)wires[i].dma;
        if (start >= base && start - base < wires[i].bytes) {
            assert(bytes <= wires[i].bytes - (start - base)); syncs++; return;
        }
    }
    assert(!"cache handoff outside the allocated DMA span");
}
static void arch_dma_sync_for_device(const void *pointer, size_t bytes) { sync_span(pointer, bytes); }
static void arch_dma_sync_for_cpu(const void *pointer, size_t bytes) { sync_span(pointer, bytes); }
static int request_irq(uint32_t irq, int (*handler)(int, void *), unsigned long flags, void *context) {
    virtio_input_inst_t *inst = context; device_t *dev = inst->owner;
    if (wires[dev->number].fail_irq) return -ENOSPC;
    assert(irq == 9 && flags == IRQF_SHARED && !irq_depth);
    for (unsigned i = 0; i < 4; i++) if (!handlers[i].handler) {
        handlers[i] = (irq_slot_t){handler, context}; return 0;
    }
    return -ENOSPC;
}
static void free_irq(uint32_t irq, void *context) {
    virtio_input_inst_t *inst = context;
    assert(!irq_depth && irq == 9 && !wires[inst->owner->number].status);
    for (unsigned i = 0; i < 4; i++) if (handlers[i].context == context) {
        handlers[i] = (irq_slot_t){0}; irq_freed++; return;
    }
    assert(!"removing the wrong shared IRQ owner");
}
static void reset(void) {
    assert(allocated == freed && !irq_depth);
    for (unsigned i = 0; i < 4; i++) {
        assert(!devices[i].context && !devices[i].drv_priv && !handlers[i].handler);
        assert(!g_input_insts[i].valid);
    }
    memset(devices, 0, sizeof(devices)); memset(wires, 0, sizeof(wires));
    for (unsigned i = 0; i < 4; i++) {
        devices[i].number = i; devices[i].bus = &pci_bus;
        devices[i].mmio = (resource_t){BASE + i * 0x1000, BASE + i * 0x1000 + 0xfff};
        devices[i].irq = (resource_t){9, 9};
    }
    ticks = irq_freed = releases = unmaps = reads = writes = syncs = wakes = 0;
    fail_action = 0;
}
static int unbind(unsigned n) {
    device_t *dev = &devices[n]; int ret;
    if (dev->stop && !dev->stopped) {
        ret = dev->stop(dev->context); if (ret) return ret; dev->stopped = 1;
    }
    ret = vinput_driver.remove(dev); if (ret) return ret;
    if (dev->mapped) { dev->mapped = 0; unmaps++; }
    if (dev->release) { dev->release(dev->context); releases++; }
    dev->context = NULL; dev->stop = NULL; dev->release = NULL; return 0;
}
static void interrupt_line(void) {
    for (unsigned i = 0; i < 4; i++) if (handlers[i].handler)
        assert(handlers[i].handler(9, handlers[i].context) == 1);
}
static void event(unsigned n, uint32_t id, uint32_t length) {
    wire_t *wire = &wires[n];
    vinput_used_t *used = (void *)(uintptr_t)wire->used;
    used->ring[used->idx % 32] = (vinput_used_elem_t){id, length}; used->idx++;
    if (id < 32) {
        struct virtio_input_event *e = (void *)(wire->dma + 0x400 + id * 64);
        *e = (struct virtio_input_event){EV_KEY, 30, n ? 0 : 1};
    }
    wire->isr = 1;
}
static void retained(unsigned n, virtio_input_inst_t *saved) {
    device_t *dev = &devices[n];
    assert(dev->context == saved && dev->drv_priv == saved && saved->valid && dev->mapped);
    assert(!releases && !unmaps && saved->dma == wires[n].dma);
    unsigned before = reads + writes + syncs;
    struct input_event e; struct input_device_info info;
    assert(vinput_class_read(dev, &e, sizeof(e)) == -ESHUTDOWN);
    assert(vinput_class_ioctl(dev, INPUT_IOCTL_GET_INFO, &info) == -ESHUTDOWN);
    assert(!vinput_class_poll(dev, 0) && before == reads + writes + syncs);
}
int main(void) {
    reset(); fail_action = 1;
    assert(vinput_driver.probe(&devices[0]) == -ENOMEM && !devices[0].context && !devices[0].drv_priv);
    assert(!unbind(0));
    reset(); wires[0].fail_pci = 1;
    assert(vinput_driver.probe(&devices[0]) == -EIO); assert(!unbind(0));
    reset(); devices[0].bus = NULL; wires[0].fail_map = 1;
    assert(vinput_driver.probe(&devices[0]) == -ENODEV); assert(!unbind(0));
    reset(); wires[0].mismatch = 1;
    assert(vinput_driver.probe(&devices[0]) == -ENODEV); assert(!writes && !unbind(0) && !writes);
    reset(); wires[0].status = 1; wires[0].hold_reset = 1;
    assert(vinput_driver.probe(&devices[0]) == -ETIMEDOUT);
    assert(unbind(0) == -ETIMEDOUT && devices[0].context); wires[0].hold_reset = 0; assert(!unbind(0));
    reset(); wires[0].fail_gate = 1;
    assert(vinput_driver.probe(&devices[0]) == -EOPNOTSUPP); assert(!unbind(0));
    reset(); wires[0].fail_alloc = 1;
    assert(vinput_driver.probe(&devices[0]) == -ENOMEM); assert(!unbind(0));
    reset(); wires[0].refuse_queue = 1;
    assert(vinput_driver.probe(&devices[0]) == -EIO);
    wires[0].hold_reset = 1; assert(unbind(0) == -ETIMEDOUT && wires[0].dma);
    wires[0].hold_reset = 0; assert(!unbind(0));
    reset(); wires[0].fail_irq = 1;
    assert(vinput_driver.probe(&devices[0]) == -ENOSPC); assert(!unbind(0));
    reset(); wires[0].refuse_driver = 1;
    assert(vinput_driver.probe(&devices[0]) == -EIO); assert(!unbind(0) && irq_freed == 1);
    reset(); assert(!vinput_driver.probe(&devices[0]) && !vinput_driver.probe(&devices[1]));
    assert(handlers[0].context != handlers[1].context && wires[0].bytes == 0xc00);
    event(0, 31, 8); event(1, 0, 8); interrupt_line();
    uint8_t output[sizeof(struct input_event) + 1]; struct input_event e;
    assert(vinput_class_read(&devices[0], output+1, sizeof(e)) == sizeof(e));
    memcpy(&e, output+1, sizeof(e)); assert(e.type == EV_KEY && e.code == 30 && e.value == 1);
    assert(vinput_class_read(&devices[1], &e, sizeof(e)) == sizeof(e) && e.value == 0);
    struct input_device_info info;
    assert(!vinput_class_ioctl(&devices[1], INPUT_IOCTL_GET_INFO, &info) && !strcmp(info.name, "model"));
    assert(!unbind(0)); event(1, 31, 8); interrupt_line();
    assert(vinput_class_read(&devices[1], &e, sizeof(e)) == sizeof(e));
    assert(!unbind(1) && irq_freed == 2);
    /* Stop during an active data path returns busy and closes new access. */
    reset(); assert(!vinput_driver.probe(&devices[0]));
    virtio_input_inst_t *saved = devices[0].context; uint64_t flags = spin_lock_irqsave(&saved->lock);
    assert(unbind(0) == -EBUSY); spin_unlock_irqrestore(&saved->lock, flags); retained(0, saved);
    wires[0].hold_reset = 1; assert(unbind(0) == -ETIMEDOUT); retained(0, saved);
    unsigned before = syncs, notifications = wires[0].notifications;
    event(0, 31, 8); interrupt_line();
    assert(syncs == before && wires[0].notifications == notifications && !wires[0].isr);
    assert(!vinput_driver.probe(&devices[1]) && devices[1].context != saved);
    wires[0].hold_reset = 0; assert(!unbind(0) && !unbind(1));
    /* Invalid used-ring metadata closes admission without returning buffers. */
    const uint32_t bad[][2] = {{0x10000,8},{32,8},{0,7}};
    for (unsigned i = 0; i < ARRAY_SIZE(bad); i++) {
        reset(); assert(!vinput_driver.probe(&devices[0]));
        event(0, bad[i][0], bad[i][1]); interrupt_line();
        assert(((virtio_input_inst_t *)devices[0].context)->closing && wires[0].notifications == 1);
        assert(!unbind(0));
    }
    reset(); assert(!vinput_driver.probe(&devices[0]));
    event(0, 0, 8); event(0, 0, 8); interrupt_line();
    assert(((virtio_input_inst_t *)devices[0].context)->closing && wires[0].notifications == 1); assert(!unbind(0));
    reset(); assert(!vinput_driver.probe(&devices[0]));
    ((vinput_used_t *)(uintptr_t)wires[0].used)->idx = 33; interrupt_line();
    assert(((virtio_input_inst_t *)devices[0].context)->closing); assert(!unbind(0));
    reset(); assert(!vinput_driver.probe(&devices[0]));
    saved = devices[0].context; saved->last_used = 0xffff;
    ((vinput_used_t *)(uintptr_t)wires[0].used)->idx = 0xffff;
    event(0, 31, 8); interrupt_line(); assert(!saved->closing && saved->last_used == 0);
    wires[0].dead = 1; assert(unbind(0) == -ETIMEDOUT); retained(0, saved);
    wires[0].dead = 0; assert(!unbind(0));
    assert(allocated == freed); puts("VINPUT_LIFETIME_OK: full DMA span, shared IRQ owners, bounded used ring, retained reset and partial probe");
}
