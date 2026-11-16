#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096U
#define MS_TO_TICKS(n) (n)
#define DMA_MASK_64BIT UINT64_MAX
#define DMA_MASK_32BIT UINT32_MAX
#define POLLOUT 4
#define POLLERR 8
static void host_log(const char *format, ...) { (void)format; }
#define kinfo host_log
#define kerr host_log
typedef struct { int held; } mutex_t;
typedef struct { int held; } spinlock_t;
static void mutex_init(mutex_t *m) { m->held = 0; }
static void mutex_lock(mutex_t *m) { assert(!m->held); m->held = 1; }
static int mutex_trylock(mutex_t *m) { if (m->held) return 0; m->held = 1; return 1; }
static void mutex_unlock(mutex_t *m) { assert(m->held); m->held = 0; }
static void spin_init(spinlock_t *s) { s->held = 0; }
static uint64_t spin_lock_irqsave(spinlock_t *s) { assert(!s->held); s->held = 1; return 0; }
static void spin_unlock_irqrestore(spinlock_t *s, uint64_t flags) { (void)flags; assert(s->held); s->held = 0; }
#include "drivers/audio/hda_internal.h"
#include "drivers/audio/backend.h"
struct hardware {
    uint32_t regs[256];
    int dead, stuck_reset, stuck_run, dead_on_enable, fail_action, fail_map, fail_mask;
    int fail_codec, fail_alloc, allocs, frees, writes, reads, reset_requests, advance;
    int stop_on_yield, busy_stop, bad_gcap, dma32, bad_dma;
    hda_controller_t *controller;
};
static struct hardware hardware[2], *allocating_hw;
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
static int hda_stop(void *);
struct allocation { void *p; struct hardware *hw; uint64_t dma; } allocations[8];
static struct hardware *owner(const volatile void *p) {
    for (unsigned i = 0; i < 2; i++)
        if ((uintptr_t)p >= (uintptr_t)hardware[i].regs &&
            (uintptr_t)p < (uintptr_t)hardware[i].regs + sizeof(hardware[i].regs)) return &hardware[i];
    abort();
}
static uint32_t readl(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    if (h->dead) return UINT32_MAX;
    if (h->controller && (uintptr_t)p == h->controller->stream + HDA_SD_LPIB && h->advance)
        *(volatile uint32_t *)p = (*(volatile uint32_t *)p + 4096U) % HDA_DMA_BYTES;
    return *(const volatile uint32_t *)p;
}
static uint16_t readw(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    if (h->dead || (h->bad_gcap && (uintptr_t)p == (uintptr_t)h->regs)) return UINT16_MAX;
    uint16_t v; memcpy(&v, (const void *)p, sizeof(v)); return v;
}
static uint8_t readb(const volatile void *p) {
    struct hardware *h = owner(p); h->reads++;
    return h->dead ? UINT8_MAX : *(const volatile uint8_t *)p;
}
static void writel(uint32_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    if ((uintptr_t)p == (uintptr_t)h->regs + HDA_REG_GCTL) {
        if (!(v & HDA_GCTL_RESET)) {
            h->reset_requests++;
            if (h->stuck_reset) v |= HDA_GCTL_RESET;
            else if (h->controller && h->controller->stream && !h->stuck_run)
                *(uint32_t *)h->controller->stream = 0;
        } else if (h->dead_on_enable) h->dead = 1;
    }
    if (h->controller && (uintptr_t)p == h->controller->stream && h->stuck_run && !(v & HDA_SD_CTL_RUN))
        v |= HDA_SD_CTL_RUN;
    *(volatile uint32_t *)p = v;
}
static void writeb(uint8_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++;
    *(volatile uint8_t *)p &= ~v;
}
static void writew(uint16_t v, volatile void *p) {
    struct hardware *h = owner(p); assert(!h->dead); h->writes++; memcpy((void *)p, &v, sizeof(v));
}
static uint64_t ticks;
static uint64_t timer_get_ticks(void) { return ticks += 10; }
static void mdelay(unsigned ms) {
    assert(ms == 1);
    for (unsigned i = 0; i < 2; i++) if (hardware[i].controller)
        assert(!hardware[i].controller->state_lock.held);
}
static void proc_yield(void) {
    for (unsigned i = 0; i < 2; i++) if (hardware[i].stop_on_yield) {
        struct hardware *h = &hardware[i]; h->stop_on_yield = 0;
        assert(hda_stop(h->controller) == -EBUSY && !h->frees); h->busy_stop++;
    }
}
static int device_probe_use_remove(device_t *d) { d->managed = 1; return 0; }
static int device_add_execution_action(device_t *d, int (*stop)(void *), void (*release)(void *), void *ctx) {
    assert(d->managed && d->drv_priv == ctx && !release);
    if (d->hw->fail_action) return -ENOMEM;
    d->stop = stop; d->context = ctx; return 0;
}
static uint32_t pci_class_code(device_t *d) { (void)d; return 0x040300U; }
static int pci_enable_and_assign_bars(device_t *d) { assert(d->managed); allocating_hw = d->hw; return 0; }
static resource_t *pci_get_bar_resource(device_t *d, unsigned bar) { assert(!bar); return &d->bar; }
static void *pci_map_bar(device_t *d, unsigned bar) { assert(!bar && d->stop); return d->hw->fail_map ? NULL : d->hw->regs; }
static int dma_set_mask(device_t *d, uint64_t mask) {
    assert(mask == (d->hw->dma32 ? DMA_MASK_32BIT : DMA_MASK_64BIT)); return d->hw->fail_mask ? -EOPNOTSUPP : 0;
}
static void *allocate(struct hardware *h, size_t bytes, uint64_t dma) {
    if (++h->allocs == h->fail_alloc) return NULL;
    void *p; assert(!posix_memalign(&p, PAGE_SIZE, bytes)); memset(p, 0, bytes);
    for (unsigned i = 0; i < 8; i++) if (!allocations[i].p) {
        allocations[i] = (struct allocation){p, h, dma}; return p;
    }
    abort();
}
static void *kcalloc(size_t n, size_t bytes) {
    assert(n == 1 && bytes == sizeof(hda_controller_t));
    void *p = allocate(allocating_hw, bytes, 0); allocating_hw->controller = p; return p;
}
static void *dma_alloc_coherent_aligned(device_t *d, size_t bytes, size_t alignment, uint64_t *dma) {
    assert(d->managed && d->stop && alignment == PAGE_SIZE);
    *dma = d->hw->dma32 ? 0x1000000U + (unsigned)d->hw->allocs * 0x10000U : 0x100000000ULL + (unsigned)d->hw->allocs * 0x10000U;
    if (d->hw->bad_dma) *dma |= 1ULL << 32;
    return allocate(d->hw, bytes, *dma);
}
static void retire(void *p, uint64_t dma) {
    for (unsigned i = 0; i < 8; i++) if (allocations[i].p == p) {
        struct hardware *h = allocations[i].hw;
        assert(allocations[i].dma == dma);
        if (h->writes) {
            assert(!h->dead && !(h->regs[HDA_REG_GCTL / 4] & HDA_GCTL_RESET));
            if (h->controller && h->controller->stream)
                assert(!(*(uint32_t *)h->controller->stream & HDA_SD_CTL_RUN));
        }
        if (p == h->controller) h->controller = NULL;
        allocations[i].p = NULL; h->frees++; free(p); return;
    }
    abort();
}
static void kfree(void *p) { retire(p, 0); }
static void dma_free_coherent_aligned(void *p, size_t bytes, uint64_t dma) { assert(bytes == PAGE_SIZE || bytes == HDA_DMA_BYTES); retire(p, dma); }
static void dma_sync_for_device(void *p, size_t bytes) { assert(p && bytes); }
int hda_codec_discover_and_setup(hda_controller_t *h, const char **stage) {
    struct hardware *hw = owner((void *)h->regs); *stage = "modeled-codec";
    h->codec = h->afg = h->dac = h->pin = 1; h->codec_configured = 1;
    return hw->fail_codec ? -EIO : 0;
}
/* PRODUCTION_FUNCTIONS */
static device_t fresh(unsigned index, int dma32) {
    struct hardware *h = &hardware[index];
    for (unsigned i = 0; i < 8; i++) assert(!allocations[i].p || allocations[i].hw != h);
    memset(h, 0, sizeof(*h)); h->dma32 = dma32; h->regs[0] = dma32 ? 0x1100 : 0x1101;
    h->regs[HDA_REG_GCTL / 4] = HDA_GCTL_RESET;
    return (device_t){.name="HDA",.hw=h,.bar={0,sizeof(h->regs)-1}};
}
static int cleanup(device_t *d) {
    int r = d->stop ? d->stop(d->context) : 0;
    if (!r) r = hda_remove(d);
    if (!r) { d->stop = NULL; d->context = NULL; }
    return r;
}
static void submit(device_t *d) {
    uint8_t data[8192]; memset(data, 0x5a, sizeof(data));
    audio_backend_write_t request = {.data=data,.bytes=sizeof(data),.start_after_bytes=4};
    assert(hda_backend_ioctl(d, AUDIO_BACKEND_WRITE_NONBLOCK, &request) == sizeof(data));
    assert(d->hw->controller->stream_running && !memcmp(d->hw->controller->pcm, data, sizeof(data)));
}
int main(void) {
    for (int dma32 = 0; dma32 <= 1; dma32++) {
        device_t d = fresh(0, dma32); assert(!hda_probe(&d)); hda_controller_t *h = d.drv_priv;
        submit(&d); d.hw->advance = 1; assert(!hda_drain_device(&d) && !h->stream_running);
        d.hw->advance = 0; submit(&d); d.hw->stuck_reset = 1;
        void *pcm = h->pcm;
        assert(cleanup(&d) == -ETIMEDOUT && d.drv_priv == h && h->pcm == pcm && !d.hw->frees);
        uint8_t data[4] = {0}; audio_backend_write_t request = {.data=data,.bytes=4}; audio_backend_status_t status;
        unsigned writes = d.hw->writes, reads = d.hw->reads;
        assert(hda_write(&d, data, 4) == -ENODEV && hda_drain_device(&d) == -ENODEV && hda_stop_device(&d) == -ENODEV);
        assert(hda_backend_ioctl(&d, AUDIO_BACKEND_QUERY, &status) == -ENODEV);
        assert(hda_backend_ioctl(&d, AUDIO_BACKEND_WRITE_NONBLOCK, &request) == -ENODEV);
        assert(hda_backend_ioctl(&d, AUDIO_BACKEND_START, NULL) == -ENODEV);
        assert(hda_stream_run(h, h->generation) == -ENODEV && hda_poll(&d, POLLOUT) == POLLERR);
        assert(d.hw->reads == (int)reads && d.hw->writes == (int)writes);
        d.hw->dead = 1;
        assert(cleanup(&d) == -ENODEV && d.drv_priv == h && !d.hw->frees);
        assert(hda_wait32(h, HDA_REG_GCTL, 1, 1) == -ENODEV);
        device_t peer = fresh(1, dma32); assert(!hda_probe(&peer) && peer.drv_priv != h);
        assert(h->pcm == pcm && !d.hw->frees && !cleanup(&peer));
        d.hw->dead = d.hw->stuck_reset = 0;
        assert(!cleanup(&d) && !d.drv_priv && d.hw->frees == 3);
    }
    device_t d = fresh(0, 0); assert(!hda_probe(&d)); hda_controller_t *h = d.drv_priv;
    d.hw->stop_on_yield = 1; uint8_t *large = calloc(1, HDA_DMA_BYTES + 4); assert(large);
    assert(hda_write(&d, large, HDA_DMA_BYTES + 4) == HDA_DMA_BYTES);
    assert(h->closing && h->generation == 1 && d.hw->busy_stop == 1 && !d.hw->frees);
    free(large); assert(!cleanup(&d) && d.hw->frees == 3);
    d = fresh(0, 0); assert(!hda_probe(&d)); submit(&d); d.hw->stuck_run = 1;
    assert(cleanup(&d) == -ETIMEDOUT && d.drv_priv && !d.hw->frees);
    d.hw->stuck_run = 0; assert(!cleanup(&d));
    for (unsigned fail = 0; fail < 10; fail++) {
        d = fresh(0, fail == 9);
        d.hw->fail_alloc = fail < 3 ? fail + 1 : 0;
        d.hw->fail_action = fail == 3; d.hw->fail_map = fail == 4; d.hw->fail_mask = fail == 5;
        d.hw->fail_codec = fail == 6; d.hw->bad_gcap = fail == 7; d.hw->dead_on_enable = fail == 8; d.hw->bad_dma = fail == 9;
        assert(hda_probe(&d) < 0);
        if (fail == 8) assert(d.drv_priv && cleanup(&d) == -ENODEV);
        d.hw->dead = d.hw->dead_on_enable = 0;
        assert(!cleanup(&d) && !d.drv_priv);
    }
    for (unsigned i = 0; i < 8; i++) assert(!allocations[i].p);
    puts("PASS: HDA controller/stream ACK, retained binding, canceled writer, closed PCM admission and partial probe");
    return 0;
}
