#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define kinfo(...) ((void)0)
#define a20_perf_count(...) ((void)0)
#define IRQF_SHARED 1
#define DMA_MASK_32BIT UINT32_MAX
#define DRIVER_PROTOCOL_NET_STATS 1
#define va_to_pa(p) ((uintptr_t)(p) & UINT32_MAX)
typedef struct { int locked; } spinlock_t;
static void spin_init(spinlock_t *s) { s->locked = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *s) { assert(!s->locked); s->locked = 1; return 0; }
static int spin_trylock_irqsave(spinlock_t *s, uint64_t *flags) {
    if (s->locked) return 0;
    *flags = spin_lock_irqsave(s); return 1;
}
static void spin_unlock_irqrestore(spinlock_t *s, uint64_t flags) { (void)flags; assert(s->locked); s->locked = 0; }
struct hardware {
    uint8_t regs[256];
    int dead, stuck_reset, stuck_enable, reset_reads, confirmed, resets, irq;
    int reads, writes, frees, fail_action, fail_map, fail_mask, fail_alloc, fail_range, fail_irq;
    int stack_calls, stop_on_tx, busy_stop, delays;
    void *allocation;
    uint64_t dma;
    uint32_t producer;
};
static struct hardware hardware[2];
typedef struct { uint64_t start, end; } resource_t;
typedef struct device {
    void *drv_priv;
    struct hardware *hw;
    resource_t bar;
    int managed;
    int (*stop)(void *);
    void *context;
} device_t;
typedef struct { uint64_t rx_packets, rx_drops, tx_packets, tx_drops; } net_dev_stats_t;
struct driver_net_stats_ops { int (*read)(device_t *, net_dev_stats_t *); };
struct driver_protocol_descriptor {
    struct { unsigned id, version, ops_size, flags; } info;
    const void *ops;
};
static const struct driver_net_stats_ops rtl8139_stats_ops;
/* PRODUCTION_DEFINITIONS */
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + 256) return &hardware[i];
    abort();
}
static uint8_t readb(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if (h->dead) return 0xff;
    if (off == RTL8139_CR && h->reset_reads && !h->stuck_reset && !h->stuck_enable) {
        if (!--h->reset_reads) { h->regs[off] = 1; h->confirmed = 1; }
    }
    return h->regs[off];
}
static uint16_t readw(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    if (h->dead) return 0xffff;
    uint16_t v; memcpy(&v, (const void *)p, sizeof(v)); return v;
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    if (h->dead) return UINT32_MAX;
    uint32_t v; memcpy(&v, (const void *)p, sizeof(v)); return v;
}
static void writeb(uint8_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if (off == RTL8139_CR && (v & RTL8139_CR_RST)) {
        h->resets++; h->confirmed = 0; h->reset_reads = 3;
        if (h->stuck_enable) v = RTL8139_CR_TE | RTL8139_CR_RE;
    }
    *(volatile uint8_t *)p = v;
}
static void writew(uint16_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    if ((uintptr_t)p - (uintptr_t)h->regs == RTL8139_ISR) v = 0;
    if ((uintptr_t)p - (uintptr_t)h->regs == RTL8139_CAPR) {
        if (((v + 16U) & 0xffffU) == h->producer) h->regs[RTL8139_CR] |= 1U;
        else h->regs[RTL8139_CR] &= ~1U;
    }
    memcpy((void *)p, &v, sizeof(v));
}
static void writel(uint32_t v, volatile void *p);
static void udelay(unsigned us) {
    assert(us == 10 && !g_rtl8139.lock.locked);
    owner((void *)g_rtl8139.regs)->delays++;
}
static void arch_dma_sync_for_device(void *p, size_t n) { assert(p && n); }
static void arch_dma_sync_for_cpu(void *p, size_t n) { assert(p && n); }
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *), void *ctx) {
    assert(d->managed && d->drv_priv == ctx && !release);
    if (d->hw->fail_action) return -ENOMEM;
    d->stop = stop; d->context = ctx; return 0;
}
static int device_add_protocol(device_t *d, const struct driver_protocol_descriptor *p) {
    assert(d->managed && p->info.id == DRIVER_PROTOCOL_NET_STATS && p->ops); return 0;
}
static int pci_enable_and_assign_bars(device_t *d) { (void)d; return 0; }
static resource_t *pci_get_bar_resource(device_t *d, int bar) { assert(bar == 1); return &d->bar; }
static void *pci_map_bar(device_t *d, int bar) { assert(bar == 1 && d->managed && d->stop); return d->hw->fail_map ? NULL : d->hw->regs; }
static int pci_intx_irq(device_t *d) { (void)d; return 11; }
static int dma_set_mask(device_t *d, uint64_t mask) { assert(mask == DMA_MASK_32BIT); return d->hw->fail_mask ? -EOPNOTSUPP : 0; }
static int dma_range_ok(device_t *d, uint64_t dma, size_t n) { assert(dma == d->hw->dma && n == RTL8139_BUF_TOTAL); return !d->hw->fail_range; }
static void *dma_alloc_coherent_aligned(device_t *d, size_t n, size_t alignment, uint64_t *dma) {
    assert(d->managed && d->stop && n == RTL8139_BUF_TOTAL && alignment == RTL8139_TX_STRIDE);
    if (d->hw->fail_alloc) return NULL;
    void *p; assert(!posix_memalign(&p, 4096, n)); d->hw->allocation = p;
    d->hw->dma = *dma = 0x1000000U; return p;
}
static void dma_free_coherent_aligned(void *p, size_t n, uint64_t dma) {
    for (unsigned i = 0; i < 2; i++) if (hardware[i].allocation == p) {
        struct hardware *h = &hardware[i]; assert(h->confirmed && !h->irq && !h->dead);
        assert(n == RTL8139_BUF_TOTAL && dma == h->dma);
        h->frees++; h->allocation = NULL; free(p); return;
    }
    abort();
}
static int rtl8139_poll_mode;
static void rtl8139_parse_cmdline(void) {}
static int rtl8139_irq_handler(int irq, void *ctx);
static int request_irq(uint32_t irq, int (*fn)(int, void *), unsigned flags, void *ctx) {
    device_t *d = ctx; assert(irq == 11 && fn == rtl8139_irq_handler && flags == IRQF_SHARED);
    assert(!rtl8139_irq_handler(11, d) && !d->hw->stack_calls);
    if (d->hw->fail_irq) return -EIO;
    d->hw->irq = 1; return 0;
}
static void free_irq(uint32_t irq, void *ctx) {
    device_t *d = ctx; assert(irq == 11 && d->hw->confirmed && d->hw->irq);
    assert(!g_rtl8139.lock.locked); d->hw->irq = 0;
}
static void net_stack_device_interrupt(device_t *d) { d->hw->stack_calls++; }
/* PRODUCTION_FUNCTIONS */
static void writel(uint32_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    if (off == RTL8139_RBSTART) h->confirmed = 0;
    if (off == RTL8139_RBSTART) assert(v == g_rtl8139.buf_dma);
    if (off >= RTL8139_TSAD(0) && off <= RTL8139_TSAD(3)) {
        unsigned slot = (off - RTL8139_TSAD(0)) / 4;
        assert(v == g_rtl8139.buf_dma + (uintptr_t)g_rtl8139.tx[slot] - (uintptr_t)g_rtl8139.buf_mem);
        assert(v != va_to_pa(g_rtl8139.tx[slot])); /* Host VA is not a DMA handle. */
    }
    if (off >= RTL8139_TSD(0) && off <= RTL8139_TSD(3) && h->stop_on_tx) {
        h->stop_on_tx = 0;
        assert(rtl8139_stop(&g_rtl8139) == -EBUSY && h->irq && !h->frees); h->busy_stop++;
    }
    memcpy((void *)p, &v, sizeof(v));
}
static device_t fresh(unsigned index) {
    struct hardware *h = &hardware[index]; assert(!h->allocation); memset(h, 0, sizeof(*h));
    h->regs[0] = 0x52; h->regs[1] = 0x54;
    return (device_t){.hw=h,.bar={0,255}};
}
/* Model the core's ordered stop followed by class drain/remove callbacks. */
static int cleanup(device_t *d) {
    int r = d->stop ? d->stop(d->context) : 0;
    if (!r) r = rtl8139_remove(d);
    if (!r) { d->stop = NULL; d->context = NULL; }
    return r;
}
static uint32_t append_frame(rtl8139_device_t *n, uint32_t off, const char *data, unsigned bytes) {
    uint8_t header[4] = {1, 0, (bytes + 4U) & 255U, (bytes + 4U) >> 8};
    for (unsigned j = 0; j < 4U + bytes + 4U; j++)
        n->rx[(off + j) & 0xffffU] = j < 4 ? header[j] : (j < bytes + 4U ? data[j - 4U] : 0);
    return ((off + bytes + 8U + 3U) & ~3U) & 0xffffU;
}
int main(void) {
    device_t d = fresh(0), peer = fresh(1); assert(!rtl8139_probe(&d));
    rtl8139_device_t *n = d.drv_priv; assert(n == &g_rtl8139 && n->ready && d.managed && d.stop);
    assert(rtl8139_probe(&peer) == -EBUSY && !peer.drv_priv && !peer.hw->writes);
    char frame[64]; memset(frame, 0x5a, sizeof(frame));
    assert(rtl8139_send(&d, frame, sizeof(frame)) == sizeof(frame));
    assert(!memcmp(n->tx[0], frame, sizeof(frame)));
    uint16_t hdr[2] = {1, 64}; memcpy(n->rx, hdr, sizeof(hdr)); memcpy(n->rx + 4, frame, 60);
    d.hw->producer = 68; d.hw->regs[RTL8139_CR] &= ~1U;
    char received[128]; assert(rtl8139_recv(&d, received, sizeof(received)) == 60);
    assert(!memcmp(received, frame, 60));
    net_dev_stats_t st; assert(!rtl8139_stats_read(&d, &st) && st.rx_packets == 1);
    uint32_t second = append_frame(n, n->rx_read, frame, 61);
    uint32_t end = append_frame(n, second, frame, 63);
    d.hw->producer = end; d.hw->regs[RTL8139_CR] &= ~1U;
    assert(rtl8139_recv(&d, received, sizeof(received)) == 61 && n->rx_read == second);
    assert(rtl8139_recv(&d, received, sizeof(received)) == 63 && n->rx_read == end);
    memcpy(n->rx + n->rx_read, hdr, sizeof(hdr)); /* Stale header in an empty ring. */
    assert(!rtl8139_recv(&d, received, sizeof(received)) && n->rx_read == end);
    n->rx_read = 65520;
    end = append_frame(n, n->rx_read, frame, 61);
    d.hw->producer = end; d.hw->regs[RTL8139_CR] &= ~1U;
    assert(rtl8139_recv(&d, received, sizeof(received)) == 61 && n->rx_read == end);
    assert(!memcmp(received, frame, 61));
    void *saved = n->buf_mem; d.hw->stuck_reset = 1;
    assert(cleanup(&d) == -ETIMEDOUT && n->buf_mem == saved && d.drv_priv == n && d.hw->irq && !d.hw->frees);
    int reads = d.hw->reads, writes = d.hw->writes;
    assert(rtl8139_send(&d, frame, sizeof(frame)) == -ENODEV);
    assert(rtl8139_recv(&d, received, sizeof(received)) == -ENODEV);
    rtl8139_poll(&d); assert(rtl8139_stats_read(&d, &st) == -ENODEV && !rtl8139_rx_irq_driven(&d));
    assert(reads == d.hw->reads && writes == d.hw->writes);
    uint16_t pending = RTL8139_IRQ_TOK;
    memcpy(d.hw->regs + RTL8139_ISR, &pending, sizeof(pending));
    assert(!rtl8139_irq_handler(11, &d) && !d.hw->stack_calls);
    assert(!readw(d.hw->regs + RTL8139_ISR));
    writes = d.hw->writes;
    assert(rtl8139_probe(&peer) == -EBUSY && n->buf_mem == saved);
    d.hw->dead = 1; assert(cleanup(&d) == -ENODEV && d.hw->writes == writes && d.hw->irq && !d.hw->frees);
    d.hw->dead = d.hw->stuck_reset = 0; assert(!cleanup(&d) && !d.drv_priv && d.hw->frees == 1);

    d = fresh(0); assert(!rtl8139_probe(&d)); d.hw->stop_on_tx = 1;
    assert(rtl8139_send(&d, frame, sizeof(frame)) == sizeof(frame) && d.hw->busy_stop == 1);
    assert(!d.hw->frees && !cleanup(&d) && d.hw->frees == 1);
    d = fresh(0); assert(!rtl8139_probe(&d)); d.hw->stuck_enable = 1;
    assert(cleanup(&d) == -ETIMEDOUT && d.hw->irq && !d.hw->frees);
    d.hw->stuck_enable = 0; assert(!cleanup(&d));

    for (unsigned fail = 0; fail < 8; fail++) {
        d = fresh(0); d.hw->fail_action = fail == 0; d.hw->fail_map = fail == 1;
        d.hw->stuck_reset = fail == 2; d.hw->fail_mask = fail == 3;
        d.hw->fail_alloc = fail == 4; d.hw->fail_range = fail == 5;
        if (fail == 6) d.hw->regs[0] = 0;
        d.hw->dead = fail == 7;
        assert(rtl8139_probe(&d) < 0 && d.drv_priv == &g_rtl8139);
        if (fail == 2 || fail == 7) {
            assert(cleanup(&d) < 0 && d.drv_priv && !d.hw->allocation);
            assert(rtl8139_probe(&peer) == -EBUSY && !peer.drv_priv);
        }
        d.hw->stuck_reset = d.hw->dead = 0;
        assert(!cleanup(&d) && !d.drv_priv);
    }
    d = fresh(0); d.hw->fail_irq = 1;
    assert(!rtl8139_probe(&d) && !rtl8139_rx_irq_driven(&d) && !d.hw->irq);
    assert(rtl8139_send(&d, frame, sizeof(frame)) == sizeof(frame) && !cleanup(&d));
    assert(!hardware[0].allocation && !hardware[1].allocation);
    puts("PASS: RTL8139 reset ACK, retained failed probe, busy I/O, closed admission and safe slot reuse");
    return 0;
}
