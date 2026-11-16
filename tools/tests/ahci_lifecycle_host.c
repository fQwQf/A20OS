#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define AHCI_PLATFORM_F_READ_ONLY 1U
#define AHCI_PLATFORM_F_PRESERVE_FIRMWARE_LINK 2U
#define RES_MMIO 1
#define A20_PERF_AHCI_COMMANDS 0
#define a20_perf_count(x) ((void)(x))
typedef struct { int unused; } wait_queue_t;
typedef struct { int locked; } mutex_t;
typedef struct { int locked; } spinlock_t;
#define SPINLOCK_INIT {0}
static uint64_t spin_lock_irqsave(spinlock_t *l) { assert(!l->locked); l->locked = 1; return 0; }
static void spin_unlock_irqrestore(spinlock_t *l, uint64_t f) { (void)f; assert(l->locked); l->locked = 0; }
static void mutex_init(mutex_t *m) { m->locked = 0; }
static void mutex_lock(mutex_t *m) { assert(!m->locked); m->locked = 1; }
static int mutex_trylock(mutex_t *m) { if (m->locked) return 0; m->locked = 1; return 1; }
static void mutex_unlock(mutex_t *m) { assert(m->locked); m->locked = 0; }
static void wait_queue_init(wait_queue_t *q) { (void)q; }
typedef struct block_dev {
    int (*read_sector)(struct block_dev *, uint64_t, void *, size_t);
    int (*write_sector)(struct block_dev *, uint64_t, const void *, size_t);
    int (*flush)(struct block_dev *);
    uint64_t capacity;
    uint32_t sector_size;
    void *priv;
    void (*release)(struct block_dev *);
} block_dev_t;
struct hardware {
    uint32_t regs[2048];
    int dead, stuck_cr, stuck_fr, fail_identify, fail_alloc, allocs, frees, irq;
    int stop_on_io, submits, stopped_while_busy, fail_map, ownership_error, writes;
};
static struct hardware hardware[2];
static int pci_bus;
typedef struct { uint64_t start, end; } resource_t;
typedef struct {
    void *bus, *drv_priv;
    struct hardware *hw;
    resource_t bar;
    int managed, fail_action;
    int (*stop)(void *);
    void *context;
} device_t;
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + sizeof(hardware[i].regs))
            return &hardware[i];
    abort();
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h = owner(p);
    return h->dead ? UINT32_MAX : *(const volatile uint32_t *)p;
}
static void writel(uint32_t v, volatile void *p) {
    struct hardware *h = owner(p);
    unsigned off = (uintptr_t)p - (uintptr_t)h->regs;
    assert(!h->dead);
    h->writes++;
    if (off == 4) v &= ~1U; /* HBA reset completes. */
    if (off == 0x118) {
        uint32_t old = h->regs[off / 4];
        /* FRE may only be cleared after the command engine acknowledges. */
        if ((old & (1U << 15)) && !(v & (1U << 4))) abort();
        v &= ~((1U << 15) | (1U << 14));
        if ((v & 1) || h->stuck_cr) v |= 1U << 15;
        if ((v & (1U << 4)) || h->stuck_fr) v |= 1U << 14;
    }
    *(volatile uint32_t *)p = v;
}
static void mdelay(unsigned ms) { (void)ms; }
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *), void *ctx) {
    assert(d->managed && d->drv_priv == ctx && !release);
    if (d->fail_action) return -ENOMEM;
    d->stop = stop; d->context = ctx; return 0;
}
static resource_t *pci_get_bar_resource(device_t *d, int i) { assert(i == 5); return &d->bar; }
static resource_t *device_get_resource(device_t *d, int type, int i) {
    assert(type == RES_MMIO && i == 0); return &d->bar;
}
static void *pci_map_bar(device_t *d, int i) { assert(i == 5 && d->managed); return d->hw->fail_map ? NULL : d->hw->regs; }
static void *device_map_mmio(device_t *d, int i) { assert(!i); return pci_map_bar(d, 5); }
struct allocation { void *p; struct hardware *h; } allocations[16];
static void *dma_alloc_coherent_aligned(device_t *d, size_t n, size_t align, uint64_t *dma) {
    (void)align;
    assert(d->managed && d->stop);
    if (++d->hw->allocs == d->hw->fail_alloc) return NULL;
    void *p = calloc(1, n); assert(p); *dma = (uintptr_t)p;
    for (unsigned i = 0; i < 16; i++) if (!allocations[i].p) {
        allocations[i] = (struct allocation){p, d->hw}; return p;
    }
    abort();
}
static void dma_free_coherent_aligned(void *p, size_t n, uint64_t dma) {
    (void)n; assert((uintptr_t)p == dma);
    for (unsigned i = 0; i < 16; i++) if (allocations[i].p == p) {
        struct hardware *h = allocations[i].h;
        assert(!(h->regs[0x118 / 4] & ((1U << 15) | (1U << 14))) && !h->irq && !h->dead);
        allocations[i].p = NULL; h->frees++; free(p); return;
    }
    abort();
}
static void dma_sync_for_device(void *p, size_t n) { assert(p && n); }
/* PRODUCTION_DEFINITIONS */
static int ahci_wait_complete(ahci_port_t *, size_t);
static void free_irq(uint32_t n, void *ctx) {
    ahci_port_t *p = ctx; struct hardware *h = owner((void *)p->host_regs);
    assert(n == 11 && h->irq && !(h->regs[0x118 / 4] & ((1U << 15) | (1U << 14))));
    h->irq = 0;
}
static void pci_msi_disable(device_t *d) { assert(!d->hw->irq); }
static void irq_free_vectors(uint32_t n, unsigned count) { assert(n == 11 && count == 1); }
static int ahci_poll_requested(void) { return 0; }
static int ahci_configure_dma_mask(device_t *d, uint32_t cap) { (void)d; (void)cap; return 0; }
static int ahci_take_ownership(uintptr_t regs, uint32_t version) {
    (void)version; return owner((void *)regs)->ownership_error;
}
static void ahci_pci_enable_ports(device_t *d, uint32_t pi) { (void)d; (void)pi; }
static int ahci_wait_link(ahci_port_t *p) { return owner((void *)p->host_regs)->regs[0x128 / 4] == 3; }
static int ahci_claim_irq(device_t *d, ahci_port_t *p, int irq) {
    assert(irq == 11); d->hw->irq = 1; p->irq = irq; p->irq_registered = 1; p->msi = 1; return 0;
}
/* PRODUCTION_FUNCTIONS */
static int ahci_wait_complete(ahci_port_t *p, size_t bytes) {
    struct hardware *h = owner((void *)p->host_regs);
    assert(h->regs[0x138 / 4] == 1);
    h->submits++;
    if (p->tables[0].cfis[2] == ATA_CMD_IDENTIFY) {
        if (h->fail_identify) { h->stuck_cr = 1; return -EIO; }
        ((uint16_t *)p->transfer)[100] = 32768;
    } else if (bytes) memset(p->transfer, 0x5a, bytes);
    h->regs[0x138 / 4] = 0;
    if (h->stop_on_io) {
        h->stop_on_io = 0;
        assert(ahci_stop(p) == -EBUSY && h->irq && !h->frees);
        h->stopped_while_busy++;
    }
    return 0;
}
static device_t fresh(unsigned idx) {
    struct hardware *h = &hardware[idx]; memset(h, 0, sizeof(*h));
    h->regs[0x10 / 4] = 0x10300; h->regs[0xc / 4] = 1; h->regs[0x128 / 4] = 3;
    return (device_t){.bus=&pci_bus,.hw=h,.bar={0,sizeof(h->regs)-1}};
}
/* Models the core's ordered execution-stop then driver-remove callbacks. */
static int cleanup(device_t *d) {
    int r = d->stop ? d->stop(d->context) : 0;
    if (!r) r = ahci_remove(d);
    if (!r) { d->stop = NULL; d->context = NULL; }
    return r;
}
int main(void) {
    device_t d = fresh(0), other = fresh(1);
    d.hw->fail_identify = 1;
    assert(ahci_probe_common(&d, 11, 0, 0) == -EIO);
    void *saved = d.drv_priv;
    assert(saved && d.managed && cleanup(&d) == -ETIMEDOUT && !d.hw->frees);
    assert(ahci_probe_common(&other, 11, 0, 0) == -EBUSY && !other.drv_priv && !other.hw->writes);
    assert(d.drv_priv == saved);
    d.hw->stuck_cr = 0;
    assert(cleanup(&d) == 0 && d.hw->frees == 4 && !g_ahci_claimed);

    d = fresh(0); assert(!ahci_probe_common(&d, 11, 0, 0));
    ahci_port_t *p = d.drv_priv;
    d.hw->stuck_fr = 1;
    assert(cleanup(&d) == -ETIMEDOUT && d.hw->irq && !d.hw->frees);
    assert(!ahci_get_dev(0));
    d.hw->dead = 1;
    int writes = d.hw->writes;
    assert(cleanup(&d) == -ENODEV && d.hw->writes == writes && d.hw->irq && !d.hw->frees);
    d.hw->dead = 0; d.hw->stuck_fr = 0;
    assert(!cleanup(&d) && d.hw->frees == 4);

    d = fresh(0); assert(!ahci_probe_common(&d, 11, 0, 0));
    block_dev_t *b = ahci_get_dev(0), *b2 = ahci_get_dev(0);
    assert(b && b == b2 && b->release && !ahci_get_dev(1));
    char buf[512]; assert(!b->read_sector(b, 0, buf, 1) && buf[0] == 0x5a);
    assert(!b->flush(b));
    assert(cleanup(&d) == -EBUSY && d.drv_priv && d.hw->irq && !d.hw->frees);
    assert(b->read_sector(b, 0, buf, 1) == -ENODEV && b->flush(b) == -ENODEV);
    b->release(b); assert(cleanup(&d) == -EBUSY);
    b2->release(b2); assert(!cleanup(&d) && !d.drv_priv);

    d = fresh(0); assert(!ahci_probe_common(&d, 11, 0, 0)); p = d.drv_priv;
    d.hw->stop_on_io = 1;
    char *large = calloc(1, AHCI_TRANSFER_BYTES + 512); assert(large);
    assert(ahci_rw(p, 0, large, AHCI_TRANSFER_SECTORS + 1, 0) == -ENODEV);
    assert(d.hw->submits == 2 && d.hw->stopped_while_busy == 1 && !d.hw->frees);
    free(large); assert(!cleanup(&d));

    for (int fail = 1; fail <= 4; fail++) {
        d = fresh(0); d.hw->fail_alloc = fail;
        assert(ahci_probe_common(&d, 11, 0, 0) == -ENOMEM);
        assert(!cleanup(&d) && d.hw->frees == 3);
    }
    d = fresh(0); d.fail_action = 1;
    assert(ahci_probe_common(&d, 11, 0, 0) == -ENOMEM && !cleanup(&d) && !d.hw->writes);
    d = fresh(0); d.hw->fail_map = 1;
    assert(ahci_probe_common(&d, 11, 0, 0) == -EIO && !cleanup(&d) && !d.hw->writes);
    d = fresh(0); d.hw->ownership_error = -ETIMEDOUT;
    assert(ahci_probe_common(&d, 11, 0, 0) == -ETIMEDOUT && !cleanup(&d) && !d.hw->writes);
    d = fresh(0); d.hw->regs[0x128 / 4] = 0;
    assert(ahci_probe_common(&d, 11, 0, 0) == -ENODEV && !cleanup(&d) && !d.hw->allocs);
    d = fresh(0); d.bus = NULL;
    assert(!ahci_probe_common(&d, 11, AHCI_PLATFORM_F_PRESERVE_FIRMWARE_LINK, 0));
    assert(!cleanup(&d));
    for (unsigned i = 0; i < 16; i++) assert(!allocations[i].p);
    puts("PASS: AHCI stop ACK, failed-probe retention/retry, live I/O, legacy pins and partial setup");
    return 0;
}
