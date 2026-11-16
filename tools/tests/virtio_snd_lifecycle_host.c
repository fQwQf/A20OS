#define _POSIX_C_SOURCE 200112L
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "drivers/audio/virtio_snd.h"
#include "drivers/block/virtio_blk.h"
#include "drivers/bus/virtio_transport.h"
#define ALIGNED(n) __attribute__((aligned(n)))
#define ROUND_UP(n, a) (((n) + (a) - 1) & ~((a) - 1))
#define MS_TO_TICKS(n) (n)
#define IRQF_SHARED 1U
#define RES_MMIO 1
#define RES_IRQ 2
#define mb() ((void)0)
#define wmb() ((void)0)
static void host_log(const char *fmt, ...) { (void)fmt; }
#define kinfo host_log
#define kerr host_log
typedef struct { int held; } mutex_t;
static void mutex_init(mutex_t *m) { m->held = 0; }
static void mutex_lock(mutex_t *m) { assert(!m->held); m->held = 1; }
static int mutex_trylock(mutex_t *m) { if (m->held) return 0; m->held = 1; return 1; }
static void mutex_unlock(mutex_t *m) { assert(m->held); m->held = 0; }
typedef struct { int initialized; } wait_queue_t;
typedef struct { int unused; } wait_queue_entry_t;
typedef struct { int unused; } proc_wake_q_t;
typedef int proc_wait_token_t;
#define PROC_WAKE_EVENT 1
#define PROC_WAIT_UNINTERRUPTIBLE 0
static void wait_queue_init(wait_queue_t *q) { q->initialized = 1; }
static void proc_wake_q_init(proc_wake_q_t *q) { (void)q; }
static void wait_queue_collect_all(wait_queue_t *q, int key, int reason, proc_wake_q_t *w, void *extra) {
    assert(q->initialized && !key && reason == PROC_WAKE_EVENT && w && !extra);
}
static void proc_wake_q_flush(proc_wake_q_t *q) { (void)q; }
static proc_wait_token_t proc_park_prepare(int kind, uint64_t until) { assert(kind == 0 && until); return 1; }
static void wait_queue_link(wait_queue_t *q, wait_queue_entry_t *e, int t, int key) { assert(q->initialized && e && t && !key); }
static void wait_queue_unlink(wait_queue_t *q, wait_queue_entry_t *e) { assert(q->initialized && e); }
static void proc_park_finish(int t) { assert(t == 1); }
static void *proc_current(void) { return (void *)1; }
static void arch_cpu_relax(void) {}
static uint64_t clock_ticks;
static uint64_t clock_get_ticks(void) { return ++clock_ticks; }
static uint64_t clock_ticks_per_sec(void) { return 1000; }
static void proc_yield(void);
static void proc_park_commit(int token) { assert(token == 1); proc_yield(); }
typedef struct { int unused; } bus_type_t;
static bus_type_t pci_bus;
struct hardware;
typedef struct { uint64_t start, end; } resource_t;
typedef struct device {
    const char *name;
    bus_type_t *bus;
    void *drv_priv;
    struct hardware *hw;
    int managed;
    int (*stop)(void *);
    void *context;
    resource_t bar, irq;
} device_t;
struct queue_hw { uint64_t desc, avail, used; uint16_t consumed; uint32_t ready; };
struct hardware {
    uint32_t regs[0x110 / 4], selected;
    struct queue_hw q[4];
    uint16_t pending[8]; unsigned pending_count;
    int dead, stuck_reset, confirmed, fail_action, fail_map, fail_transport;
    int fail_alloc, alloc_calls, fail_irq, no_version, reject_features, bad_queue;
    int fail_discovery, stick_after_discovery, stall_control, stall_tx, stop_wait, busy_stop;
    int irq_owned, irq_frees, frees, reads, writes, resets, running, legacy;
    size_t pcm_bytes;
    void *owner;
};
static struct hardware hardware[2], *current_hw;
static uint32_t readl(const volatile void *);
static void writel(uint32_t, volatile void *);
static void *kcalloc(size_t, size_t);
static void kfree(void *);
static void *dma_alloc_coherent_aligned(device_t *, size_t, size_t, uint64_t *);
static void dma_free_coherent_aligned(void *, size_t, uint64_t);
static void dma_sync_for_device(void *, size_t);
static void dma_sync_for_cpu(void *, size_t);
static int request_irq(uint32_t, int (*)(int, void *), unsigned long, void *);
static void free_irq(uint32_t, void *);
static int device_add_execution_action(device_t *, int (*)(void *), void (*)(void *), void *);
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int pci_virtio_transport_init(device_t *, uint32_t, virtio_transport_t *);
static resource_t *device_get_resource(device_t *d, int type, int index) {
    assert(!index); return type == RES_MMIO ? &d->bar : type == RES_IRQ ? &d->irq : NULL;
}
static void *device_map_mmio(device_t *d, int index) { assert(d->managed && !index); current_hw = d->hw; return d->hw->fail_map ? NULL : d->hw->regs; }
/* PRODUCTION_RESET */
/* PRODUCTION_SOUND */
struct allocation { void *p; size_t bytes; struct hardware *hw; int context; };
static struct allocation allocations[24];
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + sizeof(hardware[i].regs)) return &hardware[i];
    abort();
}
static void *allocate(struct hardware *h, size_t bytes, int context) {
    if (++h->alloc_calls == h->fail_alloc) return NULL;
    void *p; assert(!posix_memalign(&p, PAGE_SIZE, bytes)); memset(p, 0, bytes);
    for (unsigned i = 0; i < 24; i++) if (!allocations[i].p) {
        allocations[i] = (struct allocation){p, bytes, h, context}; return p;
    }
    abort();
}
static void *kcalloc(size_t n, size_t bytes) {
    assert(n == 1 && bytes == sizeof(virtio_snd_dev_t));
    void *p = allocate(current_hw, bytes, 1); current_hw->owner = p; return p;
}
static void retire(void *p) {
    for (unsigned i = 0; i < 24; i++) if (allocations[i].p == p) {
        struct hardware *h = allocations[i].hw; virtio_snd_dev_t *s = h->owner;
        assert(!h->irq_owned && s->closing && s->hardware_stopped && !s->lock.held);
        if (s->reset_required) assert(h->confirmed && !h->dead && !h->regs[VIRTIO_MMIO_STATUS / 4]);
        if (allocations[i].context) h->owner = NULL;
        h->frees++; allocations[i].p = NULL; free(p); return;
    }
    abort();
}
static void kfree(void *p) { retire(p); }
static void *dma_alloc_coherent_aligned(device_t *d, size_t bytes, size_t align, uint64_t *dma) {
    assert(d->managed && d->stop && align == PAGE_SIZE);
    void *p = allocate(d->hw, bytes, 0); *dma = (uintptr_t)p; return p;
}
static void dma_free_coherent_aligned(void *p, size_t bytes, uint64_t dma) {
    assert((uintptr_t)p == dma);
    for (unsigned i = 0; i < 24; i++) if (allocations[i].p == p) assert(allocations[i].bytes == bytes);
    retire(p);
}
static void dma_sync_for_device(void *p, size_t bytes) { assert(p && bytes); }
static void dma_sync_for_cpu(void *p, size_t bytes) { assert(p && bytes); }
static void proc_yield(void) {
    for (unsigned i = 0; i < 2; i++) if (hardware[i].stop_wait) {
        struct hardware *h = &hardware[i]; h->stop_wait = 0;
        virtio_snd_dev_t *s = h->owner; assert(s && s->lock.held);
        assert(virtio_snd_stop(s) == -EBUSY && !h->frees && h->irq_owned);
        h->busy_stop++;
    }
}
static int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *), void *p) {
    assert(d->managed && d->drv_priv == p && !release);
    if (d->hw->fail_action) return -ENOMEM;
    d->stop = stop; d->context = p; return 0;
}
static int request_irq(uint32_t irq, int (*handler)(int, void *), unsigned long flags, void *p) {
    virtio_snd_dev_t *s = p; struct hardware *h = owner(s->vt.priv);
    assert(irq == (unsigned)s->vt.irq && flags == (s->vt.shared_irq ? IRQF_SHARED : 0));
    assert(s->waiters.initialized && s->control && s->tx_dma);
    if (h->fail_irq) return -EBUSY;
    h->irq_owned = 1; h->regs[VIRTIO_MMIO_INTERRUPT_STATUS / 4] = 1;
    assert(!handler((int)irq, s) && !h->regs[VIRTIO_MMIO_INTERRUPT_STATUS / 4]); return 0;
}
static void free_irq(uint32_t irq, void *p) {
    virtio_snd_dev_t *s = p; struct hardware *h = owner(s->vt.priv);
    assert(irq == (unsigned)s->vt.irq && h->irq_owned && s->closing && s->hardware_stopped && !s->lock.held);
    assert(h->confirmed && !h->regs[VIRTIO_MMIO_STATUS / 4]); h->irq_owned = 0; h->irq_frees++;
}
static void complete_tx(struct hardware *h) {
    struct queue_hw *q = &h->q[VIRTIO_SND_QUEUE_TX];
    virtq_desc_t *desc = (void *)(uintptr_t)q->desc;
    virtq_used_t *used = (void *)(uintptr_t)q->used;
    for (unsigned i = 0; i < h->pending_count; i++) {
        uint16_t id = h->pending[i];
        const uint8_t *pcm = (void *)(uintptr_t)desc[id + 1].addr;
        for (unsigned j = 0; j < desc[id + 1].len; j++) assert(pcm[j] == 0x5a);
        h->pcm_bytes += desc[id + 1].len;
        virtio_snd_pcm_status_t *status = (void *)(uintptr_t)desc[id + 2].addr;
        status->status = VIRTIO_SND_S_OK; status->latency_bytes = 0;
        used->ring[used->idx % VIRTIO_QUEUE_SIZE] = (virtq_used_elem_t){id, sizeof(*status)};
        used->idx++;
    }
    h->pending_count = 0;
}
static void notify(struct hardware *h, unsigned index) {
    assert(index < 4);
    if (index == VIRTIO_SND_QUEUE_EVENT) return;
    struct queue_hw *q = &h->q[index];
    virtq_avail_t *avail = (void *)(uintptr_t)q->avail;
    virtq_desc_t *desc = (void *)(uintptr_t)q->desc;
    virtq_used_t *used = (void *)(uintptr_t)q->used;
    assert(q->ready && (h->regs[VIRTIO_MMIO_STATUS / 4] & VIRTIO_STATUS_DRIVER_OK));
    while (q->consumed != avail->idx) {
        uint16_t head = avail->ring[q->consumed++ % VIRTIO_QUEUE_SIZE];
        if (index == VIRTIO_SND_QUEUE_TX) {
            assert(h->pending_count < 8); h->pending[h->pending_count++] = head;
            if (h->running && !h->stall_tx) complete_tx(h);
            continue;
        }
        assert(index == VIRTIO_SND_QUEUE_CONTROL && head == 0);
        if (h->stall_control) continue;
        virtio_snd_hdr_t *request = (void *)(uintptr_t)desc[0].addr;
        uint32_t *response = (void *)(uintptr_t)desc[1].addr; *response = VIRTIO_SND_S_OK;
        if (request->code == VIRTIO_SND_R_PCM_INFO) {
            if (h->fail_discovery) {
                *response = VIRTIO_SND_S_IO_ERR;
                if (h->stick_after_discovery) h->stuck_reset = 1;
            } else {
                virtio_snd_pcm_info_t info = {.formats=1ULL << VIRTIO_SND_PCM_FMT_S16,
                    .rates=1ULL << VIRTIO_SND_PCM_RATE_48000, .direction=VIRTIO_SND_D_OUTPUT,
                    .channels_min=2, .channels_max=2};
                assert(desc[1].len == sizeof(info) + 4); memcpy(response + 1, &info, sizeof(info));
            }
        } else if (request->code == VIRTIO_SND_R_PCM_START) {
            h->running = 1; if (!h->stall_tx) complete_tx(h);
        } else if (request->code == VIRTIO_SND_R_PCM_STOP) h->running = 0;
        else if (request->code == VIRTIO_SND_R_PCM_RELEASE && !h->stall_tx) complete_tx(h);
        used->ring[used->idx % VIRTIO_QUEUE_SIZE] = (virtq_used_elem_t){0, desc[1].len}; used->idx++;
    }
    h->regs[VIRTIO_MMIO_INTERRUPT_STATUS / 4] = 1;
}
static uint32_t transport_read(virtio_transport_t *t, uint32_t off) {
    struct hardware *h = owner(t->priv); h->reads++;
    if (h->dead) return UINT32_MAX;
    if (off == VIRTIO_MMIO_DEVICE_FEATURES) return h->no_version ? 0 : VIRTIO_F_VERSION_1_BIT;
    if (off == VIRTIO_MMIO_QUEUE_NUM_MAX) return h->bad_queue ? 1 : VIRTIO_QUEUE_SIZE;
    if (off == VIRTIO_MMIO_QUEUE_READY) return h->q[h->selected].ready;
    return h->regs[off / 4];
}
static void address_half(uint64_t *address, unsigned off, uint32_t value) {
    if (off & 4U) *address = (*address & UINT32_MAX) | ((uint64_t)value << 32);
    else *address = (*address & ~((uint64_t)UINT32_MAX)) | value;
}
static void transport_write(virtio_transport_t *t, uint32_t off, uint32_t value) {
    struct hardware *h = owner(t->priv); assert(!h->dead); h->writes++;
    if (off == VIRTIO_MMIO_STATUS) {
        if (!value) {
            h->resets++; if (h->stuck_reset) return;
            h->confirmed = 1; memset(h->q, 0, sizeof(h->q)); h->pending_count = 0; h->running = 0;
        } else { h->confirmed = 0; if (h->reject_features) value &= ~VIRTIO_STATUS_FEATURES_OK; }
    }
    h->regs[off / 4] = value;
    if (off == VIRTIO_MMIO_QUEUE_SEL) { assert(value < 4); h->selected = value; }
    struct queue_hw *q = &h->q[h->selected];
    if (off == VIRTIO_MMIO_QUEUE_READY) { q->ready = value; h->confirmed = 0; }
    if (off == VIRTIO_MMIO_QUEUE_DESC_LOW || off == VIRTIO_MMIO_QUEUE_DESC_HIGH) address_half(&q->desc, off, value);
    if (off == VIRTIO_MMIO_QUEUE_DRIVER_LOW || off == VIRTIO_MMIO_QUEUE_DRIVER_HIGH) address_half(&q->avail, off, value);
    if (off == VIRTIO_MMIO_QUEUE_DEVICE_LOW || off == VIRTIO_MMIO_QUEUE_DEVICE_HIGH) address_half(&q->used, off, value);
    if (off == VIRTIO_MMIO_INTERRUPT_ACK) h->regs[VIRTIO_MMIO_INTERRUPT_STATUS / 4] &= ~value;
    if (off == VIRTIO_MMIO_QUEUE_NOTIFY) notify(h, value);
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h = owner(p); virtio_transport_t t = {.priv=h->regs};
    return transport_read(&t, (uintptr_t)p - (uintptr_t)h->regs);
}
static void writel(uint32_t value, volatile void *p) {
    struct hardware *h = owner(p); virtio_transport_t t = {.priv=h->regs};
    transport_write(&t, (uintptr_t)p - (uintptr_t)h->regs, value);
}
static int pci_virtio_transport_init(device_t *d, uint32_t id, virtio_transport_t *t) {
    assert(d->managed && id == VIRTIO_SND_DEVICE_ID); current_hw = d->hw;
    if (d->hw->fail_transport) return -ENODEV;
    *t = (virtio_transport_t){.read32=transport_read,.write32=transport_write,.priv=d->hw->regs,
        .irq=(int)d->irq.start,.shared_irq=1,.legacy=d->hw->legacy}; return 0;
}
static device_t fresh(unsigned index, int pci) {
    struct hardware *h = &hardware[index];
    for (unsigned i = 0; i < 24; i++) assert(!allocations[i].p || allocations[i].hw != h);
    memset(h, 0, sizeof(*h)); h->regs[VIRTIO_MMIO_MAGIC / 4] = 0x74726976;
    h->regs[VIRTIO_MMIO_VERSION / 4] = 2; h->regs[VIRTIO_MMIO_DEVICE_ID / 4] = VIRTIO_SND_DEVICE_ID;
    h->regs[VIRTIO_MMIO_STATUS / 4] = VIRTIO_STATUS_ACKNOWLEDGE;
    h->regs[(VIRTIO_MMIO_CONFIG + VIRTIO_SND_CONFIG_STREAMS) / 4] = 1;
    return (device_t){.name="VirtIO Sound",.bus=pci ? &pci_bus : NULL,.hw=h,.bar={0,0x10f},.irq={11+index,11+index}};
}
static int cleanup_device(device_t *d) {
    int ret = d->stop ? d->stop(d->context) : 0;
    if (!ret) ret = virtio_snd_remove(d);
    if (!ret) { d->stop = NULL; d->context = NULL; }
    return ret;
}
int main(void) {
    uint8_t pcm[VIRTIO_SND_PERIOD_BYTES * 9]; memset(pcm, 0x5a, sizeof(pcm));
    for (int pci = 0; pci <= 1; pci++) {
        device_t d = fresh(0, pci); assert(!virtio_snd_probe(&d)); virtio_snd_dev_t *s = d.drv_priv;
        for (unsigned cycle = 0; cycle < 2; cycle++) {
            assert(virtio_snd_write(&d, pcm, 8192) == 8192 && !virtio_snd_drain_device(&d));
            assert(!virtio_snd_stop_device(&d) && !s->closing);
        }
        assert(d.hw->pcm_bytes == 16384);
        d.hw->stuck_reset = 1; void *dma = s->tx_dma;
        assert(cleanup_device(&d) == -ETIMEDOUT && d.drv_priv == s && s->tx_dma == dma && d.hw->irq_owned && !d.hw->frees);
        unsigned generation = s->generation; int reads = d.hw->reads, writes = d.hw->writes;
        assert(virtio_snd_write(&d, pcm, 4) == -ENODEV && virtio_snd_stop_device(&d) == -ENODEV);
        assert(virtio_snd_drain_device(&d) == -ENODEV && virtio_snd_close(&d) == -ENODEV);
        assert(reads == d.hw->reads && writes == d.hw->writes);
        d.hw->regs[VIRTIO_MMIO_INTERRUPT_STATUS / 4] = 1;
        assert(!virtio_snd_irq_handler(s->vt.irq, s) && !d.hw->regs[VIRTIO_MMIO_INTERRUPT_STATUS / 4]);
        d.hw->dead = 1; writes = d.hw->writes;
        assert(cleanup_device(&d) == -ENODEV && s->generation == generation && d.hw->writes == writes && !d.hw->frees);
        device_t peer = fresh(1, pci); assert(!virtio_snd_probe(&peer) && peer.drv_priv != s && !cleanup_device(&peer));
        d.hw->dead = d.hw->stuck_reset = 0;
        assert(!cleanup_device(&d) && !d.drv_priv && d.hw->frees == 8 && d.hw->irq_frees == 1);
    }
    for (int control = 0; control <= 1; control++) {
        device_t d = fresh(0, 1); assert(!virtio_snd_probe(&d)); virtio_snd_dev_t *s = d.drv_priv;
        d.hw->stall_control = control; d.hw->stall_tx = !control; d.hw->stop_wait = 1;
        assert(virtio_snd_write(&d, pcm, sizeof(pcm)) == -EINTR && d.hw->busy_stop == 1);
        assert(d.drv_priv == s && s->closing && !d.hw->frees && d.hw->irq_owned);
        if (!control) assert(s->inflight == 8);
        assert(!cleanup_device(&d) && d.hw->frees == 8);
    }
    device_t d = fresh(0, 1); d.hw->fail_discovery = d.hw->stick_after_discovery = 1;
    assert(virtio_snd_probe(&d) == -EIO && d.drv_priv && !d.hw->frees);
    assert(cleanup_device(&d) == -ETIMEDOUT && d.drv_priv && d.hw->irq_owned);
    d.hw->stuck_reset = 0; assert(!cleanup_device(&d) && d.hw->frees == 8);
    for (unsigned failure = 0; failure < 17; failure++) {
        d = fresh(0, 1);
        d.hw->fail_alloc = failure == 0 ? 1 : failure >= 7 && failure <= 13 ? failure - 5 : 0;
        d.hw->fail_action = failure == 1; d.hw->fail_transport = failure == 2;
        d.hw->no_version = failure == 3; d.hw->reject_features = failure == 4;
        if (failure == 5) d.hw->regs[(VIRTIO_MMIO_CONFIG + VIRTIO_SND_CONFIG_STREAMS) / 4] = 0;
        d.hw->bad_queue = failure == 6; d.hw->stuck_reset = failure == 14;
        if (failure == 15) d.hw->regs[VIRTIO_MMIO_MAGIC / 4] = 0;
        d.hw->legacy = failure == 16;
        assert(virtio_snd_probe(&d) < 0 && !d.hw->frees);
        if (failure == 14) assert(cleanup_device(&d) == -ETIMEDOUT && d.drv_priv);
        d.hw->stuck_reset = 0; assert(!cleanup_device(&d) && !d.drv_priv);
    }
    d = fresh(0, 0); d.hw->fail_map = 1; assert(virtio_snd_probe(&d) < 0 && !cleanup_device(&d));
    d = fresh(0, 1); assert(!virtio_snd_probe(&d)); virtio_snd_dev_t *s = d.drv_priv;
    d.hw->regs[VIRTIO_MMIO_STATUS / 4] |= VIRTIO_STATUS_DEVICE_NEEDS_RESET;
    int resets = d.hw->resets; assert(virtio_snd_health(s) == -EIO && s->broken && d.hw->resets == resets && !d.hw->frees);
    assert(!cleanup_device(&d));
    d = fresh(0, 1); d.hw->fail_irq = 1;
    assert(!virtio_snd_probe(&d) && !d.hw->irq_owned && !cleanup_device(&d));
    for (unsigned i = 0; i < 24; i++) assert(!allocations[i].p);
    puts("PASS: VirtIO sound managed stop, retained IRQ/control/PCM DMA, cancellation and retry");
    return 0;
}
