/* SPDX-License-Identifier: Apache-2.0
 * RK356x I2S/TDM -> RK817 headphones, fixed48kHz stereo S16_LE.
 * Original protocol implementation, register facts cross-checked with RK817
 * datasheet1.01 and Linux6.17 rk817_codec/rockchip_i2s_tdm/pl330 drivers.
 * Every PCM sample is expanded into the low16 bits of a32-bit I2S FIFO word.
 * The supplied PL330 controller/debug-manager is exclusive to this topology;
 * any active channel/event or bound controller provider is refused. No physical board addresses occur here.
 */
#include "drivers/audio/audio_core.h"
#include "drivers/audio/rk817_audio.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/poll.h"
#include "mm/slab.h"
#include "pcm_period_ring.h"
#include "../dma/pl330_audio_program.h"
#define DATA_BYTES (PL330_AUDIO_PERIODS * PL330_AUDIO_PERIOD_BYTES)
_Static_assert(PCM_PERIODS == PL330_AUDIO_PERIODS &&
                   PCM_PERIOD_BYTES * 2 == PL330_AUDIO_PERIOD_BYTES,
               "PCM period and FIFO expansion must agree");
struct rk817_audio {
    const rk817_audio_platform_t *platform;
    volatile uint8_t *i2s, *dma;
    uint32_t *data;
    uint8_t *program;
    uint64_t data_pa, program_pa, last_completion;
    spinlock_t lock;
    mutex_t writer;
    pcm_period_ring_t ring;
    size_t partial;
    unsigned irq_live;
    int owned, quarantined, seen_period, codec_touched;
    uint8_t saved[0x50], saved_valid[0x50];
};
static uint32_t rk_read(volatile uint8_t *p, unsigned r) {
    return readl(p + r);
}
static void rk_write(volatile uint8_t *p, unsigned r, uint32_t v) {
    writel(v, p + r);
}
static void rk_update(volatile uint8_t *p, unsigned r, uint32_t mask,
                      uint32_t v) {
    rk_write(p, r, (rk_read(p, r) & ~mask) | v);
}
static int codec_set(struct rk817_audio *s, unsigned reg, uint8_t mask,
                     uint8_t value) {
    uint8_t old = 0;
    int r = s->platform->codec_register(reg, &old, 0);
    if (r < 0)
        return r;
    if (!s->saved_valid[reg]) {
        s->saved[reg] = old;
        s->saved_valid[reg] = 1;
    }
    uint8_t next = (old & ~mask) | value;
    s->codec_touched = 1;
    r = s->platform->codec_register(reg, &next, 1);
    if (r < 0)
        return r;
    uint8_t check = 0;
    r = s->platform->codec_register(reg, &check, 0);
    return r < 0 ? r : ((check & mask) == (next & mask) ? 0 : -EIO);
}
static int codec_mute(struct rk817_audio *s, int mute) {
    return codec_set(s, 0x38, 1, mute ? 1 : 0);
}
static int codec_init(struct rk817_audio *s) {
    uint8_t active = 0;
    int available = s->platform->codec_register(0x15, &active, 0);
    if (available < 0)
        return available;
    if (active)
        return -EBUSY;
    /* Keep headphones digitally muted through bias/charge-pump settling.
     * PLL 12.288MHz *360/12 /6 /5 =12.288MHz. No PMIC regulator register is
     * touched here; LDO4 IO-domain policy belongs to the board callback. */
    static const uint8_t setup[][3] = {
        {0x38, 0x11, 1},    {0x15, 0x0f, 0},    {0x47, 1, 1},
        {0x30, 0x02, 0x02}, {0x35, 0x07, 2},    {0x13, 0xff, 0xf4},
        {0x43, 0xff, 0x58}, {0x44, 0xff, 0x2d}, {0x45, 0xff, 0x0c},
        {0x46, 0xff, 0xa5}, {0x42, 0x0c, 4},    {0x48, 0x0f, 0},
        {0x49, 0x0f, 0},    {0x4a, 0xff, 0},    {0x4b, 0x1f, 15},
        {0x31, 0xff, 16},   {0x32, 0xff, 16},   {0x17, 0x46, 0x40},
        {0x47, 1, 0},       {0x2f, 0x0b, 0},    {0x3f, 0x18, 0x10},
        {0x3d, 0x60, 0},    {0x15, 0x0f, 0x0f}, {0x4c, 0x20, 0x20}};
    for (unsigned i = 0; i < sizeof(setup) / sizeof(setup[0]); i++) {
        int r = codec_set(s, setup[i][0], setup[i][1], setup[i][2]);
        if (r < 0)
            return r;
    }
    mdelay(20);
    return 0;
}
static void transmitter(struct rk817_audio *s, int run) {
    rk_update(s->i2s, 0x10, 1U << 8, run ? 1U << 8 : 0);
    rk_update(s->i2s, 0x1c, 1, run ? 1 : 0);
}
/* Caller holds the controller/ring lock across every debug transaction. */
static int dma_debug(struct rk817_audio *s, uint32_t instruction,
                     uint32_t argument) {
    for (unsigned i = 0; i < 1000; i++) {
        if (!(rk_read(s->dma, 0xd00) & 1)) {
            rk_write(s->dma, 0xd08, instruction);
            rk_write(s->dma, 0xd0c, argument);
            rk_write(s->dma, 0xd04, 0);
            return 0;
        }
        udelay(10);
    }
    return -ETIMEDOUT;
}
static int dma_manager_stop(struct rk817_audio *s) {
    if (!(rk_read(s->dma, 0x30) & 1) && !(rk_read(s->dma, 0x00) & 15))
        return 0;
    int r = dma_debug(s, 1U << 16, 0); /* manager DMAKILL */
    if (!r)
        for (unsigned i = 0; i < 1000; i++) {
            if (!(rk_read(s->dma, 0x30) & 1) && !(rk_read(s->dma, 0x00) & 15))
                return 0;
            udelay(10);
        }
    s->quarantined = 1;
    return -ETIMEDOUT;
}
static void dma_quarantine_irqs(struct rk817_audio *s) {
    /* This board topology owns this whole PL330, including both IRQ lines.
     * An unkillable abort source must not trap the CPU in a level IRQ storm. */
    for (unsigned i = 0; i < s->irq_live; i++)
        irq_disable(s->platform->dma_irq[i]);
    s->quarantined = 1;
}
static int dma_stop(struct rk817_audio *s) {
    if ((rk_read(s->dma, 0x100) & 15) == 0)
        return 0;
    int r = dma_debug(s, (1U << 16) | 1, 0); /* channel0 DMAKILL */
    if (!r)
        for (unsigned i = 0; i < 1000; i++) {
            if ((rk_read(s->dma, 0x100) & 15) == 0)
                return 0;
            udelay(10);
        }
    s->quarantined = 1;
    return -ETIMEDOUT;
}
static int fifo_reset(struct rk817_audio *s) {
    rk_write(s->i2s, 0x20, 1);
    for (unsigned i = 0; i < 1000; i++) {
        if (!(rk_read(s->i2s, 0x20) & 1)) {
            rk_update(s->i2s, 0x14, 4, 4);
            return 0;
        }
        udelay(10);
    }
    return -ETIMEDOUT;
}
static int stream_reset(struct rk817_audio *s) {
    uint64_t f = spin_lock_irqsave(&s->lock);
    transmitter(s, 0);
    s->ring.running = 0;
    int manager = dma_manager_stop(s);
    int r = dma_stop(s);
    if (!r)
        r = manager;
    if (r < 0)
        dma_quarantine_irqs(s);
    spin_unlock_irqrestore(&s->lock, f);
    int muted = codec_mute(s, 1);
    if (r < 0)
        return r;
    if (muted < 0)
        return muted;
    f = spin_lock_irqsave(&s->lock);
    memset(&s->ring, 0, sizeof(s->ring));
    s->partial = 0;
    memset(s->data, 0, DATA_BYTES);
    dma_sync_for_device(s->data, DATA_BYTES);
    r = fifo_reset(s);
    spin_unlock_irqrestore(&s->lock, f);
    return r;
}
static int dma_start_locked(struct rk817_audio *s) {
    /* Program begins with period0. Reset always leaves producer/consumer0;
     * a stream cannot restart after XRUN until explicit STOP succeeds. */
    rk_write(s->dma, 0x2c, 1);
    s->seen_period = 0;
    s->last_completion = clock_get_ticks();
    int r = dma_debug(s, 0xa2U << 16,
                      (uint32_t)s->program_pa); /* manager DMAGO NS,ch0 */
    if (!r) {
        unsigned wait;
        for (wait = 0; wait < 1000; wait++) {
            if ((rk_read(s->dma, 0x30) & 1) || (rk_read(s->dma, 0x34) & 1)) {
                r = -EIO;
                break;
            }
            if (!(rk_read(s->dma, 0xd00) & 1) &&
                !(rk_read(s->dma, 0x00) & 15) && (rk_read(s->dma, 0x100) & 15))
                break;
            udelay(10);
        }
        if (wait == 1000)
            r = -ETIMEDOUT;
    }
    if (r < 0) {
        s->ring.error = r;
        if (dma_stop(s) < 0 || dma_manager_stop(s) < 0)
            dma_quarantine_irqs(s);
        return r;
    }
    s->ring.running = 1;
    transmitter(s, 1);
    return 0;
}
static int rk817_irq(int irq, void *opaque) {
    (void)irq;
    struct rk817_audio *s = opaque;
    uint64_t f = spin_lock_irqsave(&s->lock);
    uint32_t pending = rk_read(s->dma, 0x28) & 1;
    uint32_t fault = (rk_read(s->dma, 0x34) & 1) | (rk_read(s->dma, 0x30) & 1);
    if (!pending && !fault) {
        spin_unlock_irqrestore(&s->lock, f);
        return 0;
    }
    if (pending)
        rk_write(s->dma, 0x2c, 1);
    if (fault) {
        transmitter(s, 0);
        s->ring.running = 0;
        s->ring.error = -EIO;
        if (dma_stop(s) < 0 || dma_manager_stop(s) < 0)
            dma_quarantine_irqs(s);
        spin_unlock_irqrestore(&s->lock, f);
        return 1;
    }
    if (s->ring.running) {
        uint32_t source = rk_read(s->dma, 0x400);
        uint64_t delta = (uint64_t)source - s->data_pa;
        unsigned next = (s->ring.consumer + 1) % PCM_PERIODS;
        unsigned current =
            (unsigned)(delta / PL330_AUDIO_PERIOD_BYTES) % PCM_PERIODS;
        uint64_t now = clock_get_ticks();
        uint64_t period = clock_ticks_per_sec() * PCM_PERIOD_BYTES / 192000U;
        int valid = source >= s->data_pa && delta <= DATA_BYTES && period &&
                    now - s->last_completion < 2 * period;
        int boundary = source == s->data_pa + (s->ring.consumer + 1) *
                                                  PL330_AUDIO_PERIOD_BYTES;
        int fifo_ok = !s->seen_period || !(rk_read(s->i2s, 0x18) & 2);
        s->seen_period = 1;
        s->last_completion = now;
        rk_update(s->i2s, 0x14, 4, 4);
        int result = !fault && valid && fifo_ok && (current == next || boundary)
                         ? pcm_period_complete(&s->ring)
                         : -1;
        if (result >= 0) {
            unsigned prev = (s->ring.consumer + PCM_PERIODS - 1) % PCM_PERIODS;
            memset(s->data + prev * (PL330_AUDIO_PERIOD_BYTES / 4), 0,
                   PL330_AUDIO_PERIOD_BYTES);
            dma_sync_for_device(s->data + prev * (PL330_AUDIO_PERIOD_BYTES / 4),
                                PL330_AUDIO_PERIOD_BYTES);
        }
        if (result <= 0) {
            /* Stop peripheral requests immediately; process context performs
             * bounded DMAKILL and codec mute before reclaiming any memory. */
            rk_update(s->i2s, 0x10, 1U << 8, 0);
            s->ring.running = 0;
            if (result < 0) {
                s->ring.error = fault ? -EIO : -EPIPE;
                transmitter(s, 0);
            }
        }
    }
    spin_unlock_irqrestore(&s->lock, f);
    return 1;
}
static int rk817_write(device_t *dev, const void *buffer, size_t count) {
    struct rk817_audio *s = dev->drv_priv;
    if (!s || (count && !buffer) || count % 4)
        return -EINVAL;
    if (!count)
        return 0;
    if (count > 0x7ffffffc)
        return -EOVERFLOW;
    mutex_lock(&s->writer);
    if (s->quarantined) {
        mutex_unlock(&s->writer);
        return -EIO;
    }
    if (s->ring.error) {
        int r = s->ring.error;
        mutex_unlock(&s->writer);
        return r;
    }
    size_t done = 0;
    unsigned waiting = 0;
    while (done < count) {
        uint64_t f = spin_lock_irqsave(&s->lock);
        if (s->ring.error) {
            spin_unlock_irqrestore(&s->lock, f);
            break;
        }
        if (s->ring.count == PCM_PERIODS) {
            spin_unlock_irqrestore(&s->lock, f);
            if (++waiting == 1000) {
                f = spin_lock_irqsave(&s->lock);
                transmitter(s, 0);
                s->ring.running = 0;
                if (dma_stop(s) < 0)
                    dma_quarantine_irqs(s);
                s->ring.error = -ETIMEDOUT;
                spin_unlock_irqrestore(&s->lock, f);
                break;
            }
            mdelay(1);
            continue;
        }
        waiting = 0;
        size_t n = PCM_PERIOD_BYTES - s->partial;
        if (n > count - done)
            n = count - done;
        uint32_t *target =
            s->data + s->ring.producer * (PL330_AUDIO_PERIOD_BYTES / 4);
        const uint8_t *input = (const uint8_t *)buffer + done;
        for (size_t i = 0; i < n; i += 2)
            target[(s->partial + i) / 2] =
                (uint32_t)input[i] | ((uint32_t)input[i + 1] << 8);
        s->partial += n;
        done += n;
        int start = 0;
        if (s->partial == PCM_PERIOD_BYTES) {
            dma_sync_for_device(target, PL330_AUDIO_PERIOD_BYTES);
            if (pcm_period_publish(&s->ring) < 0)
                s->ring.error = -EIO;
            s->partial = 0;
            start = !s->ring.running && s->ring.count >= 2;
        }
        spin_unlock_irqrestore(&s->lock, f);
        if (start) {
            int r = codec_mute(s, 0);
            f = spin_lock_irqsave(&s->lock);
            if (r < 0)
                s->ring.error = r;
            else
                dma_start_locked(s);
            spin_unlock_irqrestore(&s->lock, f);
        }
    }
    int r = done ? (int)done : s->ring.error;
    mutex_unlock(&s->writer);
    return r;
}
static int rk817_stop(device_t *d) {
    struct rk817_audio *s = d->drv_priv;
    if (!s)
        return -ENODEV;
    mutex_lock(&s->writer);
    int r = stream_reset(s);
    mutex_unlock(&s->writer);
    return r;
}
static int rk817_drain(device_t *d) {
    struct rk817_audio *s = d->drv_priv;
    if (!s)
        return -ENODEV;
    mutex_lock(&s->writer);
    uint64_t f = spin_lock_irqsave(&s->lock);
    s->ring.draining = 1;
    if (s->partial && !s->ring.error) {
        uint32_t *p =
            s->data + s->ring.producer * (PL330_AUDIO_PERIOD_BYTES / 4);
        memset(p + s->partial / 2, 0,
               PL330_AUDIO_PERIOD_BYTES - s->partial * 2);
        dma_sync_for_device(p, PL330_AUDIO_PERIOD_BYTES);
        if (pcm_period_publish(&s->ring) < 0)
            s->ring.error = -EIO;
        s->partial = 0;
    }
    int start = !s->ring.running && s->ring.count && !s->ring.error;
    spin_unlock_irqrestore(&s->lock, f);
    if (start) {
        int r = codec_mute(s, 0);
        f = spin_lock_irqsave(&s->lock);
        if (r < 0)
            s->ring.error = r;
        else
            dma_start_locked(s);
        spin_unlock_irqrestore(&s->lock, f);
    }
    unsigned ms;
    for (ms = 0; ms < 1500 && s->ring.running && !s->ring.error; ms++)
        mdelay(1);
    int r = s->ring.error;
    if (ms == 1500)
        r = -ETIMEDOUT;
    f = spin_lock_irqsave(&s->lock);
    s->ring.running = 0;
    int manager = dma_manager_stop(s);
    int stopped = dma_stop(s);
    if (!stopped)
        stopped = manager;
    if (stopped < 0) {
        r = stopped;
        dma_quarantine_irqs(s);
    }
    spin_unlock_irqrestore(&s->lock, f);
    if (!r) {
        unsigned i;
        for (i = 0; i < 2000 && (rk_read(s->i2s, 0x0c) & 63); i++)
            udelay(10);
        if (i == 2000)
            r = -ETIMEDOUT;
        else
            udelay(50);
    }
    transmitter(s, 0);
    int muted = codec_mute(s, 1);
    if (!r)
        r = muted;
    if (!r)
        r = stream_reset(s);
    mutex_unlock(&s->writer);
    return r;
}
static int rk817_format(device_t *d, const a20_audio_format_t *f) {
    if (!f || f->rate != 48000 || f->channels != 2 ||
        f->format != A20_AUDIO_FORMAT_S16_LE)
        return -EINVAL;
    return rk817_stop(d);
}
static int rk817_poll(device_t *d, short e) {
    struct rk817_audio *s = d->drv_priv;
    if (!s)
        return POLLERR;
    uint64_t f = spin_lock_irqsave(&s->lock);
    int r = s->ring.error || s->quarantined ? POLLERR
            : s->ring.count < PCM_PERIODS   ? e & POLLOUT
                                            : 0;
    spin_unlock_irqrestore(&s->lock, f);
    return r;
}
static void rk817_destroy(struct rk817_audio *s) {
    if (!s)
        return;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    if (s->i2s && s->owned)
        transmitter(s, 0);
    s->ring.running = 0;
    if (s->dma && s->owned) {
        rk_update(s->dma, 0x20, 1, 0);
        int manager = dma_manager_stop(s);
        int channel = dma_stop(s);
        if (manager < 0 || channel < 0) {
            dma_quarantine_irqs(s);
            printf("[RK817] DMA stop failed; buffers quarantined\n");
        }
    }
    spin_unlock_irqrestore(&s->lock, flags);
    for (unsigned i = 0; i < s->irq_live; i++)
        free_irq(s->platform->dma_irq[i], s);
    if (!s->quarantined) {
        if (s->program)
            dma_free_coherent_aligned(s->program, 4096, s->program_pa);
        if (s->data)
            dma_free_coherent_aligned(s->data, DATA_BYTES, s->data_pa);
    }
    if (s->codec_touched) {
        codec_mute(s, 1);
        /* Reverse the path before restoring unrelated pre-probe settings. */
        codec_set(s, 0x15, 15, 0);
        codec_set(s, 0x3d, 0x60, 0x60);
        codec_set(s, 0x3f, 0x18, 8);
        codec_set(s, 0x2f, 0x0b, 0x0b);
        codec_set(s, 0x47, 1, 1);
        int restore_error = 0;
        for (unsigned r = 0x50; r-- > 0;) {
            if (!s->saved_valid[r] || r == 0x38)
                continue;
            if (s->platform->codec_register(r, &s->saved[r], 1) < 0)
                restore_error = 1;
        }
        if (!restore_error && s->saved_valid[0x38]) {
            mdelay(20);
            if (s->platform->codec_register(0x38, &s->saved[0x38], 1) < 0)
                restore_error = 1;
        }
        if (restore_error) {
            codec_mute(s, 1);
            printf("[RK817] Codec restore failed; retaining digital mute\n");
        }
    }
    if (s->i2s)
        arch_device_unmap_mmio((void *)s->i2s, 0x1000);
    if (s->dma)
        arch_device_unmap_mmio((void *)s->dma, 0x4000);
    kfree(s);
}
static int rk817_probe(device_t *d) {
    const rk817_audio_platform_t *p = d->plat_data;
    if (!p || p->magic != RK817_AUDIO_MAGIC || !p->prepare ||
        !p->codec_register)
        return -ENODEV;
    resource_t *r = device_get_resource(d, RES_MMIO, 0);
    if (!r || r->end < r->start || r->end - r->start < 0xfff || !p->dma_pa ||
        !p->dma_irq[0] || !p->dma_irq[1] || p->dma_irq[0] == p->dma_irq[1] ||
        p->dma_request >= 32)
        return -EINVAL;
    uint64_t pas[6] = {p->dma_pa,         p->provider_pa[0], p->provider_pa[1],
                       p->provider_pa[2], p->provider_pa[3], p->provider_pa[4]};
    for (unsigned i = 0; i < 6; i++) {
        if (!pas[i])
            return -EINVAL;
        device_t *provider = device_find_get_by_mmio(pas[i]);
        if (!provider)
            return -EPROBE_DEFER;
        int e = device_add_dependency(d, provider);
        device_put(provider);
        if (e < 0)
            return e;
    }
    struct rk817_audio *s = kcalloc(1, sizeof(*s));
    if (!s)
        return -ENOMEM;
    s->platform = p;
    spin_init(&s->lock);
    mutex_init(&s->writer);
    int e = p->prepare();
    if (e < 0) {
        kfree(s);
        return e;
    }
    s->i2s = arch_device_map_mmio(r->start, 0x1000, 0);
    s->dma = arch_device_map_mmio(p->dma_pa, 0x4000, 0);
    if (!s->i2s || !s->dma) {
        e = -ENOMEM;
        goto fail;
    }
    if (rk_read(s->dma, 0x20) || (rk_read(s->i2s, 0x1c) & 3) ||
        (rk_read(s->dma, 0) & 15) || (rk_read(s->dma, 0xd00) & 1)) {
        e = -EBUSY;
        goto fail;
    }
    if (rk_read(s->dma, 0x30) & 1) {
        e = -EBUSY;
        goto fail;
    }
    uint32_t identity = 0;
    for (unsigned i = 0; i < 4; i++)
        identity |= (rk_read(s->dma, 0xfe0 + i * 4) & 255U) << (i * 8);
    if ((identity & 0xfffffU) != 0x41330U) {
        e = -ENODEV;
        goto fail;
    }
    uint32_t config = rk_read(s->dma, 0xe00);
    if (!(config & 1) || !(config & 4) ||
        ((config >> 12) & 31) < p->dma_request ||
        !(rk_read(s->dma, 0xe0c) & 1) ||
        !(rk_read(s->dma, 0xe10) & (1U << p->dma_request))) {
        e = -EACCES;
        goto fail;
    }
    unsigned channels = ((config >> 4) & 7) + 1;
    for (unsigned i = 0; i < channels; i++) {
        if ((rk_read(s->dma, 0x100 + i * 8) & 15) || rk_read(s->dma, 0x34)) {
            e = -EBUSY;
            goto fail;
        }
    }
    s->owned = 1;
    if (dma_set_mask(d, 0xffffffffULL) < 0) {
        e = -EIO;
        goto fail;
    }
    s->data = dma_alloc_coherent_aligned(d, DATA_BYTES, 4096, &s->data_pa);
    s->program = dma_alloc_coherent_aligned(d, 4096, 4096, &s->program_pa);
    if (!s->data || !s->program) {
        e = -ENOMEM;
        goto fail;
    }
    if (s->program_pa > 0xfffff000ULL ||
        !pl330_audio_program(s->program, 4096, s->data_pa, r->start + 0x24,
                             p->dma_request, 0)) {
        e = -EOVERFLOW;
        goto fail;
    }
    memset(s->data, 0, DATA_BYTES);
    dma_sync_for_device(s->data, DATA_BYTES);
    dma_sync_for_device(s->program, 4096);
    e = codec_init(s);
    if (e < 0)
        goto fail;
    rk_write(s->i2s, 0, 15); /* TXCR stereo, I2S16 */
    rk_write(s->i2s, 0x08,
             (1U << 28) | (63U << 8) |
                 63U);                      /* CKR TX/RX64 BCLK/frame, master */
    rk_write(s->i2s, 0x38, 3U | (3U << 8)); /* 12.288MHz/4=3.072MHz BCLK */
    rk_write(s->i2s, 0x30, 0);
    rk_write(s->i2s, 0x34, 0);  /* TDM disabled */
    rk_write(s->i2s, 0x10, 16); /* TX threshold16 words, DMA off */
    e = fifo_reset(s);
    if (e < 0)
        goto fail;
    for (unsigned i = 0; i < 2; i++) {
        e = request_irq(p->dma_irq[i], rk817_irq, IRQF_SHARED, s);
        if (e < 0)
            goto fail;
        s->irq_live++;
    }
    rk_write(s->dma, 0x2c, 1);
    rk_update(s->dma, 0x20, 1, 1);
    for (unsigned i = 0; i < 2; i++)
        irq_enable(p->dma_irq[i]);
    d->drv_priv = s;
    return 0;
fail:
    rk817_destroy(s);
    return e;
}
static int rk817_remove(device_t *d) {
    struct rk817_audio *s = d->drv_priv;
    d->drv_priv = NULL;
    rk817_destroy(s);
    return 0;
}
static const audio_dev_ops_t rk817_ops = {
    .caps = {1, A20_AUDIO_CAP_PCM, 48000, 48000},
    .pcm_format = {48000, 2, A20_AUDIO_FORMAT_S16_LE},
    .write = rk817_write,
    .set_format = rk817_format,
    .stop = rk817_stop,
    .drain = rk817_drain,
    .poll = rk817_poll,
    .close = rk817_stop};
static const firmware_device_id_t rk817_ids[] = {
    {.id = "rockchip,rk3568-i2s-tdm"}, {0}};
static driver_t rk817_driver = {.name = "rk817-pcm",
                                .bus = &platform_bus,
                                .firmware_ids = rk817_ids,
                                .probe = rk817_probe,
                                .remove = rk817_remove,
                                .class_type = DEV_CLASS_AUDIO,
                                .class_ops = &rk817_ops,
                                .irq_optional = 1};
DRIVER_REGISTER(rk817_driver);
