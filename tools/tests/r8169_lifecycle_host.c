#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define IRQF_SHARED 1
#define DMA_MASK_64BIT UINT64_MAX
#define PFN_NONE UINTPTR_MAX
typedef uintptr_t pfn_t;
static pfn_t phys_to_pfn(uint64_t p) { return p >> 12; }
static void *pfn_to_virt(pfn_t p) { return (void *)(p << 12); }
static void host_log(const char *format, ...) { (void)format; }
#define kinfo host_log
#define kerr host_log
typedef struct { int held; } spinlock_t;
static void spin_init(spinlock_t *s) { s->held = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *s) { assert(!s->held); s->held = 1; return 0; }
static int spin_trylock_irqsave(spinlock_t *s, uint64_t *f) { if (s->held) return 0; *f = spin_lock_irqsave(s); return 1; }
static void spin_unlock_irqrestore(spinlock_t *s, uint64_t f) { (void)f; assert(s->held); s->held = 0; }
struct hardware;
typedef struct { uint64_t start, end; } resource_t;
typedef struct device {
    const char *name;
    void *drv_priv;
    struct hardware *hw;
    resource_t bar;
    int managed;
    int (*stop)(void *);
    void *context;
} device_t;
/* PRODUCTION_DEFINITIONS */
static uint8_t r8169_sim_read8(r8169_device_t *, uint32_t);
static void r8169_sim_write8(r8169_device_t *, uint32_t, uint8_t);
static int r8169_stop(void *);
static int g_r8169_poll_mode;
struct hardware {
    uint8_t regs[256];
    int dead, stuck_reset, stuck_engine, stuck_mask, confirmed, irq;
    int reads, writes, resets, irq_frees, frees, stack_calls, early_irq;
    int fail_alloc, alloc_calls, fail_action, fail_map, fail_mask, fail_irq;
    int stop_on_tx, busy_stop;
    device_t *device;
};
static struct hardware hardware[2], *current_hw;
static const char *cmdline = "";
struct allocation { void *p; size_t bytes; struct hardware *hw; int context, synced; };
static struct allocation allocations[16];
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + sizeof(hardware[i].regs)) return &hardware[i];
    abort();
}
static uint8_t readb(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    return h->dead ? UINT8_MAX : *(const volatile uint8_t *)p;
}
static uint16_t readw(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    uint16_t v; memcpy(&v, (const void *)p, sizeof(v)); return h->dead ? UINT16_MAX : v;
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    uint32_t v; memcpy(&v, (const void *)p, sizeof(v)); return h->dead ? UINT32_MAX : v;
}
static void writeb(uint8_t v, volatile void *p);
static void writew(uint16_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if (off == R8169_INTRSTAT) {
        uint16_t old; memcpy(&old, (const void *)p, sizeof(old)); v = old & ~v;
    }
    if (off == R8169_INTRMASK && !v && h->stuck_mask) return;
    memcpy((void *)p, &v, sizeof(v));
}
static void writel(uint32_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if ((off == R8169_TNPDS_LO || off == R8169_RDSAR_LO) && v) h->confirmed = 0;
    memcpy((void *)p, &v, sizeof(v));
}
static void mdelay(unsigned ms) {
    assert(ms == 1);
    for (unsigned i = 0; i < 16; i++)
        if (allocations[i].p && allocations[i].context)
            assert(!((r8169_device_t *)allocations[i].p)->lock.held);
}
static void *allocate(struct hardware *h, size_t bytes, int context) {
    if (h && ++h->alloc_calls == h->fail_alloc) return NULL;
    void *p; assert(!posix_memalign(&p, PAGE_SIZE, bytes)); memset(p, 0, bytes);
    for (unsigned i = 0; i < 16; i++) if (!allocations[i].p) {
        allocations[i] = (struct allocation){p, bytes, h, context, 0}; return p;
    }
    abort();
}
static void *kmalloc(size_t bytes) { assert(bytes == sizeof(r8169_device_t)); return allocate(current_hw, bytes, 1); }
static void retire(void *p) {
    for (unsigned i = 0; i < 16; i++) if (allocations[i].p == p) {
        struct hardware *h = allocations[i].hw;
        if (h) {
            assert(!h->irq);
            r8169_device_t *n = h->device->drv_priv;
            /* remove clears drv_priv just before freeing the context. */
            if (allocations[i].context) n = p;
            assert(n && n->closing && n->hardware_stopped && !n->lock.held);
            if (n->regs) assert(h->confirmed && !h->dead);
            h->frees++;
        }
        allocations[i].p = NULL; free(p); return;
    }
    abort();
}
static void kfree(void *p) { retire(p); }
static void *dma_alloc_coherent_aligned(device_t *d, size_t bytes, size_t alignment, uint64_t *dma) {
    assert(alignment == PAGE_SIZE || alignment == R8169_DESC_ALIGN);
    if (d) assert(d->managed && d->stop);
    void *p = allocate(d ? d->hw : NULL, bytes, 0); *dma = (uintptr_t)p; return p;
}
static void dma_free_coherent_aligned(void *p, size_t bytes, uint64_t dma) {
    assert((uintptr_t)p == dma);
    for (unsigned i = 0; i < 16; i++) if (allocations[i].p == p) assert(allocations[i].bytes == bytes);
    retire(p);
}
static void arch_dma_sync_for_device(void *p, size_t bytes) {
    assert(p && bytes);
    for (unsigned i = 0; i < 16; i++)
        if ((uintptr_t)p >= (uintptr_t)allocations[i].p &&
            (uintptr_t)p + bytes <= (uintptr_t)allocations[i].p + allocations[i].bytes) {
            allocations[i].synced = 1; return;
        }
    abort();
}
static void arch_dma_sync_for_cpu(void *p, size_t bytes) { arch_dma_sync_for_device(p, bytes); }
static const char *bootargs_get(void) { return cmdline; }
static void random_fill(void *p, size_t bytes) { memset(p, 0x3c, bytes); }
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *), void *p) {
    assert(d->managed && d->drv_priv == p && !release);
    if (d->hw->fail_action) return -ENOMEM;
    d->stop = stop; d->context = p; return 0;
}
static int pci_enable_and_assign_bars(device_t *d) { assert(d->managed); current_hw = d->hw; d->hw->device = d; return 0; }
static resource_t *pci_get_bar_resource(device_t *d, unsigned bar) { assert(bar == 2); return &d->bar; }
static void *pci_map_bar(device_t *d, unsigned bar) { assert(bar == 2 && d->stop); return d->hw->fail_map ? NULL : d->hw->regs; }
static int dma_set_mask(device_t *d, uint64_t mask) { assert(mask == DMA_MASK_64BIT); return d->hw->fail_mask ? -EOPNOTSUPP : 0; }
static int pci_intx_irq(device_t *d) { return 11 + (int)(d->hw - hardware); }
static int request_irq(uint32_t irq, int (*fn)(int, void *), uint32_t flags, void *p) {
    device_t *d = p; assert(irq == (unsigned)pci_intx_irq(d) && flags == IRQF_SHARED && d->stop);
    if (d->hw->fail_irq) return -EBUSY;
    d->hw->irq = 1;
    /* An interrupt can arrive as soon as the handler is installed. */
    d->hw->regs[R8169_INTRSTAT] = R8169_IRQ_ROK;
    assert(!fn((int)irq, d) && !d->hw->regs[R8169_INTRSTAT] && !d->hw->stack_calls);
    d->hw->early_irq++; return 0;
}
static void free_irq(uint32_t irq, void *p) {
    device_t *d = p; r8169_device_t *n = d->drv_priv;
    assert(irq == (unsigned)pci_intx_irq(d) && d->hw->irq && d->hw->confirmed);
    assert(n->closing && n->hardware_stopped && !n->lock.held && !d->hw->dead);
    d->hw->irq = 0; d->hw->irq_frees++;
}
static void net_stack_device_interrupt(device_t *d) { d->hw->stack_calls++; }
/* PRODUCTION_FUNCTIONS */
static void writeb(uint8_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if (off == R8169_CHIPCMD && (v & R8169_CMD_RESET)) {
        h->resets++; h->confirmed = !h->stuck_reset && !h->stuck_engine;
        v = h->stuck_reset ? R8169_CMD_RESET : h->stuck_engine ? R8169_CMD_TE | R8169_CMD_RE : 0;
        if (!h->stuck_mask) { h->regs[R8169_INTRMASK] = h->regs[R8169_INTRMASK + 1] = 0; }
        h->regs[R8169_INTRSTAT] = h->regs[R8169_INTRSTAT + 1] = 0;
    } else if (off == R8169_CHIPCMD && (v & (R8169_CMD_TE | R8169_CMD_RE))) {
        r8169_device_t *n = h->device->drv_priv;
        for (unsigned i = 0; i < 16; i++) if (allocations[i].p == n->tx_ring ||
                allocations[i].p == n->rx_ring || allocations[i].p == n->rx_buf)
            assert(allocations[i].synced);
        h->confirmed = 0;
    }
    *(volatile uint8_t *)p = v;
    if (off == R8169_TXPOLL && h->stop_on_tx) {
        h->stop_on_tx = 0;
        assert(r8169_stop(h->device->drv_priv) == -EBUSY && !h->frees && h->irq);
        h->busy_stop++;
    }
}
static device_t fresh(unsigned index) {
    struct hardware *h = &hardware[index];
    for (unsigned i = 0; i < 16; i++) assert(!allocations[i].p || allocations[i].hw != h);
    memset(h, 0, sizeof(*h));
    static const uint8_t mac[6] = {0x52,0x54,0,0x12,0x34,0x56}; memcpy(h->regs, mac, sizeof(mac));
    h->regs[R8169_PHYSTATUS] = R8169_PHY_LINK_STATUS;
    cmdline = ""; g_r8169_poll_mode = 0;
    return (device_t){.name="R8169",.hw=h,.bar={0,255}};
}
static int cleanup(device_t *d) {
    int r = d->stop ? d->stop(d->context) : 0;
    if (!r) r = r8169_remove(d);
    if (!r) { d->stop = NULL; d->context = NULL; }
    return r;
}
int main(void) {
    uint8_t frame[64], out[128]; memset(frame, 0x5a, sizeof(frame));
    for (unsigned mode = 0; mode < 4; mode++) {
        device_t d = fresh(0); assert(!r8169_probe(&d)); r8169_device_t *n = d.drv_priv;
        assert(d.hw->early_irq == 1 && r8169_link_up(&d) && r8169_rx_irq_driven(&d));
        assert(r8169_send(&d, frame, sizeof(frame)) == sizeof(frame));
        r8169_poll(&d); assert(n->tx_busy == 1);
        n->tx_ring[0].opts1 &= ~R8169_DESC_OWN;
        r8169_poll(&d); assert(!n->tx_busy && !memcmp(n->tx_buf, frame, sizeof(frame)));
        memcpy(n->rx_buf, frame, sizeof(frame));
        n->rx_ring[0].opts1 = (sizeof(frame) + R8169_ETH_FCS_LEN) | R8169_DESC_FIRST | R8169_DESC_LAST;
        assert(r8169_recv(&d, out, sizeof(out)) == sizeof(frame) && !memcmp(out, frame, sizeof(frame)));
        d.hw->regs[R8169_INTRSTAT] = R8169_IRQ_ROK;
        assert(!r8169_irq_handler(n->irq, &d) && d.hw->stack_calls == 1);
        d.hw->stuck_reset = mode == 0; d.hw->stuck_engine = mode == 1;
        d.hw->stuck_mask = mode == 2; d.hw->dead = mode == 3;
        assert(cleanup(&d) == (mode == 3 ? -ENODEV : -ETIMEDOUT));
        assert(d.drv_priv == n && n->tx_ring && !d.hw->frees && d.hw->irq && !d.hw->irq_frees);
        int reads = d.hw->reads, writes = d.hw->writes;
        assert(r8169_send(&d, frame, sizeof(frame)) == -ENODEV && r8169_recv(&d, out, sizeof(out)) == -ENODEV);
        r8169_poll(&d); assert(!r8169_link_up(&d) && !r8169_rx_irq_driven(&d) && !r8169_mac(&d));
        assert(reads == d.hw->reads && writes == d.hw->writes);
        d.hw->regs[R8169_INTRSTAT] = R8169_IRQ_ROK;
        assert(!r8169_irq_handler(n->irq, &d) && d.hw->stack_calls == 1);
        if (!d.hw->dead) assert(!d.hw->regs[R8169_INTRSTAT]);
        writes = d.hw->writes; d.hw->dead = 1;
        assert(cleanup(&d) == -ENODEV && d.hw->writes == writes && !d.hw->frees);
        device_t peer = fresh(1); assert(!r8169_probe(&peer) && peer.drv_priv != n && !cleanup(&peer));
        assert(d.drv_priv == n && !d.hw->frees);
        d.hw->dead = d.hw->stuck_reset = d.hw->stuck_engine = d.hw->stuck_mask = 0;
        assert(!cleanup(&d) && !d.drv_priv && d.hw->frees == 5 && d.hw->irq_frees == 1);
        assert(!r8169_remove(&d));
    }
    device_t d = fresh(0); assert(!r8169_probe(&d)); d.hw->stop_on_tx = 1;
    assert(r8169_send(&d, frame, sizeof(frame)) == sizeof(frame) && d.hw->busy_stop == 1);
    assert(!cleanup(&d) && d.hw->frees == 5);
    for (unsigned fail = 0; fail < 10; fail++) {
        d = fresh(0);
        d.hw->fail_alloc = fail == 0 ? 1 : fail >= 4 && fail <= 7 ? fail - 2 : 0;
        d.hw->fail_action = fail == 1; d.hw->fail_map = fail == 2; d.hw->fail_mask = fail == 3;
        d.hw->stuck_reset = fail == 8; d.hw->dead = fail == 9;
        assert(r8169_probe(&d) < 0 && !d.hw->frees);
        if (fail == 8 || fail == 9) assert(cleanup(&d) < 0 && d.drv_priv);
        d.hw->dead = d.hw->stuck_reset = 0;
        assert(!cleanup(&d) && !d.drv_priv);
    }
    for (unsigned fallback = 0; fallback < 2; fallback++) {
        d = fresh(0); d.hw->fail_irq = fallback == 0;
        if (fallback) cmdline = "a20.r8169.poll=1";
        assert(!r8169_probe(&d) && !d.hw->irq && !r8169_rx_irq_driven(&d));
        assert(!cleanup(&d) && d.hw->frees == 5);
    }
    assert(!a20_boot_selftest_r8169());
    for (unsigned i = 0; i < 16; i++) assert(!allocations[i].p);
    return 0;
}
