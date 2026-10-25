/* SPDX-License-Identifier: Apache-2.0
 * A64 codec-I2S/AIF1 and V3s DAC: 48 kHz stereo S16_LE playback.
 * Register facts cross-checked with Linux v6.17 sun4i-i2s.c,
 * sun8i-codec.c, sun4i-codec.c, sun{8,50}i-codec-analog.c and sun6i-dma.c.
 * No Linux implementation is included. Capture, mixing and clock switching
 * are deliberately not advertised. The architecture owns supply/clock policy.
 */
#include "drivers/audio/audio_core.h"
#include "drivers/audio/sunxi_audio.h"
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
#include "sunxi_pcm_ring.h"

#define DMA_CHANNEL 0x100U
#define DMA_EN (DMA_CHANNEL + 0x00)
#define DMA_LLI (DMA_CHANNEL + 0x08)
#define DMA_SRC (DMA_CHANNEL + 0x10)
#define DMA_IRQ_PACKAGE BIT(1)
#define DMA_IRQ_MASK 7U
#define SUNXI_DMA_CFG (1U | BIT(9) | (15U << 16) | BIT(21) | BIT(25))
/* Single-beat 16-bit SDRAM source, fixed 16-bit codec destination, DRQ 15.
 * Single beat is common to A64 and V3s: no SoC-specific burst encoding. */
struct sunxi_lli { uint32_t config, source, destination, length, wait, next; };
_Static_assert(sizeof(struct sunxi_lli) == 24, "sun6i hardware descriptor");
struct sunxi_audio {
    const sunxi_audio_platform_t *platform;
    volatile uint8_t *dai, *digital, *analog, *dma;
    uint64_t fifo_pa, data_pa, lli_pa;
    uint8_t *data;
    struct sunxi_lli *lli;
    spinlock_t lock;
    mutex_t writer;
    sunxi_pcm_ring_t ring;
    size_t partial;
    int irq_live, powered, quarantined, dma_owned, seen_period;
    uint64_t last_completion;
};
static uint32_t reg_read(volatile uint8_t *base, unsigned reg) {
    return readl(base + reg);
}
static void reg_write(volatile uint8_t *base, unsigned reg, uint32_t value) {
    writel(value, base + reg);
}
static void reg_update(volatile uint8_t *base, unsigned reg, uint32_t mask, uint32_t value) {
    reg_write(base, reg, (reg_read(base, reg) & ~mask) | value);
}
/* PRCM analog registers use an indexed 5-bit address / 8-bit data port.
 * Driver lock serializes every access; no read/write strobe is left active. */
static uint8_t analog_read(struct sunxi_audio *s, unsigned reg) {
    uint32_t value = reg_read(s->analog, 0);
    value = (value | BIT(28)) & ~(BIT(24) | (31U << 16));
    reg_write(s->analog, 0, value | (reg << 16));
    return reg_read(s->analog, 0) & 255;
}
static void analog_write(struct sunxi_audio *s, unsigned reg, uint8_t value) {
    uint32_t old = reg_read(s->analog, 0);
    old = (old | BIT(28)) & ~(BIT(24) | (31U << 16) | (255U << 8));
    uint32_t data = old | (reg << 16) | ((uint32_t)value << 8);
    reg_write(s->analog, 0, data);
    reg_write(s->analog, 0, data | BIT(24));
    reg_write(s->analog, 0, data);
}
static void analog_update(struct sunxi_audio *s, unsigned reg, uint8_t mask, uint8_t value) {
    analog_write(s, reg, (analog_read(s, reg) & ~mask) | value);
}
static void transmitter(struct sunxi_audio *s, int run) {
    if (s->platform->variant == SUNXI_AUDIO_A64) {
        reg_update(s->dai, 0x1c, BIT(7), run ? BIT(7) : 0);
        reg_update(s->dai, 0x00, BIT(2), run ? BIT(2) : 0);
    } else {
        reg_update(s->dai, 0x04, BIT(4), run ? BIT(4) : 0);
        reg_update(s->dai, 0x00, BIT(31), run ? BIT(31) : 0);
    }
}
static void mute(struct sunxi_audio *s, int muted) {
    unsigned reg = s->platform->variant == SUNXI_AUDIO_A64 ? 0x0a : 0x03;
    analog_update(s, reg, BIT(3) | BIT(2), muted ? 0 : BIT(3) | BIT(2));
}
static int dma_stop(struct sunxi_audio *s) {
    reg_write(s->dma, DMA_EN, 0);
    for (unsigned i = 0; i < 1000; i++) {
        if (!(reg_read(s->dma, DMA_EN) & 1)) return 0;
        udelay(10);
    }
    s->quarantined = 1;
    return -ETIMEDOUT;
}
static uint32_t fifo_faults(struct sunxi_audio *s) {
    unsigned reg = s->platform->variant == SUNXI_AUDIO_A64 ? 0x0c : 0x08;
    uint32_t mask = s->platform->variant == SUNXI_AUDIO_A64 ? BIT(6) | BIT(5) : BIT(2) | BIT(1);
    uint32_t errors = reg_read(s->dai, reg) & mask;
    if (errors) reg_write(s->dai, reg, errors);
    return errors;
}
static unsigned fifo_space(struct sunxi_audio *s) {
    return s->platform->variant == SUNXI_AUDIO_A64 ?
        (reg_read(s->dai, 0x18) >> 16) & 255U :
        (reg_read(s->dai, 0x08) >> 8) & 0x7fffU;
}
static void fifo_reset(struct sunxi_audio *s) {
    if (s->platform->variant == SUNXI_AUDIO_A64) {
        reg_update(s->dai, 0x14, BIT(25), BIT(25));
        reg_write(s->dai, 0x28, 0);
        reg_write(s->dai, 0x0c, BIT(6) | BIT(5));
    } else {
        reg_update(s->dai, 0x04, BIT(0), BIT(0));
        reg_write(s->dai, 0x08, 0xe); /* documented W1C FIFO IRQ flags */
    }
}
static void dma_start_locked(struct sunxi_audio *s) {
    unsigned slot = s->ring.consumer;
    reg_write(s->dma, 0x10, DMA_IRQ_MASK);
    reg_write(s->dma, DMA_LLI, (uint32_t)(s->lli_pa + slot * sizeof(*s->lli)));
    reg_write(s->dma, DMA_CHANNEL + 0x04, 0);
    s->ring.running = 1;
    s->seen_period = 0;
    s->last_completion = clock_get_ticks();
    reg_write(s->dma, DMA_EN, 1);
    transmitter(s, 1);
}
static int sunxi_irq(int irq, void *opaque) {
    (void)irq;
    struct sunxi_audio *s = opaque;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    uint32_t pending = reg_read(s->dma, 0x10) & DMA_IRQ_MASK;
    if (!pending) {
        spin_unlock_irqrestore(&s->lock, flags);
        return 0;
    }
    reg_write(s->dma, 0x10, pending);
    if (s->ring.running && (pending & DMA_IRQ_PACKAGE)) {
        uint32_t source = reg_read(s->dma, DMA_SRC);
        unsigned expected = (s->ring.consumer + 1) % SUNXI_PCM_PERIODS;
        uint64_t delta = (uint64_t)source - s->data_pa;
        int current_valid = source >= s->data_pa && delta <= SUNXI_PCM_BYTES;
        unsigned current = (unsigned)(delta / SUNXI_PCM_PERIOD_BYTES) % SUNXI_PCM_PERIODS;
        /* Package flags are sticky, not counters. A delayed IRQ which missed
         * a complete period must not release/refill the wrong buffer. */
        uint64_t now = clock_get_ticks();
        uint64_t period_ticks = clock_ticks_per_sec() * SUNXI_PCM_PERIOD_BYTES / 192000U;
        int timely = period_ticks && now - s->last_completion < period_ticks * 2;
        s->last_completion = now;
        int boundary = current == s->ring.consumer && reg_read(s->dma, DMA_CHANNEL + 0x18) == 0;
        uint32_t fifo_errors = fifo_faults(s);
        /* The serializer may emit initial zeros before its first DMA fill.
         * Clear startup status once; any later FIFO fault is a real XRUN. */
        int fifo_ok = !s->seen_period || !fifo_errors;
        s->seen_period = 1;
        int result = timely && fifo_ok && current_valid && (current == expected || boundary) ? sunxi_pcm_complete(&s->ring) : -1;
        unsigned previous = (s->ring.consumer + SUNXI_PCM_PERIODS - 1) % SUNXI_PCM_PERIODS;
        if (result >= 0) {
            memset(s->data + previous * SUNXI_PCM_PERIOD_BYTES, 0, SUNXI_PCM_PERIOD_BYTES);
            dma_sync_for_device(s->data + previous * SUNXI_PCM_PERIOD_BYTES, SUNXI_PCM_PERIOD_BYTES);
        }
        if (result <= 0) {
            /* Stop at the end of valid content. DMA may fetch zeros during
             * interrupt latency; stale samples are never deliberately replayed. */
            reg_write(s->dma, DMA_EN, 0);
            if (s->platform->variant == SUNXI_AUDIO_A64)
                reg_update(s->dai, 0x1c, BIT(7), 0);
            else reg_update(s->dai, 0x04, BIT(4), 0);
            s->ring.running = 0;
            if (result < 0) {
                s->ring.error = -EPIPE;
                transmitter(s, 0);
                mute(s, 1);
            }
        }
    }
    spin_unlock_irqrestore(&s->lock, flags);
    return 1;
}
static int configure_codec(struct sunxi_audio *s) {
    transmitter(s, 0);
    mute(s, 1);
    if (s->platform->variant == SUNXI_AUDIO_A64) {
        /* CPU DAI: I2S, master, two 16-bit slots, TX FIFO low-halfword.
         * PLL 24.576MHz /4 MCLK=6.144MHz /4 BCLK=1.536MHz, LRCK=48k. */
        reg_write(s->dai, 0x00, BIT(8) | BIT(0));
        reg_write(s->dai, 0x04, 0);
        reg_write(s->dai, 0x08, 0);
        reg_write(s->dai, 0x14, 0x000400f0U | BIT(2));
        reg_write(s->dai, 0x24, BIT(7) | (1U << 4) | 2U);
        reg_write(s->dai, 0x30, 1);
        reg_write(s->dai, 0x34, 0x76543210);
        /* AIF1 slave, 16-bit word, BCLK/LRCK=32, Fs encoding 8=48k.
         * AIF1 receive slots 0/1 -> left/right DAC mixers, 0dB digital gain. */
        reg_write(s->digital, 0x00c, BIT(11) | (2U << 8) | BIT(3));
        reg_write(s->digital, 0x010, BIT(15) | BIT(2));
        reg_write(s->digital, 0x014, BIT(15) | BIT(2));
        reg_write(s->digital, 0x018, 8U << 12);
        reg_write(s->digital, 0x040, BIT(15) | (6U << 9) | (1U << 6) | BIT(4));
        reg_write(s->digital, 0x048, BIT(15) | BIT(14));
        reg_write(s->digital, 0x058, 0xa0a0);
        reg_write(s->digital, 0x124, 0xa0a0);
        reg_write(s->digital, 0x130, BIT(15) | BIT(11));
        reg_write(s->digital, 0x120, BIT(15));
        analog_update(s, 0x0a, 0xff, BIT(7) | BIT(6));
        analog_update(s, 0x00, 0xff, BIT(7) | BIT(6) | 0x20);
    } else {
        /* V3s native DAC FIFO: Fs=0 is48k, FIR32, S16, stereo,
         * transmit zeros on starvation rather than holding the last sample. */
        reg_write(s->dai, 0x04, BIT(28) | BIT(24) | (15U << 8));
        analog_update(s, 0x03, 0xff, BIT(7) | BIT(6));
        analog_update(s, 0x00, 0xff, BIT(7) | 0x20);
        analog_update(s, 0x07, 0xf0, BIT(7) | BIT(6) | BIT(5) | BIT(4));
    }
    fifo_reset(s);
    /* V3s headphone amp bias settling from the documented upstream event;
     * mute is held for the entire interval, never inside an IRQ or spinlock. */
    mdelay(s->platform->variant == SUNXI_AUDIO_V3S ? 700 : 20);
    return 0;
}
static int reset_stream(struct sunxi_audio *s) {
    transmitter(s, 0);
    mute(s, 1);
    int result = dma_stop(s);
    if (result < 0) return result;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    memset(&s->ring, 0, sizeof(s->ring));
    s->partial = 0;
    memset(s->data, 0, SUNXI_PCM_BYTES);
    dma_sync_for_device(s->data, SUNXI_PCM_BYTES);
    fifo_reset(s);
    spin_unlock_irqrestore(&s->lock, flags);
    return 0;
}
static int audio_write(device_t *dev, const void *buffer, size_t count) {
    struct sunxi_audio *s = dev->drv_priv;
    if (!s || (count && !buffer)) return -EINVAL;
    if (!count) return 0;
    if (count % 4) return -EINVAL;
    if (count > 0x7ffffffcU) return -EOVERFLOW;
    mutex_lock(&s->writer);
    if (s->quarantined) { mutex_unlock(&s->writer); return -EIO; }
    if (s->ring.error) { int error = s->ring.error; mutex_unlock(&s->writer); return error; }
    size_t done = 0;
    unsigned waiting = 0;
    while (done < count) {
        uint64_t flags = spin_lock_irqsave(&s->lock);
        int error = s->ring.error;
        if (error) { spin_unlock_irqrestore(&s->lock, flags); break; }
        if (s->ring.count == SUNXI_PCM_PERIODS) {
            spin_unlock_irqrestore(&s->lock, flags);
            if (++waiting >= 1000) {
                transmitter(s, 0);
                dma_stop(s);
                s->ring.error = -ETIMEDOUT;
                break;
            }
            mdelay(1);
            continue;
        }
        waiting = 0;
        size_t n = SUNXI_PCM_PERIOD_BYTES - s->partial;
        if (n > count - done) n = count - done;
        uint8_t *target = s->data + s->ring.producer * SUNXI_PCM_PERIOD_BYTES;
        memcpy(target + s->partial, (const uint8_t *)buffer + done, n);
        s->partial += n;
        done += n;
        if (s->partial == SUNXI_PCM_PERIOD_BYTES) {
            dma_sync_for_device(target, SUNXI_PCM_PERIOD_BYTES);
            if (sunxi_pcm_publish(&s->ring) < 0) s->ring.error = -EIO;
            s->partial = 0;
            if (!s->ring.running && s->ring.count >= 2) {
                mute(s, 0);
                dma_start_locked(s);
            }
        }
        spin_unlock_irqrestore(&s->lock, flags);
    }
    int result = done ? (int)done : s->ring.error;
    mutex_unlock(&s->writer);
    return result;
}
static int audio_stop(device_t *dev) {
    struct sunxi_audio *s = dev->drv_priv;
    if (!s) return -ENODEV;
    mutex_lock(&s->writer);
    int result = reset_stream(s);
    mutex_unlock(&s->writer);
    return result;
}
static int audio_drain(device_t *dev) {
    struct sunxi_audio *s = dev->drv_priv;
    if (!s) return -ENODEV;
    mutex_lock(&s->writer);
    uint64_t flags = spin_lock_irqsave(&s->lock);
    s->ring.draining = 1;
    if (s->partial && !s->ring.error) {
        uint8_t *target = s->data + s->ring.producer * SUNXI_PCM_PERIOD_BYTES;
        memset(target + s->partial, 0, SUNXI_PCM_PERIOD_BYTES - s->partial);
        dma_sync_for_device(target, SUNXI_PCM_PERIOD_BYTES);
        if (sunxi_pcm_publish(&s->ring) < 0) s->ring.error = -EIO;
        s->partial = 0;
    }
    if (!s->ring.running && s->ring.count && !s->ring.error) { mute(s, 0); dma_start_locked(s); }
    spin_unlock_irqrestore(&s->lock, flags);
    unsigned ms;
    for (ms = 0; ms < 1500 && s->ring.running && !s->ring.error; ms++) mdelay(1);
    int result = s->ring.error;
    if (ms == 1500) result = -ETIMEDOUT;
    int stopped = dma_stop(s);
    if (stopped < 0) result = stopped;
    /* Manuals: A64 DA_FSTA[23:16] and V3s AC_DAC_FIFOS[22:8]
     * report128 free sample words when empty. Drain the actual FIFO, then
     * allow the final stereo frame to leave the serializer (42us at48k). */
    if (!result) {
        unsigned wait;
        for (wait = 0; wait < 2000 && fifo_space(s) != 128; wait++) udelay(10);
        if (wait == 2000) result = -ETIMEDOUT;
        else udelay(50);
    }
    transmitter(s, 0);
    mute(s, 1);
    if (!result) result = reset_stream(s);
    mutex_unlock(&s->writer);
    return result;
}
static int audio_format(device_t *dev, const a20_audio_format_t *format) {
    if (!format || format->rate != 48000 || format->channels != 2 || format->format != A20_AUDIO_FORMAT_S16_LE) return -EINVAL;
    return audio_stop(dev);
}
static int audio_poll(device_t *dev, short events) {
    struct sunxi_audio *s = dev->drv_priv;
    if (!s) return POLLERR;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    int result = s->ring.error || s->quarantined ? POLLERR :
        (s->ring.count < SUNXI_PCM_PERIODS ? events & POLLOUT : 0);
    spin_unlock_irqrestore(&s->lock, flags);
    return result;
}
static void destroy(struct sunxi_audio *s) {
    if (!s) return;
    if (s->dma && s->dma_owned) {
        reg_update(s->dma, 0x00, DMA_IRQ_MASK, 0);
        if (s->data && dma_stop(s) < 0)
            printf("[SUNXI-AUDIO] DMA failed to stop; quarantining coherent buffers\n");
    }
    if (s->irq_live) free_irq(s->platform->dma_irq, s);
    if (!s->quarantined) {
        if (s->lli) dma_free_coherent_aligned(s->lli, 4096, s->lli_pa);
        if (s->data) dma_free_coherent_aligned(s->data, SUNXI_PCM_BYTES, s->data_pa);
    }
    if (s->powered) {
        mute(s, 1);
        if (s->platform->variant == SUNXI_AUDIO_A64) {
            analog_update(s, 0x00, BIT(7) | BIT(6), 0);
            analog_update(s, 0x0a, BIT(7) | BIT(6), 0);
            if (s->digital) reg_write(s->digital, 0x120, 0);
        } else {
            analog_update(s, 0x07, BIT(7), 0);
            analog_update(s, 0x03, BIT(7) | BIT(6), 0);
        }
        s->platform->power(0);
    }
    if (s->dai) arch_device_unmap_mmio((void *)s->dai, 0x200);
    if (s->digital) arch_device_unmap_mmio((void *)s->digital, 0x600);
    if (s->analog) arch_device_unmap_mmio((void *)s->analog, 4);
    if (s->dma) arch_device_unmap_mmio((void *)s->dma, 0x1000);
    kfree(s);
}
static int audio_probe(device_t *dev) {
    const sunxi_audio_platform_t *p = dev->plat_data;
    if (!p || p->magic != SUNXI_AUDIO_PLATFORM_MAGIC || !p->prepare || !p->power ||
        (p->variant != SUNXI_AUDIO_A64 && p->variant != SUNXI_AUDIO_V3S)) return -ENODEV;
    resource_t *resource = device_get_resource(dev, RES_MMIO, 0);
    if (!resource || resource->end < resource->start || resource->end - resource->start < 0x1ff || !p->analog_pa || !p->dma_pa || !p->ccu_pa || !p->dma_irq ||
        (p->variant == SUNXI_AUDIO_A64 && (!p->digital_pa || !p->rsb_pa))) return -EINVAL;
    /* Core clears links before each probe attempt. Reconstruct the complete
     * topology here, including resources used indirectly by arch callbacks. */
    uint64_t providers[] = {p->dma_pa, p->analog_pa, p->digital_pa, p->ccu_pa, p->rsb_pa};
    for (unsigned i = 0; i < sizeof(providers) / sizeof(providers[0]); i++) {
        if (!providers[i]) continue;
        device_t *provider = device_find_get_by_mmio(providers[i]);
        if (!provider) return -EPROBE_DEFER;
        int linked = device_add_dependency(dev, provider);
        device_put(provider);
        if (linked < 0) return linked;
    }
    struct sunxi_audio *s = kcalloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    s->platform = p;
    spin_init(&s->lock);
    mutex_init(&s->writer);
    int result = p->prepare();
    if (result < 0) { kfree(s); return result; }
    s->dai = arch_device_map_mmio(resource->start, 0x200, 0);
    s->analog = arch_device_map_mmio(p->analog_pa, 4, 0);
    s->dma = arch_device_map_mmio(p->dma_pa, 0x1000, 0);
    if (p->variant == SUNXI_AUDIO_A64) s->digital = arch_device_map_mmio(p->digital_pa, 0x600, 0);
    if (!s->dai || !s->analog || !s->dma || (p->variant == SUNXI_AUDIO_A64 && !s->digital)) { result = -ENOMEM; goto fail; }
    /* Channel0 is exclusively reserved by this early board topology. Never
     * reset another bus master or take over an already enabled DMA channel. */
    if ((reg_read(s->dma, DMA_EN) & 1) || (reg_read(s->dma, 0x00) & DMA_IRQ_MASK)) { result = -EBUSY; goto fail; }
    s->dma_owned = 1;
    if (dma_set_mask(dev, 0xffffffffULL) < 0) { result = -EIO; goto fail; }
    s->data = dma_alloc_coherent_aligned(dev, SUNXI_PCM_BYTES, 4096, &s->data_pa);
    s->lli = dma_alloc_coherent_aligned(dev, 4096, 4096, &s->lli_pa);
    if (!s->data || !s->lli) { result = -ENOMEM; goto fail; }
    s->fifo_pa = resource->start + 0x20;
    if (s->fifo_pa > 0xffffffffULL || s->data_pa + SUNXI_PCM_BYTES > 0x100000000ULL || s->lli_pa + 4096 > 0x100000000ULL) { result = -EOVERFLOW; goto fail; }
    memset(s->data, 0, SUNXI_PCM_BYTES);
    for (unsigned i = 0; i < SUNXI_PCM_PERIODS; i++) {
        s->lli[i] = (struct sunxi_lli){SUNXI_DMA_CFG, (uint32_t)(s->data_pa + i * SUNXI_PCM_PERIOD_BYTES),
            (uint32_t)s->fifo_pa, SUNXI_PCM_PERIOD_BYTES, 8,
            (uint32_t)(s->lli_pa + ((i + 1) % SUNXI_PCM_PERIODS) * sizeof(*s->lli))};
    }
    dma_sync_for_device(s->data, SUNXI_PCM_BYTES);
    dma_sync_for_device(s->lli, 4096);
    s->powered = 1;
    result = p->power(1);
    if (result < 0) goto fail;
    result = configure_codec(s);
    if (result < 0) goto fail;
    result = request_irq(p->dma_irq, sunxi_irq, IRQF_SHARED, s);
    if (result < 0) goto fail;
    s->irq_live = 1;
    reg_update(s->dma, p->variant == SUNXI_AUDIO_A64 ? 0x28 : 0x20, BIT(2), BIT(2)); /* Variant-specific DMA MCLK autogate */
    reg_write(s->dma, 0x10, DMA_IRQ_MASK);
    reg_update(s->dma, 0x00, DMA_IRQ_MASK, DMA_IRQ_PACKAGE);
    irq_enable(p->dma_irq);
    dev->drv_priv = s;
    return 0;
fail:
    destroy(s);
    return result;
}
static int audio_remove(device_t *dev) {
    struct sunxi_audio *s = dev->drv_priv;
    if (s) { transmitter(s, 0); mute(s, 1); }
    dev->drv_priv = NULL;
    destroy(s);
    return 0;
}
static const audio_dev_ops_t sunxi_audio_ops = {
    .caps = {1, A20_AUDIO_CAP_PCM, 48000, 48000},
    .pcm_format = {48000, 2, A20_AUDIO_FORMAT_S16_LE},
    .write = audio_write, .set_format = audio_format, .stop = audio_stop,
    .drain = audio_drain, .poll = audio_poll, .close = audio_stop,
};
static const firmware_device_id_t sunxi_audio_ids[] = {
    {.id = "allwinner,sun50i-a64-codec-i2s"},
    {.id = "allwinner,sun8i-v3s-codec"}, {0},
};
static driver_t sunxi_audio_driver = {
    .name = "sunxi-codec", .bus = &platform_bus, .firmware_ids = sunxi_audio_ids,
    .probe = audio_probe, .remove = audio_remove, .class_type = DEV_CLASS_AUDIO,
    .class_ops = &sunxi_audio_ops, .irq_optional = 1,
};
DRIVER_REGISTER(sunxi_audio_driver);
