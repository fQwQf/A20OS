#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define PAGE_SIZE_BITS 12
#define MS_TO_TICKS(n) (n)
#define IRQF_SHARED 1
#define IRQF_ONESHOT 2
#define kinfo(...) ((void)0)
typedef struct {
    int unused;
} wait_queue_t;
typedef struct {
    int locked;
} mutex_t;
static void mutex_init(mutex_t *m) { m->locked = 0; }
static void mutex_lock(mutex_t *m) {
    assert(!m->locked);
    m->locked = 1;
}
static int mutex_trylock(mutex_t *m) {
    if (m->locked)
        return 0;
    m->locked = 1;
    return 1;
}
static void mutex_unlock(mutex_t *m) {
    assert(m->locked);
    m->locked = 0;
}
struct hardware {
    uint8_t regs[8192];
    uint64_t cap;
    int ready, stuck, dead, cfs, fail_identify, fail_alloc, allocs, frees, irq, reset, stop_on_io,
        submits;
};
static struct hardware hardware[2];
typedef struct {
    uint64_t start, end;
} resource_t;
typedef struct {
    const char *name;
    void *drv_priv;
    struct hardware *hw;
    resource_t bar;
    int managed, fail_action;
    int (*stop)(void *);
    void *context;
} device_t;
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + 8192)
            return &hardware[i];
    abort();
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h = owner(p);
    assert((uintptr_t)p - (uintptr_t)h->regs == 0x1c);
    return h->dead ? UINT32_MAX : ((unsigned)h->ready | (h->cfs ? 2U : 0));
}
static uint64_t readq(const volatile void *p) { return owner(p)->cap; }
static void writel(uint32_t v, volatile void *p) {
    struct hardware *h = owner(p);
    uintptr_t off = (uintptr_t)p - (uintptr_t)h->regs;
    if (off == 0x14) {
        if (!v) {
            h->reset++;
            if (!h->stuck)
                h->ready = 0;
        } else
            h->ready = 1;
    }
}
static void writeq(uint64_t v, volatile void *p) {
    (void)v;
    (void)p;
}
static void mdelay(unsigned ms) { (void)ms; }
static uint32_t pci_class_code(device_t *d) {
    (void)d;
    return 0x010802;
}
static int device_probe_use_remove(device_t *d) {
    d->managed = 1;
    return 0;
}
static int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *),
                                       void *ctx) {
    assert(d->managed && d->drv_priv == ctx && !release);
    if (d->fail_action)
        return -ENOMEM;
    d->stop = stop;
    d->context = ctx;
    return 0;
}
static int pci_enable_and_assign_bars(device_t *d) {
    assert(d->managed);
    return 0;
}
static resource_t *pci_get_bar_resource(device_t *d, int i) {
    assert(!i);
    return &d->bar;
}
static void *pci_map_bar(device_t *d, int i) {
    assert(!i && d->managed);
    return d->hw->regs;
}
static int pci_intx_irq(device_t *d) {
    (void)d;
    return 11;
}
struct allocation {
    void *p;
    struct hardware *h;
} allocations[32];
static void *dma_alloc_coherent_aligned(device_t *d, size_t bytes, size_t alignment,
                                        uint64_t *dma) {
    (void)alignment;
    assert(d->managed && d->stop);
    if (d->hw->fail_alloc && ++d->hw->allocs == d->hw->fail_alloc)
        return NULL;
    if (!d->hw->fail_alloc)
        d->hw->allocs++;
    void *p = calloc(1, bytes);
    assert(p);
    *dma = (uintptr_t)p;
    for (unsigned i = 0; i < 32; i++)
        if (!allocations[i].p) {
            allocations[i] = (struct allocation){p, d->hw};
            return p;
        }
    abort();
}
static void dma_free_coherent_aligned(void *p, size_t bytes, uint64_t dma) {
    (void)bytes;
    assert((uintptr_t)p == dma);
    for (unsigned i = 0; i < 32; i++)
        if (allocations[i].p == p) {
            struct hardware *h = allocations[i].h;
            assert(!h->ready && !h->dead && !h->irq);
            h->frees++;
            allocations[i].p = NULL;
            free(p);
            return;
        }
    abort();
}
static void *kcalloc(size_t n, size_t bytes) { return calloc(n, bytes); }
static void kfree(void *p) { free(p); }
static void dma_sync_for_device(void *p, size_t n) {
    (void)p;
    (void)n;
}
static void dma_sync_for_cpu(void *p, size_t n) {
    (void)p;
    (void)n;
}
/* PRODUCTION_DEFINITIONS */
static inline volatile void *nvme_reg(nvme_controller_t *, uint32_t);
static int nvme_stop(void *);
static int nvme_irq_handler(int n, void *p) {
    (void)n;
    (void)p;
    return 0;
}
static int request_irq(uint32_t n, int (*fn)(int, void *), unsigned flags, void *p) {
    nvme_controller_t *c = p;
    (void)n;
    assert(fn == nvme_irq_handler && flags == (IRQF_SHARED | IRQF_ONESHOT));
    owner((void *)c->regs)->irq = 1;
    return 0;
}
static void free_irq(uint32_t n, void *p) {
    nvme_controller_t *c = p;
    struct hardware *h = owner((void *)c->regs);
    (void)n;
    assert(!h->ready && !h->dead && h->irq);
    h->irq = 0;
}
static int nvme_queue_alloc(nvme_controller_t *c, nvme_queue_t *q, uint16_t id, uint16_t depth,
                            device_t *d) {
    (void)c;
    (void)id;
    q->sq = dma_alloc_coherent_aligned(d, PAGE_SIZE, PAGE_SIZE, &q->sq_dma);
    q->cq = dma_alloc_coherent_aligned(d, PAGE_SIZE, PAGE_SIZE, &q->cq_dma);
    q->depth = depth;
    return q->sq && q->cq ? 0 : -ENOMEM;
}
static int nvme_admin(nvme_controller_t *c, uint8_t op, uint32_t ns, uint64_t prp, uint32_t a,
                      uint32_t b, uint32_t *r) {
    (void)c;
    (void)op;
    (void)ns;
    (void)prp;
    (void)a;
    (void)b;
    (void)r;
    return 0;
}
static int nvme_identify(nvme_controller_t *c) {
    struct hardware *h = owner((void *)c->regs);
    if (h->fail_identify) {
        h->stuck = 1;
        return -EIO;
    }
    c->capacity = 1024;
    c->sector_size = 512;
    c->namespace_id = 1;
    return 0;
}
static int nvme_submit(nvme_controller_t *c, nvme_queue_t *q, const nvme_sqe_t *r, uint32_t *out) {
    (void)q;
    (void)r;
    (void)out;
    struct hardware *h = owner((void *)c->regs);
    assert(h->ready && !c->stopping);
    h->submits++;
    if (h->stop_on_io) {
        h->stop_on_io = 0;
        assert(nvme_stop(c) == -EBUSY);
    }
    return 0;
}
/* PRODUCTION_FUNCTIONS */
static device_t device(unsigned i) {
    struct hardware *h = &hardware[i];
    memset(h, 0, sizeof(*h));
    h->cap = NVME_CAP_CSS_NVM | 63U;
    return (device_t){.name = "test", .hw = h, .bar = {0, 8191}};
}
static void removed(device_t *d) { assert(!nvme_remove(d) && !d->drv_priv && !d->hw->irq); }
int main(void) {
    device_t d = device(0);
    d.hw->fail_identify = 1;
    assert(nvme_probe(&d) == -EIO && d.drv_priv && d.stop);
    nvme_controller_t *c = d.drv_priv;
    void *sq = c->admin.sq;
    assert(nvme_remove(&d) == -ETIMEDOUT && d.drv_priv == c && c->admin.sq == sq && !d.hw->frees);
    assert(c->stopping && !c->stopped);
    /* Another controller cannot replace the retained failed-probe context. */
    device_t other = device(1);
    assert(!nvme_probe(&other) && other.drv_priv != c);
    removed(&other);
    d.hw->stuck = 0;
    removed(&d);
    assert(d.hw->allocs == d.hw->frees);
    d = device(0);
    assert(!nvme_probe(&d));
    c = d.drv_priv;
    c->io_lock.locked = 1;
    int resets = d.hw->reset;
    assert(d.stop(c) == -EBUSY && d.hw->reset == resets && d.hw->irq && !d.hw->frees);
    c->io_lock.locked = 0;
    char data[NVME_DMA_BYTES + 512];
    assert(nvme_rw(&d, 0, data, 1, 0) == -ENODEV && !d.hw->submits);
    assert(nvme_flush(&d) == -ENODEV);
    removed(&d);
    d = device(0);
    assert(!nvme_probe(&d));
    c = d.drv_priv;
    d.hw->stuck = 1;
    assert(nvme_remove(&d) == -ETIMEDOUT && d.drv_priv == c && d.hw->irq && !d.hw->frees);
    d.hw->dead = 1;
    assert(nvme_remove(&d) == -ENODEV && d.drv_priv == c && d.hw->irq && !d.hw->frees);
    d.hw->dead = 0;
    d.hw->stuck = 0;
    d.hw->cfs = 1;
    removed(&d);
    /* Managed action allocation and unsupported-capability failures did not
     * enable this controller and must not reset unknown hardware. */
    d = device(0);
    d.fail_action = 1;
    assert(nvme_probe(&d) == -ENOMEM && d.drv_priv);
    removed(&d);
    assert(!d.hw->reset);
    d = device(0);
    d.hw->cap = 0;
    assert(nvme_probe(&d) == -EOPNOTSUPP && d.drv_priv);
    removed(&d);
    assert(!d.hw->reset);
    /* A partial ring allocation remains owned and is freed only after stop. */
    d = device(0);
    d.hw->fail_alloc = 2;
    assert(nvme_probe(&d) == -EIO && d.drv_priv);
    removed(&d);
    assert(d.hw->frees == 1);
    d = device(0);
    assert(!nvme_probe(&d));
    d.hw->stop_on_io = 1;
    assert(nvme_rw(&d, 0, data, sizeof(data) / 512, 1) == -ENODEV && d.hw->submits == 1);
    removed(&d);
    for (unsigned i = 0; i < 32; i++)
        assert(!allocations[i].p);
    puts("NVME_LIFECYCLE: PASS failed probe, busy I/O, timeout/removed MMIO retention, fatal "
         "disable acknowledgement, partial init and retry");
    return 0;
}
