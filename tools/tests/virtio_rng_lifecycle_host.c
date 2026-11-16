#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/block/virtio_blk.h"
#include "drivers/bus/virtio_transport.h"
#define ALIGNED(n) __attribute__((aligned(n)))
#define MS_TO_TICKS(n) (n)
#define IRQF_SHARED 1U
#define RES_MMIO 1
#define RES_IRQ 2
#define mb() ((void)0)
#define wmb() ((void)0)
static void host_log(const char *fmt, ...) { (void)fmt; }
#define kinfo host_log
typedef struct { int held; } spinlock_t;
static void spin_init(spinlock_t *l) { l->held = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *l) { assert(!l->held); l->held = 1; return 0; }
static void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { assert(!f && l->held); l->held = 0; }
static int spin_trylock_irqsave(spinlock_t *l, uint64_t *f) {
    if (l->held) return 0;
    l->held = 1; *f = 0; return 1;
}
static void *proc_current(void) { return (void *)1; }
static void arch_cpu_relax(void) {}
static uint64_t ticks;
static uint64_t clock_get_ticks(void) { return ++ticks; }
static uint64_t clock_ticks_per_sec(void) { return 1000; }
static void proc_yield(void);
typedef struct { int unused; } bus_type_t;
static bus_type_t pci_bus;
typedef struct { uint64_t start, end; } resource_t;
struct hardware;
typedef struct device {
    bus_type_t *bus;
    void *drv_priv;
    struct hardware *hw;
    int managed;
    int (*stop)(void *);
    void *context;
    resource_t bar, irq;
} device_t;
struct hardware {
    uint32_t regs[0x110 / 4];
    uint16_t consumed;
    int pci, dead, stuck_reset, confirmed, fail_action, fail_map, fail_transport;
    int fail_alloc, fail_irq, no_version, reject_features, bad_queue, fail_name;
    int stall, stop_notify, stop_wait, stop_reseed, busy_stops, legacy;
    int irq_owned, irq_frees, allocs, frees, resets, notifies, seeds, in_irq;
    void *dma;
};
static struct hardware hardware[2];
static uint32_t readl(const volatile void *);
static void writel(uint32_t, volatile void *);
static void *dma_alloc_coherent_aligned(device_t *, size_t, size_t, uint64_t *);
static void dma_free_coherent_aligned(void *, size_t, uint64_t);
static void dma_sync_for_device(void *p, size_t n) { assert(p && n); }
static void dma_sync_for_cpu(void *p, size_t n) { assert(p && n); }
static void random_reseed(uint64_t);
static int request_irq(uint32_t, int (*)(int, void *), unsigned long, void *);
static void free_irq(uint32_t, void *);
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int device_add_execution_action(device_t *d, int (*s)(void *), void (*r)(void *), void *p) {
    assert(d->managed && d->drv_priv == p && !r);
    if (d->hw->fail_action) return -ENOMEM;
    d->stop = s; d->context = p; return 0;
}
static resource_t *drv_device_get_resource(device_t *d, int type, int index) {
    assert(!index); return type == RES_MMIO ? &d->bar : type == RES_IRQ ? &d->irq : NULL;
}
static void *device_map_mmio(device_t *d, int index) { assert(d->managed && !index); return d->hw->fail_map ? NULL : d->hw->regs; }
static int drv_device_set_devfs_name(device_t *d, const char *n) { assert(!strcmp(n, "hwrng")); return d->hw->fail_name ? -ENOMEM : 0; }
static int pci_virtio_transport_init(device_t *, uint32_t, virtio_transport_t *);
/* PRODUCTION_RESET */
/* PRODUCTION_RNG */
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + sizeof(hardware[i].regs)) return &hardware[i];
    abort();
}
static struct hardware *current(void) { assert(g_vrng.owner); return g_vrng.owner->hw; }
static void *dma_alloc_coherent_aligned(device_t *d, size_t bytes, size_t align, uint64_t *dma) {
    assert(d->managed && d->stop && bytes == sizeof(vrng_mem_t) && align == PAGE_SIZE);
    if (d->hw->fail_alloc) return NULL;
    void *p; assert(!posix_memalign(&p, align, bytes));
    assert(!d->hw->dma); d->hw->dma = p; d->hw->allocs++; *dma = (uintptr_t)p; return p;
}
static void dma_free_coherent_aligned(void *p, size_t bytes, uint64_t dma) {
    struct hardware *h = current();
    assert(p == h->dma && (uintptr_t)p == dma && bytes == sizeof(vrng_mem_t));
    assert(g_vrng.closing && g_vrng.hardware_stopped && !g_vrng.active_calls);
    assert(!h->irq_owned && h->confirmed && !h->regs[VRNG_MMIO_STATUS / 4]);
    h->dma = NULL; h->frees++; free(p);
}
static void active_stop(int *hook) {
    struct hardware *h = current(); *hook = 0;
    assert(g_vrng.active_calls && !g_vrng.lock.held);
    assert(vrng_stop(&g_vrng) == -EBUSY && g_vrng.closing);
    assert(h->dma && !h->frees && !g_vrng.hardware_stopped);
    h->busy_stops++;
}
static void proc_yield(void) {
    if (g_vrng.owner && current()->stop_wait) active_stop(&current()->stop_wait);
}
static void random_reseed(uint64_t word) {
    (void)word; struct hardware *h = current();
    assert(!g_vrng.lock.held && !h->in_irq && g_vrng.active_calls);
    h->seeds++;
    if (h->stop_reseed) active_stop(&h->stop_reseed);
}
static int request_irq(uint32_t irq, int (*handler)(int, void *), unsigned long flags, void *p) {
    struct hardware *h = current();
    assert(p == &g_vrng && irq == 12 && flags == (h->pci ? IRQF_SHARED : 0));
    assert(g_vrng.mem && !g_vrng.valid && !g_vrng.mem->avail.idx);
    if (h->fail_irq) return -EBUSY;
    h->irq_owned = 1; h->regs[VRNG_MMIO_INTR_STATUS / 4] = 1; h->in_irq = 1;
    assert(!handler((int)irq, p) && !h->regs[VRNG_MMIO_INTR_STATUS / 4]);
    h->in_irq = 0; assert(!h->seeds); return 0;
}
static void free_irq(uint32_t irq, void *p) {
    struct hardware *h = current();
    assert(p == &g_vrng && irq == 12 && h->irq_owned && h->confirmed);
    assert(!g_vrng.lock.held && g_vrng.hardware_stopped && !g_vrng.active_calls);
    h->irq_owned = 0; h->irq_frees++;
}
static uint64_t address(struct hardware *h, unsigned off) {
    return h->regs[off / 4] | ((uint64_t)h->regs[(off + 4) / 4] << 32);
}
static uint8_t pattern(unsigned slot, unsigned byte) { return (uint8_t)(slot * 19 + byte + 1); }
static void notify(struct hardware *h) {
    assert(g_vrng.active_calls && !g_vrng.lock.held && h->dma);
    h->notifies++;
    if (h->stop_notify) active_stop(&h->stop_notify);
    if (h->stall) return;
    vrng_desc_t *desc = (void *)(uintptr_t)address(h, VRNG_MMIO_QUEUE_DESC_LOW);
    vrng_avail_t *avail = (void *)(uintptr_t)address(h, VRNG_MMIO_QUEUE_DRIVER_LOW);
    vrng_used_t *used = (void *)(uintptr_t)address(h, VRNG_MMIO_QUEUE_DEVICE_LOW);
    while (h->consumed != avail->idx) {
        uint16_t id = avail->ring[h->consumed++ % VRNG_Q_SLOTS];
        assert(id < VRNG_Q_SLOTS && desc[id].flags == VRNG_QD_F_WRITE && desc[id].len == VRNG_BUF_SIZE);
        uint8_t *bytes = (void *)(uintptr_t)desc[id].addr;
        assert(bytes == ((vrng_mem_t *)h->dma)->buf[id]);
        for (unsigned i = 0; i < desc[id].len; i++) bytes[i] = pattern(id, i);
        used->ring[used->idx++ % VRNG_Q_SLOTS] = (vrng_used_elem_t){id, desc[id].len};
    }
    h->regs[VRNG_MMIO_INTR_STATUS / 4] = 1;
    if (h->irq_owned) {
        h->in_irq = 1; vrng_irq_handler(12, &g_vrng); h->in_irq = 0;
        assert(!h->regs[VRNG_MMIO_INTR_STATUS / 4]);
    }
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h = owner(p); unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if (h->dead) return UINT32_MAX;
    if (off == VRNG_MMIO_DEV_FEATURES) return h->no_version ? 0 : VRNG_F_VERSION_1;
    if (off == VRNG_MMIO_QUEUE_NUM_MAX) return h->bad_queue ? 1 : VRNG_Q_SLOTS;
    uint32_t val = h->regs[off / 4];
    if (off == VRNG_MMIO_INTR_STATUS && h->pci) h->regs[off / 4] = 0;
    return val;
}
static void writel(uint32_t val, volatile void *p) {
    struct hardware *h = owner(p); unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if (off == VRNG_MMIO_QUEUE_NOTIFY) { assert(!val); notify(h); return; }
    if (off == VRNG_MMIO_INTR_ACK) { h->regs[VRNG_MMIO_INTR_STATUS / 4] &= ~val; return; }
    if (off == VRNG_MMIO_STATUS) {
        assert(!g_vrng.lock.held);
        if (!val) {
            h->resets++;
            if (h->stuck_reset) { h->regs[off / 4] |= VRNG_STATUS_ACK; return; }
            h->confirmed = 1; h->consumed = 0;
            h->regs[VRNG_MMIO_QUEUE_READY / 4] = 0;
        } else h->confirmed = 0;
        if (h->reject_features) val &= ~VRNG_STATUS_FEATURES_OK;
    }
    h->regs[off / 4] = val;
}
static uint32_t pci_read(virtio_transport_t *t, uint32_t off) { return readl((uint8_t *)t->priv + off); }
static void pci_write(virtio_transport_t *t, uint32_t off, uint32_t val) { writel(val, (uint8_t *)t->priv + off); }
static int pci_virtio_transport_init(device_t *d, uint32_t id, virtio_transport_t *t) {
    assert(d->managed && d->stop && id == VRNG_DEVICE_ID);
    if (d->hw->fail_transport) return -ENODEV;
    *t = (virtio_transport_t){.read32=pci_read, .write32=pci_write, .priv=d->hw->regs,
        .irq=12, .shared_irq=1, .legacy=d->hw->legacy}; return 0;
}
static device_t fresh(unsigned i, int pci) {
    assert(!g_vrng.owner); struct hardware *h = &hardware[i];
    assert(!h->dma && !h->irq_owned); memset(h, 0, sizeof(*h)); h->pci = pci;
    h->regs[VRNG_MMIO_MAGIC / 4] = VRNG_MMIO_MAGIC_VALUE;
    h->regs[VRNG_MMIO_VERSION / 4] = 2;
    h->regs[VRNG_MMIO_DEVICE_ID / 4] = VRNG_DEVICE_ID;
    return (device_t){.bus=pci ? &pci_bus : NULL, .hw=h,
        .bar={.start=(uintptr_t)h->regs}, .irq={.start=12}};
}
/* Driver-core ordering adapter: execution stops, class calls drain, then remove.
 * The real core is not linked here; the assertions test the provider contract. */
static int teardown(device_t *d) {
    if (d->stop) { int ret = d->stop(d->context); if (ret) return ret; }
    return vrng_remove(d);
}
static void normal_read(int pci, int no_irq) {
    device_t d = fresh(0, pci); struct hardware *h = d.hw; h->fail_irq = no_irq;
    assert(!vrng_probe(&d) && !h->notifies && !h->seeds && !g_vrng.mem->avail.idx);
    assert(!vrng_class_poll(&d, 1) && !h->notifies);
    uint8_t bytes[256]; assert(vrng_class_read(&d, bytes, sizeof(bytes)) == 256);
    for (unsigned i = 0; i < sizeof(bytes); i++) assert(bytes[i] == pattern(0, i));
    assert(h->notifies == 1 && h->seeds == 32 && !g_vrng.active_calls);
    assert(vrng_class_poll(&d, 1) && h->seeds == 32);
    assert(vrng_class_read(&d, bytes, 37) == 37);
    for (unsigned i = 0; i < 37; i++) assert(bytes[i] == pattern(1, i));
    assert(h->seeds == 36 && h->notifies == 1);
    h->stuck_reset = 1; void *dma = h->dma;
    assert(teardown(&d) == -ETIMEDOUT && d.drv_priv == &g_vrng && h->dma == dma && !h->frees);
    assert(h->irq_owned == !no_irq && vrng_class_read(&d, bytes, 1) == -ENODEV);
    assert(!vrng_class_poll(&d, 1));
    device_t peer = {.bus=&pci_bus, .hw=&hardware[1]};
    assert(vrng_probe(&peer) == -EBUSY && !peer.drv_priv && g_vrng.mem == dma);
    h->dead = 1; assert(teardown(&d) == -ENODEV && h->dma == dma); h->dead = 0;
    h->stuck_reset = 0; assert(!teardown(&d) && !d.drv_priv && h->frees == 1);
    assert(!g_vrng.owner && h->irq_frees == !no_irq && !teardown(&d));
    peer = fresh(1, pci); assert(!vrng_probe(&peer) && !teardown(&peer));
}
static void read_race(unsigned hook) {
    device_t d = fresh(0, 1); struct hardware *h = d.hw;
    assert(!vrng_probe(&d)); uint8_t bytes[256];
    if (hook == 0) h->stop_notify = 1;
    if (hook == 1) { h->stop_wait = 1; h->stall = 1; }
    if (hook == 2) h->stop_reseed = 1;
    int ret = vrng_class_read(&d, bytes, sizeof(bytes));
    assert(ret == (hook == 2 ? 256 : -ENODEV));
    assert(h->busy_stops == 1 && !g_vrng.active_calls && h->dma && h->irq_owned);
    assert(!teardown(&d) && h->frees == 1 && h->irq_frees == 1);
}
static void partial_probe(unsigned failure) {
    device_t d = fresh(0, failure != 2); struct hardware *h = d.hw;
    switch (failure) {
    case 0: h->fail_action = 1; break;
    case 1: h->fail_transport = 1; break;
    case 2: h->fail_map = 1; break;
    case 3: h->regs[VRNG_MMIO_MAGIC / 4] = 0; break;
    case 4: h->legacy = 1; break;
    case 5: h->stuck_reset = 1; break;
    case 6: h->no_version = 1; break;
    case 7: h->reject_features = 1; break;
    case 8: h->fail_alloc = 1; break;
    case 9: h->bad_queue = 1; break;
    case 10: h->fail_name = 1; break;
    default: abort();
    }
    assert(vrng_probe(&d) < 0 && d.drv_priv == &g_vrng && g_vrng.owner == &d);
    assert(!g_vrng.valid && !h->seeds && !h->notifies);
    if (g_vrng.reset_required) {
        h->stuck_reset = 1; void *dma = h->dma; int irq = h->irq_owned;
        assert(teardown(&d) == -ETIMEDOUT && g_vrng.owner == &d && h->dma == dma && h->irq_owned == irq);
        device_t peer = {.hw=&hardware[1]}; assert(vrng_probe(&peer) == -EBUSY);
        assert(!h->frees && !h->irq_frees); h->stuck_reset = 0;
    }
    assert(!teardown(&d) && !d.drv_priv && !g_vrng.owner);
    assert(h->allocs == h->frees && !h->dma && !h->irq_owned);
}
int main(void) {
    normal_read(0, 0); normal_read(1, 0); normal_read(1, 1);
    for (unsigned i = 0; i < 3; i++) read_race(i);
    for (unsigned i = 0; i < 11; i++) partial_probe(i);
    puts("virtio-rng lifecycle: reads, lock-free notify/wait/reseed races, reset retention and 11 partial probes passed");
    return 0;
}
