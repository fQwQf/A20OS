#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ALIGNED(n) __attribute__((aligned(n)))
#define kinfo(...) ((void)0)
#define kerr(...) ((void)0)
#define A20_BLK_IRQ_POLL 0
#define A20_BLK_IRQ_INTX 1
#define A20_BLK_IRQ_MSIX 2
#define VIRTIO_ID_SCSI 8
#define IRQF_SHARED 1
#define US_TO_TICKS(n) ((n) / 1000)
#define MS_TO_TICKS(n) (n)
#define PROC_WAIT_UNINTERRUPTIBLE 0
#define mb() ((void)0)
#define wmb() ((void)0)
#define va_to_pa(p) ((uintptr_t)(p))
typedef struct { int locked; } mutex_t;
typedef struct { int locked; } spinlock_t;
#define SPINLOCK_INIT {0}
static void mutex_init(mutex_t *m) { m->locked = 0; }
static void mutex_lock(mutex_t *m) { assert(!m->locked); m->locked = 1; }
static int mutex_trylock(mutex_t *m) { if (m->locked) return 0; m->locked = 1; return 1; }
static void mutex_unlock(mutex_t *m) { assert(m->locked); m->locked = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *s) { assert(!s->locked); s->locked = 1; return 0; }
static void spin_unlock_irqrestore(spinlock_t *s, uint64_t flags) { (void)flags; assert(s->locked); s->locked = 0; }
typedef int wait_queue_t;
typedef int wait_queue_entry_t;
typedef int proc_wait_token_t;
static void wait_queue_init(wait_queue_t *q) { *q = 0; }
static proc_wait_token_t proc_park_prepare(int kind, uint64_t deadline) { (void)kind; (void)deadline; return 0; }
static void wait_queue_link(wait_queue_t *q, wait_queue_entry_t *e, int t, int n) { (void)q; (void)e; (void)t; (void)n; }
static void wait_queue_unlink(wait_queue_t *q, wait_queue_entry_t *e) { (void)q; (void)e; }
static int proc_park_commit(int t) { (void)t; return 0; }
static int proc_park_cancel(int t) { (void)t; return 0; }
static void proc_park_finish(int t) { (void)t; }
static uint64_t ticks;
static uint64_t timer_get_ticks(void) { return ticks += 10; }
static uint64_t clock_get_ticks(void) { return timer_get_ticks(); }
static uint64_t clock_ticks_per_sec(void) { return 1000; }
static void arch_cpu_relax(void) {}
static void *proc_current(void) { return NULL; }
static void proc_yield(void) {}
static void arch_dma_sync_for_device(void *p, size_t n) { assert(p && n); }
static void arch_dma_sync_for_cpu(void *p, size_t n) { assert(p && n); }
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
    uint32_t status, queue, queue_ready[3];
    int stuck, timeout, fail_transport, fail_action, fail_alloc, fail_queue;
    int irq, confirmed, resets, allocs, frees, notify, unit_attention, bad_id, bad_idx;
    int resid, sense_len, stop_on_io, busy_stop, features, bad_len;
    void *dma, *expected;
};
typedef struct {
    const char *name;
    void *drv_priv;
    struct hardware *hw;
    int managed;
    int (*stop)(void *);
    void *context;
} device_t;
/* PRODUCTION_DEFINITIONS */
static uint32_t model_read(virtio_transport_t *vt, uint32_t off) {
    struct hardware *h = vt->priv;
    switch (off) {
    case VIRTIO_MMIO_STATUS: if (!h->status) h->confirmed = 1; return h->status;
    case VIRTIO_MMIO_DEVICE_FEATURES: return h->features ? VIRTIO_F_VERSION_1_BIT : 0;
    case VIRTIO_MMIO_QUEUE_NUM_MAX: return h->queue == (unsigned)h->fail_queue ? 0 : 32;
    case VIRTIO_MMIO_QUEUE_READY: return h->queue_ready[h->queue];
    default: return 0;
    }
}
static void model_write(virtio_transport_t *, uint32_t, uint32_t);
static void model_teardown(virtio_transport_t *vt) {
    struct hardware *h = vt->priv; assert(h->confirmed && !h->irq); vt->msix_vectors = 0;
}
static int model_prepare(virtio_transport_t *vt, unsigned count) {
    assert(count == 1); vt->msix_base = 11; vt->msix_vectors = 1; return 0;
}
static void model_arm(virtio_transport_t *vt) { assert(((struct hardware *)vt->priv)->irq); }
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *), void *ctx) {
    assert(d->managed && d->drv_priv == ctx && !release);
    if (d->hw->fail_action) return -ENOMEM;
    d->stop = stop; d->context = ctx; return 0;
}
static int pci_virtio_transport_init(device_t *d, int id, virtio_transport_t *vt) {
    assert(id == 8 && d->managed && d->stop);
    if (d->hw->fail_transport) return -EIO;
    *vt = (virtio_transport_t){.priv=d->hw,.read32=model_read,.write32=model_write,
        .irq=7,.shared_irq=1,.msix_prepare=model_prepare,.msix_arm=model_arm,.msix_teardown=model_teardown};
    return 0;
}
static void *dma_alloc_coherent_aligned(device_t *d, size_t n, size_t align, uint64_t *dma) {
    (void)align; assert(d->managed && d->stop && n == VIRTIO_SCSI_DMA_BYTES);
    if (d->hw->fail_alloc) return NULL;
    void *p = calloc(1, n); assert(p); *dma = (uintptr_t)p; d->hw->dma = p; d->hw->allocs++; return p;
}
static struct hardware *find_hw(void *p) {
    for (int i = 0; i < VIRTIO_SCSI_MAX_DEVS; i++)
        if (g_scsi[i].device && g_scsi[i].bounce == p) return g_scsi[i].vt.priv;
    abort();
}
static void dma_free_coherent_aligned(void *p, size_t n, uint64_t dma) {
    struct hardware *h = find_hw(p);
    assert(h->confirmed && !h->irq && n == VIRTIO_SCSI_DMA_BYTES && (uintptr_t)p == dma);
    h->frees++; h->dma = NULL; free(p);
}
static int virtio_scsi_force_poll(void) { return 0; }
static int virtio_scsi_irq_handler(int irq, void *ctx) { (void)irq; (void)ctx; return 0; }
static int request_irq(uint32_t irq, int (*handler)(int, void *), unsigned flags, void *ctx) {
    virtio_scsi_dev_t *d = ctx; (void)flags; assert(irq == 11 && handler == virtio_scsi_irq_handler);
    ((struct hardware *)d->vt.priv)->irq = 1; return 0;
}
static void free_irq(uint32_t irq, void *ctx) {
    virtio_scsi_dev_t *d = ctx; struct hardware *h = d->vt.priv;
    assert(irq == 11 && h->confirmed && h->irq); h->irq = 0;
}
/* PRODUCTION_FUNCTIONS */
static void model_write(virtio_transport_t *vt, uint32_t off, uint32_t val) {
    struct hardware *h = vt->priv;
    if (off == VIRTIO_MMIO_STATUS) {
        if (!val) {
            h->resets++; h->confirmed = 0;
            if (!h->stuck) { h->status = 0; memset(h->queue_ready, 0, sizeof(h->queue_ready)); }
        } else { assert(!h->stuck); h->status = val; }
    } else if (off == VIRTIO_MMIO_QUEUE_SEL) h->queue = val;
    else if (off == VIRTIO_MMIO_QUEUE_READY) h->queue_ready[h->queue] = val;
    else if (off == VIRTIO_MMIO_QUEUE_NOTIFY) {
        virtio_scsi_dev_t *d = (void *)vt; assert(val == 2); h->notify++;
        assert(d->desc[0].addr == va_to_pa(&d->req));
        size_t bytes = d->desc[0].next == 2 && !(d->desc[2].flags & VIRTQ_DESC_F_NEXT) ? 0 : d->desc[1].len;
        if (bytes) assert(d->desc[1].addr == d->bounce_dma && d->desc[1].addr != (uintptr_t)h->expected);
        if (h->stop_on_io) {
            h->stop_on_io = 0; assert(virtio_scsi_stop(d) == -EBUSY && h->irq); h->busy_stop++;
        }
        if (h->timeout) return;
        if (bytes) {
            if (d->desc[1].flags & VIRTQ_DESC_F_WRITE) memset(d->bounce, 0x5a, bytes);
            else if (h->expected) assert(!memcmp(d->bounce, h->expected, bytes));
        }
        if (d->req.cdb[0] == SCSI_CMD_READ_CAPACITY10) {
            put_be32(d->bounce, 32767); put_be32(d->bounce + 4, 512);
        }
        if (h->unit_attention) {
            h->unit_attention--; d->resp.status = SCSI_STATUS_CHECK_CONDITION;
            d->resp.sense_len = 3; d->resp.sense[0] = 0x70; d->resp.sense[2] = 6;
            d->resp.resid = bytes;
        } else { d->resp.resid = h->resid; d->resp.sense_len = h->sense_len; }
        d->used.ring[d->used.idx % VIRTIO_SCSI_QUEUE_SIZE].id = h->bad_id ? 7 : 0;
        d->used.ring[d->used.idx % VIRTIO_SCSI_QUEUE_SIZE].len = h->bad_len ? 0 :
            sizeof(d->resp) + bytes;
        if (d->resp.status == SCSI_STATUS_CHECK_CONDITION)
            d->used.ring[d->used.idx % VIRTIO_SCSI_QUEUE_SIZE].len = 15;
        d->used.idx += h->bad_idx ? 2 : 1;
    }
}
static device_t fresh(struct hardware *h) {
    memset(h, 0, sizeof(*h)); h->features = 1; h->fail_queue = -1;
    return (device_t){.name="test",.hw=h};
}
static int cleanup(device_t *d) {
    int r = d->stop ? d->stop(d->context) : 0;
    if (!r) r = virtio_scsi_remove(d);
    if (!r) { d->stop = NULL; d->context = NULL; }
    return r;
}
int main(void) {
    struct hardware h, peer_hw;
    device_t d = fresh(&h); h.unit_attention = 1;
    assert(!virtio_scsi_probe(&d) && h.notify == 3 && virtio_scsi_ready(0));
    virtio_scsi_dev_t *s = d.drv_priv;
    block_dev_t *b = virtio_scsi_get_dev(0), *b2 = virtio_scsi_get_dev(0);
    assert(b && b == b2 && s->consumer_refs == 2 && virtio_scsi_ready(0));
    char *buf = malloc(66048); assert(buf); memset(buf, 0x4b, 66048);
    h.expected = buf; assert(!b->write_sector(b, 0, buf, 129));
    h.expected = NULL; h.unit_attention = 1;
    assert(!b->read_sector(b, 0, buf, 129));
    for (unsigned i = 0; i < 66048; i++) assert(buf[i] == 0x5a);
    assert(!b->flush(b) && cleanup(&d) == -EBUSY && h.irq && !h.frees);
    assert(!virtio_scsi_get_dev(0) && !virtio_scsi_ready(0) && b->flush(b) == -ENODEV);
    b->release(b); assert(cleanup(&d) == -EBUSY);
    b2->release(b2); assert(!cleanup(&d) && h.frees == 1); free(buf);

    d = fresh(&h); assert(!virtio_scsi_probe(&d)); s = d.drv_priv;
    buf = malloc(512); assert(buf); memset(buf, 0x4b, 512); h.timeout = 1; h.expected = buf;
    assert(virtio_scsi_read(&s->block, 0, buf, 1) == -ETIMEDOUT && s->closing);
    for (unsigned i = 0; i < 512; i++) assert(buf[i] == 0x4b);
    uint8_t snapshot[sizeof(s->desc)]; memcpy(snapshot, s->desc, sizeof(snapshot));
    int submits = h.notify;
    assert(virtio_scsi_read(&s->block, 0, buf, 1) == -ENODEV && h.notify == submits);
    assert(!memcmp(snapshot, s->desc, sizeof(snapshot)));
    free(buf); memset(h.dma, 0x7e, 512); /* Late DMA cannot reach the freed caller. */
    h.stuck = 1;
    assert(cleanup(&d) == -ETIMEDOUT && d.drv_priv == s && h.irq && !h.frees);
    device_t peer = fresh(&peer_hw); assert(!virtio_scsi_probe(&peer) && peer.drv_priv != s);
    assert(s->bounce == h.dma && s->desc[1].addr == s->bounce_dma);
    assert(!cleanup(&peer)); h.stuck = 0; assert(!cleanup(&d) && h.frees == 1);

    for (unsigned bad = 0; bad < 5; bad++) {
        d = fresh(&h); assert(!virtio_scsi_probe(&d)); s = d.drv_priv;
        char out[512]; memset(out, 0x4b, sizeof(out));
        h.bad_id = bad == 0; h.bad_idx = bad == 1; h.resid = bad == 2 ? 1 : 0; h.sense_len = bad == 3 ? 97 : 0;
        h.bad_len = bad == 4;
        assert(virtio_scsi_read(&s->block, 0, out, 1) < 0);
        for (unsigned i = 0; i < sizeof(out); i++) assert(out[i] == 0x4b);
        assert(!cleanup(&d));
    }
    d = fresh(&h); assert(!virtio_scsi_probe(&d)); s = d.drv_priv;
    h.stop_on_io = 1; char large[66048];
    assert(virtio_scsi_read(&s->block, 0, large, 129) == -ENODEV && h.busy_stop == 1);
    assert(!cleanup(&d));

    d = fresh(&h); h.fail_queue = 1;
    assert(virtio_scsi_probe(&d) < 0 && h.queue_ready[0]); s = d.drv_priv; h.stuck = 1;
    assert(cleanup(&d) == -ETIMEDOUT && s->device == &d && !h.frees);
    peer = fresh(&peer_hw); assert(!virtio_scsi_probe(&peer) && peer.drv_priv != s);
    assert(!cleanup(&peer)); h.stuck = 0; assert(!cleanup(&d));
    for (unsigned fail = 0; fail < 5; fail++) {
        d = fresh(&h); h.fail_action = fail == 0; h.fail_transport = fail == 1;
        h.fail_alloc = fail == 2; h.features = fail != 3;
        if (fail == 4) { h.stuck = 1; h.status = 7; }
        assert(virtio_scsi_probe(&d) < 0 && d.drv_priv);
        if (fail == 4) assert(!h.allocs && cleanup(&d) == -ETIMEDOUT);
        h.stuck = 0; assert(!cleanup(&d) && !d.drv_priv);
    }
    puts("PASS: production SCSI timeout/late-DMA isolation, reset ACK, failed probe, reference pins and completion validation");
    return 0;
}
