#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void host_log(const char *format, ...) { (void)format; }
#define kinfo host_log
#define a20_perf_count(...) ((void)0)
#define a20_perf_add(...) ((void)0)
#define DMA_MASK_64BIT UINT64_MAX
#define va_to_pa(p) ((uintptr_t)(p))
#define wmb() ((void)0)
typedef struct { int locked; } spinlock_t;
static void spin_init(spinlock_t *s) { s->locked = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *s) { assert(!s->locked); s->locked = 1; return 0; }
static int spin_trylock_irqsave(spinlock_t *s, uint64_t *f) { if (s->locked) return 0; *f = spin_lock_irqsave(s); return 1; }
static void spin_unlock_irqrestore(spinlock_t *s, uint64_t f) { (void)f; assert(s->locked); s->locked = 0; }
struct hardware {
    uint32_t regs[0x6000 / 4], id;
    int dead, stuck_reset, stuck_master, stuck_engine, confirmed, drained, settled;
    int fail_action, fail_map, fail_mask, fail_range, fail_alloc, alloc_calls;
    int reads, writes, resets, master_disables, frees, irq_frees, stack_calls;
    unsigned irq_mask;
    int force_msi, stop_on_tx, busy_stop;
};
static struct hardware hardware[2], *current_hw;
typedef struct { uint64_t start, end; } resource_t;
typedef struct device {
    void *drv_priv;
    struct hardware *hw;
    resource_t bar;
    int managed;
    int (*stop)(void *);
    void *context;
} device_t;
/* PRODUCTION_DEFINITIONS */
struct allocation { void *p; struct hardware *h; int context; } allocations[8];
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + sizeof(hardware[i].regs))
            return current_hw = &hardware[i];
    abort();
}
static void outside_ring_lock(void) {
    for (unsigned i = 0; i < 8; i++) if (allocations[i].p && allocations[i].context)
        assert(!((e1000_device_t *)allocations[i].p)->lock.locked);
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    if (h->dead) return UINT32_MAX;
    uint32_t value = *(const volatile uint32_t *)p;
    if ((uintptr_t)p - (uintptr_t)h->regs == E1000_ICR) *(volatile uint32_t *)p = 0;
    return value;
}
static void writel(uint32_t v, volatile void *p);
static void mdelay(unsigned ms) {
    outside_ring_lock(); assert(ms == 10 || ms == 5);
    if (ms == 10 && !(current_hw->regs[E1000_RCTL / 4] & E1000_RCTL_EN) &&
        !(current_hw->regs[E1000_TCTL / 4] & E1000_TCTL_EN)) current_hw->drained = 1;
    if (ms == 5) current_hw->settled = 1;
}
static void udelay(unsigned us) { assert(us == 100); outside_ring_lock(); }
static void arch_dma_sync_for_device(void *p, size_t n) { assert(p && n); }
static void arch_dma_sync_for_cpu(void *p, size_t n) { assert(p && n); }
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *), void *ctx) {
    assert(d->managed && d->drv_priv == ctx && !release);
    if (d->hw->fail_action) return -ENOMEM;
    d->stop = stop; d->context = ctx; return 0;
}
static int pci_enable_and_assign_bars(device_t *d) { assert(d->managed); current_hw = d->hw; return 0; }
static resource_t *pci_get_bar_resource(device_t *d, unsigned bar) { assert(!bar); return &d->bar; }
static void *pci_map_bar(device_t *d, unsigned bar) { assert(!bar && d->stop); return d->hw->fail_map ? NULL : d->hw->regs; }
static uint32_t pci_device_id(device_t *d) { return d->hw->id; }
static int dma_set_mask(device_t *d, uint64_t mask) { assert(mask == DMA_MASK_64BIT); return d->hw->fail_mask ? -EOPNOTSUPP : 0; }
static int dma_range_ok(device_t *d, uint64_t addr, size_t size) { assert(addr && size); return !d->hw->fail_range; }
static void *kmalloc(size_t n) {
    struct hardware *h = current_hw;
    if (++h->alloc_calls == h->fail_alloc) return NULL;
    void *p; assert(!posix_memalign(&p, 4096, n));
    for (unsigned i = 0; i < 8; i++) if (!allocations[i].p) {
        allocations[i] = (struct allocation){p, h, n == sizeof(e1000_device_t)}; return p;
    }
    abort();
}
static void kfree(void *p) {
    for (unsigned i = 0; i < 8; i++) if (allocations[i].p == p) {
        struct hardware *h = allocations[i].h;
        assert(!h->irq_mask);
        if (h->regs[E1000_RDLEN / 4] || h->regs[E1000_TDLEN / 4]) assert(h->confirmed);
        allocations[i].p = NULL; h->frees++; free(p); return;
    }
    abort();
}
static void e1000_poll_mode_init(void) {}
static void e1000_itr_init(device_t *d, e1000_device_t *n) { (void)d; (void)n; }
static void e1000_setup_interrupts(device_t *d, e1000_device_t *n) {
    assert(d->stop && n->ready && n->rx && n->tx);
    if (d->hw->force_msi) { n->irq = 80; n->msi = 1; d->hw->irq_mask = 1; }
    else if (n->pcie) { n->irq = n->msix_base = 64; n->msix_vectors = 2; d->hw->irq_mask = 3; }
    else { n->irq = 11; d->hw->irq_mask = 1; }
    n->irq_registered = 1;
}
static void free_irq(uint32_t irq, void *ctx) {
    device_t *d = ctx; struct hardware *h = d->hw;
    outside_ring_lock(); assert(h->confirmed && (h->id == 0x808610d3U || h->settled));
    unsigned bit = irq == 65 ? 2 : 1;
    assert(h->irq_mask & bit); h->irq_mask &= ~bit; h->irq_frees++;
}
static void e1000_msix_teardown(device_t *d, e1000_device_t *n) {
    assert(!d->hw->irq_mask && n->msix_vectors == 2); n->msix_vectors = 0; n->msix_base = -1;
}
static void pci_msi_disable(device_t *d) { assert(!d->hw->irq_mask); }
static void irq_free_vectors(uint32_t irq, unsigned count) { assert(irq == 80 && count == 1); }
static void net_stack_device_interrupt(device_t *d) { d->hw->stack_calls++; }
/* PRODUCTION_FUNCTIONS */
static void writel(uint32_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if ((off == E1000_RCTL || off == E1000_TCTL) && h->stuck_engine && !(v & 2U)) v |= 2U;
    if (off == E1000_RDLEN || off == E1000_TDLEN) h->confirmed = 0;
    if (off == E1000_CTRL) {
        if (h->id == 0x808610d3U) {
            if (v & E1000_CTRL_GIO_MASTER_DISABLE) {
                h->master_disables++;
                if (!h->stuck_master) { h->regs[E1000_STATUS / 4] &= ~E1000_STATUS_GIO_MASTER_ENABLE; h->confirmed = 1; }
            } else { h->regs[E1000_STATUS / 4] |= E1000_STATUS_GIO_MASTER_ENABLE; h->confirmed = 0; }
        } else if (v & E1000_CTRL_RST) {
            h->resets++;
            if (!h->stuck_reset) v &= ~E1000_CTRL_RST;
            h->confirmed = !h->stuck_reset && !h->stuck_engine && h->drained;
            /* Model QEMU: reset bit self-clears but queue lengths remain. */
        }
    }
    *(volatile uint32_t *)p = v;
    if (off == E1000_TDT && v && h->stop_on_tx) {
        h->stop_on_tx = 0;
        device_t *d = NULL;
        for (unsigned i = 0; i < 8; i++) if (allocations[i].p && allocations[i].context && allocations[i].h == h)
            d = ((e1000_device_t *)allocations[i].p)->device;
        assert(d && e1000_stop(d->drv_priv) == -EBUSY && h->irq_mask && !h->frees); h->busy_stop++;
    }
}
static device_t fresh(unsigned index, int pcie) {
    struct hardware *h = &hardware[index];
    for (unsigned i = 0; i < 8; i++) assert(!allocations[i].p || allocations[i].h != h);
    memset(h, 0, sizeof(*h)); h->id = pcie ? 0x808610d3U : 0x8086100eU;
    h->regs[E1000_STATUS / 4] = 2U | (pcie ? E1000_STATUS_GIO_MASTER_ENABLE : 0);
    h->regs[E1000_RAL0 / 4] = 0x12005452; h->regs[E1000_RAH0 / 4] = 0x80005634;
    return (device_t){.hw=h,.bar={0,0x5fff}};
}
static int cleanup(device_t *d) {
    int r = d->stop ? d->stop(d->context) : 0;
    if (!r) r = e1000_remove(d);
    if (!r) { d->stop = NULL; d->context = NULL; }
    return r;
}
int main(void) {
    char frame[64], received[128]; memset(frame, 0x5a, sizeof(frame));
    for (int pcie = 0; pcie <= 1; pcie++) {
        device_t d = fresh(0, pcie); assert(!e1000_probe(&d)); e1000_device_t *n = d.drv_priv;
        assert(n->ready && e1000_class_rx_irq_driven(&d) && e1000_link_up(&d));
        assert(e1000_send(&d, frame, sizeof(frame)) == sizeof(frame));
        assert(!memcmp(n->tx_buf[0], frame, sizeof(frame)) && (n->tx[0].command & E1000_TXD_CMD_RS));
        /* A prefetched TDH is not a completed DMA writeback. */
        d.hw->regs[E1000_TDH / 4] = 1;
        uint64_t flags = spin_lock_irqsave(&n->lock);
        assert(!e1000_reclaim_tx_locked(n) && !n->tx_done);
        n->tx[0].status = E1000_TXD_STAT_DD;
        assert(e1000_reclaim_tx_locked(n) == 1 && n->tx_done == 1);
        spin_unlock_irqrestore(&n->lock, flags);
        n->rx[0].status = E1000_RXD_STAT_DD | E1000_RXD_STAT_EOP;
        n->rx[0].length = sizeof(frame); memcpy(n->rx_buf[0], frame, sizeof(frame));
        assert(e1000_recv(&d, received, sizeof(received)) == sizeof(frame) && !memcmp(received, frame, sizeof(frame)));
        void *ring = n->ring_mem; d.hw->stuck_reset = !pcie; d.hw->stuck_master = pcie;
        assert(cleanup(&d) == -ETIMEDOUT && n->ring_mem == ring && d.drv_priv == n && d.hw->irq_mask && !d.hw->frees);
        int reads = d.hw->reads, writes = d.hw->writes;
        assert(e1000_send(&d, frame, sizeof(frame)) == -ENODEV && e1000_recv(&d, received, sizeof(received)) == -ENODEV);
        e1000_poll(&d); assert(!e1000_link_up(&d) && !e1000_class_rx_irq_driven(&d));
        assert(reads == d.hw->reads && writes == d.hw->writes);
        d.hw->regs[E1000_ICR / 4] = E1000_IMS_TXDW;
        assert(!e1000_irq_handler(n->irq, &d) && !d.hw->regs[E1000_ICR / 4] && !d.hw->stack_calls);
        writes = d.hw->writes; d.hw->dead = 1;
        assert(cleanup(&d) == -ENODEV && d.hw->writes == writes && d.hw->irq_mask && !d.hw->frees);
        d.hw->dead = d.hw->stuck_reset = d.hw->stuck_master = 0;
        assert(!cleanup(&d) && !d.drv_priv && d.hw->frees == 2 && d.hw->irq_frees == (pcie ? 2 : 1));
        d = fresh(0, pcie); assert(!e1000_probe(&d)); d.hw->stop_on_tx = 1;
        assert(e1000_send(&d, frame, sizeof(frame)) == sizeof(frame) && d.hw->busy_stop == 1);
        assert(!cleanup(&d) && d.hw->frees == 2);
    }
    device_t a = fresh(0, 1); a.hw->stuck_master = 1;
    assert(e1000_probe(&a) == -ETIMEDOUT && a.drv_priv && cleanup(&a) == -ETIMEDOUT);
    void *held = a.drv_priv;
    device_t b = fresh(1, 0); assert(!e1000_probe(&b) && b.drv_priv != held);
    assert(!cleanup(&b) && a.drv_priv == held && !a.hw->frees);
    a.hw->stuck_master = 0; assert(!cleanup(&a));
    for (unsigned fail = 0; fail < 7; fail++) {
        a = fresh(0, 0); a.hw->fail_alloc = fail == 0 ? 1 : (fail == 4 ? 2 : 0);
        a.hw->fail_action = fail == 1; a.hw->fail_map = fail == 2; a.hw->fail_mask = fail == 3;
        a.hw->fail_range = fail == 5; if (fail == 6) a.hw->regs[E1000_RAH0 / 4] = 0;
        assert(e1000_probe(&a) < 0); assert(!cleanup(&a) && !a.drv_priv);
    }
    for (int pcie = 0; pcie <= 1; pcie++) {
        a = fresh(0, pcie); assert(!e1000_probe(&a)); a.hw->stuck_engine = 1;
        assert(cleanup(&a) == -ETIMEDOUT && a.hw->irq_mask && !a.hw->frees);
        a.hw->stuck_engine = 0; assert(!cleanup(&a));
    }
    a = fresh(0, 0); a.hw->force_msi = 1;
    assert(!e1000_probe(&a) && !cleanup(&a) && a.hw->irq_frees == 1);
    for (unsigned i = 0; i < 8; i++) assert(!allocations[i].p);
    puts("PASS: E1000 PCI/PCIe stop ACK, DMA/IRQ retention, failed probe, active I/O and DD ownership");
    return 0;
}
