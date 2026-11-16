/* SPDX-License-Identifier: Apache-2.0
 * SG2002 I2S3 -> internal DAC -> Nano fixed-enable mono PA. Playback only.
 * No panel dependencies. Architecture owns resource/clock/cache authorization.
 */
#include "drivers/audio/audio_core.h"
#include "drivers/audio/sg2002_audio.h"
#include "drivers/core/driver_register.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "core/errno.h"
#include "firmware/device.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/poll.h"
#include "mm/slab.h"
#include "sg2002_audio_hw.h"
struct sg2002_audio {
    const sg2002_audio_platform_t *platform;
    volatile uint8_t *i2s, *dac, *dma;
    uint8_t *data;
    struct sg2002_lli *lli;
    uint64_t data_pa, lli_pa, last_completion;
    spinlock_t lock;
    mutex_t writer;
    pcm_period_ring_t ring;
    size_t partial;
    int irq_live, quarantined, dma_owned;
    int channel_owned; /* Binding claim survives an ordinary stream stop. */
    int closing;
};
static int execution_stop(void *context);

static uint32_t rd(volatile uint8_t *p, unsigned r) {
    return readl(p + r);
}
static void wr(volatile uint8_t *p, unsigned r, uint32_t v) {
    writel(v, p + r);
}
static uint64_t dr(struct sg2002_audio *s, unsigned r) {
    return readq(s->dma + r);
}
static void dw(struct sg2002_audio *s, unsigned r, uint64_t v) {
    writeq(v, s->dma + r);
}
static void update(volatile uint8_t *p, unsigned r, uint32_t mask,
                   uint32_t value) {
    wr(p, r, (rd(p, r) & ~mask) | value);
}
static void mute(struct sg2002_audio *s, int on) {
    update(s->dac, 0x28, 3U << 16, on ? 3U << 16 : 0);
}
static void idle_locked(struct sg2002_audio *s, int error) {
    mute(s, 1);
    wr(s->i2s, 0x18, 0);
    s->ring.running = 0;
    if (error)
        s->ring.error = error;
}
static int halt_dma(struct sg2002_audio *s) {
    /* Per-channel abort+WE, without disturbing other DMA channels. */
    if (dr(s, SG_DMA_ENABLE) == ~0ULL) {
        s->quarantined = 1;
        return -ENODEV;
    }
    dw(s, SG_CH_INT_SIGNAL, 0);
    dw(s, SG_CH_INT_EN, 0);
    if (dr(s, SG_CH_INT_SIGNAL) || dr(s, SG_CH_INT_EN)) {
        s->quarantined = 1;
        return -EIO;
    }
    dw(s, SG_DMA_ENABLE,
       (SG_DMA_MASK << 8) | (SG_DMA_MASK << 32) | (SG_DMA_MASK << 40));
    for (unsigned i = 0; i < 1000; i++) {
        uint64_t enabled = dr(s, SG_DMA_ENABLE);
        if (enabled == ~0ULL) { s->quarantined = 1; return -ENODEV; }
        if (!(enabled & SG_DMA_MASK)) {
            dw(s, SG_CH_INT_CLEAR, ~0ULL);
            s->dma_owned = 0;
            return 0;
        }
        udelay(10);
    }
    s->quarantined = 1;
    return -ETIMEDOUT;
}
static void start_locked(struct sg2002_audio *s) {
    unsigned index = s->ring.consumer;
    dma_sync_for_device(s->lli, SG_LLI_COUNT * sizeof(*s->lli));
    dma_sync_for_device(s->data, PCM_BYTES);
    dw(s, SG_CH_INT_CLEAR, ~0ULL);
    dw(s, SG_CH_CFG, 15ULL | (1ULL << 32) | (7ULL << 44));
    dw(s, SG_CH_LLP,
       s->lli_pa + (uint64_t)index * SG_MICROS_PER_PERIOD * sizeof(*s->lli));
    dw(s, SG_CH_SAR, s->data_pa + (uint64_t)index * PCM_PERIOD_BYTES);
    dw(s, SG_CH_DAR, s->platform->i2s_pa + 0xc0);
    dw(s, SG_CH_CTL, 0);
    dw(s, SG_CH_INT_EN, SG_DMA_ERRORS | 1);
    dw(s, SG_CH_INT_SIGNAL, SG_DMA_ERRORS | 1);
    dw(s, SG_DMA_CFG, dr(s, SG_DMA_CFG) | 3);
    update(s->dac, 0, 3, 3);
    wr(s->i2s, 0x24, 0x77);
    wr(s->i2s, 0x20, 0); /* DMA IRQ handles periods. */
    wr(s->i2s, 0x30, 1U << 16);
    wr(s->i2s, 0x30, 0);
    wr(s->i2s, 0x1c, 2);
    wr(s->i2s, 0x1c, 0);
    s->last_completion = clock_get_ticks();
    s->ring.running = 1;
    s->dma_owned = 1;
    dw(s, SG_DMA_ENABLE, SG_DMA_MASK | (SG_DMA_MASK << 8));
    wr(s->i2s, 0x18, 1);
    mute(s, 0);
}
static int audio_irq(int irq, void *arg) {
    (void)irq;
    struct sg2002_audio *s = arg;
    uint64_t flags = spin_lock_irqsave(&s->lock);
    uint64_t status = dr(s, SG_CH_INT_STATUS);
    if (!status) {
        spin_unlock_irqrestore(&s->lock, flags);
        return 0;
    }
    dw(s, SG_CH_INT_CLEAR, status);
    if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE) || !s->ring.running) {
        spin_unlock_irqrestore(&s->lock, flags);
        return 1;
    }
    if (status & SG_DMA_ERRORS) {
        idle_locked(s, -EIO);
        goto done;
    }
    if (status & 1) {
        uint64_t now = clock_get_ticks(), at = dr(s, SG_CH_SAR);
        unsigned next = (s->ring.consumer + 1) % PCM_PERIODS;
        uint64_t expected = s->data_pa + (uint64_t)next * PCM_PERIOD_BYTES;
        /* More than one period of latency/coalescing cannot be retired safely. */
        if (now - s->last_completion > clock_ticks_per_sec() * 2 / 47 ||
            ((at < expected || at >= expected + PCM_PERIOD_BYTES) &&
             !(next == 0 && at == s->data_pa + PCM_BYTES))) {
            idle_locked(s, -EPIPE);
            goto done;
        }
        unsigned retired = s->ring.consumer;
        int result = pcm_period_complete(&s->ring);
        memset(s->data + retired * PCM_PERIOD_BYTES, 0, PCM_PERIOD_BYTES);
        dma_sync_for_device(s->data + retired * PCM_PERIOD_BYTES,
                            PCM_PERIOD_BYTES);
        s->last_completion = now;
        if (result < 0)
            idle_locked(s, -EPIPE);
        /* On drain leave the serializer running until process-context abort
         * stops further DMA, then allow the bounded FIFO tail to play. */
    }
done:
    spin_unlock_irqrestore(&s->lock, flags);
    return 1;
}
static int reset_stream(struct sg2002_audio *s) {
    uint64_t flags = spin_lock_irqsave(&s->lock);
    idle_locked(s, 0);
    spin_unlock_irqrestore(&s->lock, flags);
    int rc = s->dma_owned ? halt_dma(s) : 0;
    if (rc)
        return rc;
    flags = spin_lock_irqsave(&s->lock);
    memset(&s->ring, 0, sizeof(s->ring));
    s->partial = 0;
    memset(s->data, 0, PCM_BYTES);
    dma_sync_for_device(s->data, PCM_BYTES);
    spin_unlock_irqrestore(&s->lock, flags);
    return 0;
}
static int audio_stop(device_t *dev) {
    struct sg2002_audio *s = dev->drv_priv;
    if (!s)
        return -ENODEV;
    mutex_lock(&s->writer);
    int rc = __atomic_load_n(&s->closing, __ATOMIC_ACQUIRE) ? -ENODEV : reset_stream(s);
    mutex_unlock(&s->writer);
    return rc;
}
static int audio_format(device_t *dev, const a20_audio_format_t *f) {
    if (!f || f->rate != 48000 || f->channels != 2 ||
        f->format != A20_AUDIO_FORMAT_S16_LE)
        return -EINVAL;
    return audio_stop(dev);
}
static int audio_write(device_t *dev, const void *buffer, size_t length) {
    struct sg2002_audio *s = dev->drv_priv;
    if (!s)
        return -ENODEV;
    if (!length)
        return 0;
    if (!buffer || (length & 3))
        return -EINVAL;
    if (length > 0x7fffffffU)
        return -EOVERFLOW;
    mutex_lock(&s->writer);
    size_t written = 0;
    uint64_t begin = clock_get_ticks();
    while (written < length) {
        uint64_t flags = spin_lock_irqsave(&s->lock);
        if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&s->lock, flags);
            mutex_unlock(&s->writer);
            return written ? (int)written : -ENODEV;
        }
        if (clock_get_ticks() - begin > clock_ticks_per_sec()) {
            idle_locked(s, -ETIMEDOUT);
        }
        if (s->quarantined || s->ring.error) {
            int e = s->quarantined ? -EIO : s->ring.error;
            spin_unlock_irqrestore(&s->lock, flags);
            mutex_unlock(&s->writer);
            return written ? (int)written : e;
        }
        if (s->ring.count == PCM_PERIODS) {
            spin_unlock_irqrestore(&s->lock, flags);
            mdelay(1);
            continue;
        }
        unsigned slot = s->ring.producer;
        if (s->ring.running) {
            uint64_t at = dr(s, SG_CH_SAR);
            /* The final microblock may finish before hardware reloads SAR.
             * Do not classify that transient ring boundary as corrupt DMA. */
            if (at == s->data_pa + PCM_BYTES) {
                spin_unlock_irqrestore(&s->lock, flags);
                mdelay(1);
                continue;
            }
            if (at < s->data_pa || at > s->data_pa + PCM_BYTES) {
                idle_locked(s, -EIO);
                spin_unlock_irqrestore(&s->lock, flags);
                continue;
            }
            unsigned active = (unsigned)((at - s->data_pa) / PCM_PERIOD_BYTES);
            if (slot == active || slot == (active + 1) % PCM_PERIODS) {
                spin_unlock_irqrestore(&s->lock, flags);
                mdelay(1);
                continue;
            }
        }
        size_t n = PCM_PERIOD_BYTES - s->partial;
        if (n > length - written)
            n = length - written;
        memcpy(s->data + slot * PCM_PERIOD_BYTES + s->partial,
               (const uint8_t *)buffer + written, n);
        s->partial += n;
        written += n;
        begin = clock_get_ticks();
        if (s->partial == PCM_PERIOD_BYTES) {
            dma_sync_for_device(s->data + slot * PCM_PERIOD_BYTES,
                                PCM_PERIOD_BYTES);
            pcm_period_publish(&s->ring);
            s->partial = 0;
            if (!s->ring.running && s->ring.count >= 2)
                start_locked(s);
        }
        spin_unlock_irqrestore(&s->lock, flags);
    }
    mutex_unlock(&s->writer);
    return (int)written;
}
static int audio_drain(device_t *dev) {
    struct sg2002_audio *s = dev->drv_priv;
    if (!s)
        return -ENODEV;
    mutex_lock(&s->writer);
    if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE)) {
        mutex_unlock(&s->writer);
        return -ENODEV;
    }
    uint64_t flags = spin_lock_irqsave(&s->lock);
    if (!s->ring.count && !s->partial && !s->dma_owned && !s->ring.error) {
        spin_unlock_irqrestore(&s->lock, flags);
        mutex_unlock(&s->writer);
        return 0;
    }
    s->ring.draining = 1;
    if (s->partial && !s->ring.error) {
        unsigned slot = s->ring.producer;
        memset(s->data + slot * PCM_PERIOD_BYTES + s->partial, 0,
               PCM_PERIOD_BYTES - s->partial);
        dma_sync_for_device(s->data + slot * PCM_PERIOD_BYTES,
                            PCM_PERIOD_BYTES);
        pcm_period_publish(&s->ring);
        s->partial = 0;
    }
    if (s->ring.count && !s->ring.running && !s->ring.error)
        start_locked(s);
    spin_unlock_irqrestore(&s->lock, flags);
    uint64_t begin = clock_get_ticks();
    int rc = 0;
    for (;;) {
        if (__atomic_load_n(&s->closing, __ATOMIC_ACQUIRE)) {
            mutex_unlock(&s->writer);
            return -ENODEV;
        }
        flags = spin_lock_irqsave(&s->lock);
        int count = s->ring.count;
        rc = s->ring.error;
        spin_unlock_irqrestore(&s->lock, flags);
        if (rc || !count)
            break;
        if (clock_get_ticks() - begin > clock_ticks_per_sec()) {
            rc = -ETIMEDOUT;
            break;
        }
        mdelay(1);
    }
    if (rc) {
        flags = spin_lock_irqsave(&s->lock);
        idle_locked(s, rc);
        spin_unlock_irqrestore(&s->lock, flags);
    }
    if (s->dma_owned) {
        int stop = halt_dma(s);
        if (stop)
            rc = stop;
    }
    if (rc) {
        flags = spin_lock_irqsave(&s->lock);
        idle_locked(s, rc);
        spin_unlock_irqrestore(&s->lock, flags);
    }
    /* SDK FIFO is 32x32-bit words. At verified 48k stereo S16 this is at
     * most 32 frames (667us); retain clock/serializer for 2ms including tail.
     * No undocumented TX_STATUS FIFO-count bits are guessed. */
    if (!rc) {
        if (!(rd(s->i2s, 0x18) & 1) || !(rd(s->i2s, 0x60) & 0x100))
            rc = -EIO;
        else
            udelay(2000);
    }
    if (!rc)
        rc = reset_stream(s);
    mutex_unlock(&s->writer);
    return rc;
}
static int audio_poll(device_t *dev, short events) {
    struct sg2002_audio *s = dev->drv_priv;
    if (!s)
        return POLLERR;
    uint64_t f = spin_lock_irqsave(&s->lock);
    int r = (s->ring.error || s->quarantined ||
             __atomic_load_n(&s->closing, __ATOMIC_ACQUIRE))
                ? POLLERR
                : (s->ring.count < PCM_PERIODS ? events & POLLOUT : 0);
    spin_unlock_irqrestore(&s->lock, f);
    return r;
}
static int probe(device_t *dev) {
    const sg2002_audio_platform_t *p = dev->plat_data;
    if (!p || p->magic != SG2002_AUDIO_PLATFORM_MAGIC || !p->prepare ||
        !p->dma_irq)
        return -ENODEV;
    if (fw_node_match_id(&dev->firmware_node, "cvitek,cv1835-i2s") != 1)
        return -ENODEV;
    resource_t *resource = device_get_resource(dev, RES_MMIO, 0);
    if (!resource || resource->start != p->i2s_pa ||
        resource->end < resource->start ||
        resource->end - resource->start < 0xff)
        return -ENODEV;
    /* The core drops old dependency links before every bind attempt. Rebuild
     * them here, never rely on links made during board configuration. */
    for (unsigned i = 0; i < SG2002_AUDIO_PROVIDERS; i++) {
        if (!p->provider_pa[i] || !p->provider_bytes[i])
            return -ENODEV;
        device_t *provider = device_find_get_by_mmio(p->provider_pa[i]);
        if (!provider)
            return -EPROBE_DEFER;
        /* No DMA broker exists here: a native or user-owned provider must
         * remain exclusively owned, even when channel 7 happens to be idle. */
        if (i < 2 && (provider->drv || provider->user_owned)) {
            device_put(provider);
            return -EBUSY;
        }
        resource_t *r = device_get_resource(provider, RES_MMIO, 0);
        int linked =
            (!r || r->start != p->provider_pa[i] || r->end < r->start ||
             r->end - r->start < p->provider_bytes[i] - 1)
                ? -ENODEV
                : device_add_dependency(dev, provider);
        device_put(provider);
        if (linked)
            return linked;
    }
    int rc = device_probe_use_remove(dev);
    if (rc) return rc;
    struct sg2002_audio *s = kcalloc(1, sizeof(*s));
    if (!s) return -ENOMEM;
    s->platform = p;
    spin_init(&s->lock);
    mutex_init(&s->writer);
    dev->drv_priv = s;
    rc = device_add_execution_action(dev, execution_stop, NULL, s);
    if (rc) return rc;
    rc = p->prepare();
    if (rc) return rc; /* Still no DMA allocation before cache proof. */
    s->i2s = arch_device_map_mmio(p->i2s_pa, 0x100, 0);
    s->dac = arch_device_map_mmio(p->dac_pa, 0x100, 0);
    s->dma = arch_device_map_mmio(p->dma_pa, 0x1000, 0);
    if (!s->i2s || !s->dac || !s->dma) {
        rc = -ENXIO;
        goto fail;
    }
    if (dr(s, SG_DMA_ENABLE) & SG_DMA_MASK) {
        rc = -EBUSY;
        goto fail;
    }
    s->channel_owned = 1;
    if (dma_set_mask(dev, DMA_MASK_32BIT)) {
        rc = -EIO;
        goto fail;
    }
    s->data = dma_alloc_coherent_aligned(dev, PCM_BYTES, 64, &s->data_pa);
    s->lli = dma_alloc_coherent_aligned(dev, SG_LLI_COUNT * sizeof(*s->lli), 64,
                                        &s->lli_pa);
    if (!s->data || !s->lli) {
        rc = -ENOMEM;
        goto fail;
    }
    memset(s->data, 0, PCM_BYTES);
    sg2002_build_lli(s->lli, s->data_pa, s->lli_pa, p->i2s_pa + 0xc0);
    mute(s, 1);
    update(s->dac, 0, 3, 0);
    update(s->dac, 4, 3, 0);
    update(s->dac, 0xc, 0xffff, 0x19);
    wr(s->dac, 0x10, 0x00ff00ff); /* half-scale digital gain, both channels */
    wr(s->i2s, 0x18, 0);
    wr(s->i2s, 0x00, 0x83);       /* TX, master, hardware DMA */
    wr(s->i2s, 0x04, 0x000f200f); /* SDK cv182xa S16 stereo special framing */
    wr(s->i2s, 0x08, 0x000f0f01);
    wr(s->i2s, 0x0c, 3);
    wr(s->i2s, 0x10, 2);
    wr(s->i2s, 0x28, 8U << 16);
    wr(s->i2s, 0x60, 0x1c0);
    wr(s->i2s, 0x64, 0x00100002);
    wr(s->i2s, 0x2c, 1);
    rc = request_irq(p->dma_irq, audio_irq, IRQF_SHARED, s);
    if (rc)
        goto fail;
    s->irq_live = 1;
    dev->drv_priv = s;
    irq_enable(p->dma_irq);
    printf("[AUDIO] SG2002 internal DAC: 48000 Hz stereo S16_LE, fixed-enable "
           "PA\n");
    return 0;
fail:
    return rc; /* Core runs fallible stop before releasing a failed probe. */
}

static int execution_stop(void *context) {
    struct sg2002_audio *s = context;
    __atomic_store_n(&s->closing, 1, __ATOMIC_RELEASE);
    if (!mutex_trylock(&s->writer)) return -EBUSY;
    uint64_t flags;
    if (!spin_trylock_irqsave(&s->lock, &flags)) {
        mutex_unlock(&s->writer);
        return -EBUSY;
    }
    int rc = 0;
    if (s->channel_owned) {
        idle_locked(s, 0);
        update(s->dac, 0, 3, 0);
        if ((rd(s->i2s, 0x18) & 1) || (rd(s->dac, 0) & 3) ||
            (rd(s->dac, 0x28) & (3U << 16)) != (3U << 16)) rc = -EIO;
    }
    spin_unlock_irqrestore(&s->lock, flags);
    /* An idle stream still owns channel 7 until the binding is removed. */
    if (!rc && s->channel_owned) rc = halt_dma(s);
    if (!rc && s->irq_live) {
        free_irq(s->platform->dma_irq, s);
        s->irq_live = 0;
    }
    mutex_unlock(&s->writer);
    return rc;
}

static int remove_device(device_t *dev) {
    struct sg2002_audio *s = dev->drv_priv;
    if (!s) return 0;
    int rc = execution_stop(s);
    if (rc) return rc;
    if (s->lli)
        dma_free_coherent_aligned(s->lli, SG_LLI_COUNT * sizeof(*s->lli), s->lli_pa);
    if (s->data) dma_free_coherent_aligned(s->data, PCM_BYTES, s->data_pa);
    if (s->dma) arch_device_unmap_mmio((void *)s->dma, 0x1000);
    if (s->dac) arch_device_unmap_mmio((void *)s->dac, 0x100);
    if (s->i2s) arch_device_unmap_mmio((void *)s->i2s, 0x100);
    dev->drv_priv = NULL;
    kfree(s);
    return 0;
}
static const firmware_device_id_t ids[] = {{.id = "cvitek,cv1835-i2s"}, {0}};
static const audio_dev_ops_t ops = {
    .caps = {1, A20_AUDIO_CAP_PCM, 48000, 48000},
    .pcm_format = {48000, 2, A20_AUDIO_FORMAT_S16_LE},
    .write = audio_write,
    .set_format = audio_format,
    .stop = audio_stop,
    .drain = audio_drain,
    .poll = audio_poll,
    .close = audio_stop,
};
static driver_t driver = {.name = "sg2002-audio",
                          .bus = &platform_bus,
                          .firmware_ids = ids,
                          .probe = probe,
                          .remove = remove_device,
                          .class_type = DEV_CLASS_AUDIO,
                          .class_ops = &ops};
DRIVER_REGISTER(driver);
