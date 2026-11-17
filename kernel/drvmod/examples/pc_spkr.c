/*
 * PC speaker tone backend — drvmod module (x86_64).
 *
 * Migrated from kernel/drivers/audio/pc_speaker.c: the same driver_t
 * object is registered into the unified driver core through the framework
 * bridge (drv_driver_register), so userspace still reaches it through the
 * audio class (/dev/audioN).  Resource access is port I/O only
 * (drv_in8/drv_out8); no MMIO or IRQ is used.
 */

#include "drvmod/drvmod.h"
#include "drivers/audio/pc_speaker.h"

A20_DRIVER_DESCRIPTOR(A20_DRIVER_PLACEMENT_KERNEL_MODULE,
                      A20_DRIVER_TYPE_AUDIO, "pc-speaker", A20_DRIVER_ABI, A20_DRIVER_RES_IOPORT,
                      0, 1,
                      A20_DRIVER_MATCH(A20_DRIVER_BUS_FIXED,
                                           A20_PLATFORM_VENDOR,
                                           A20_DEVICE_PC_SPEAKER));

#include "drivers/audio/audio_core.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_core.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/lock.h"

#define PIT_INPUT_HZ 1193182U
#define PC_SPEAKER_MIN_HZ ((PIT_INPUT_HZ + 0xffffU - 1U) / 0xffffU)
#define PC_SPEAKER_MAX_HZ 20000U

typedef struct pc_speaker {
    uint16_t pit_command;
    uint16_t pit_channel2;
    uint16_t control;
    spinlock_t lock;
    uint32_t generation;
    int closing;
    int hardware_owned;
    int stopped;
} pc_speaker_t;

static int pc_speaker_stop_locked(pc_speaker_t *speaker)
{
    uint8_t value = drv_in8(speaker->control);
    if (value == 0xffU)
        return -ENODEV;
    drv_out8(speaker->control, value & (uint8_t)~0x03U);
    value = drv_in8(speaker->control);
    if (value == 0xffU)
        return -ENODEV;
    return value & 0x03U ? -EIO : 0;
}

static int pc_speaker_tone(pc_speaker_t *speaker, uint32_t hz,
                           uint32_t duration_ms)
{
    if (hz < PC_SPEAKER_MIN_HZ || hz > PC_SPEAKER_MAX_HZ ||
        duration_ms > 5000U)
        return -EINVAL;
    uint32_t divisor = PIT_INPUT_HZ / hz;
    if (!divisor || divisor > 0xffffU)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&speaker->lock);
    if (__atomic_load_n(&speaker->closing, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&speaker->lock, flags);
        return -ESHUTDOWN;
    }
    uint8_t value = drv_in8(speaker->control);
    if (value == 0xffU) {
        __atomic_store_n(&speaker->closing, 1, __ATOMIC_RELEASE);
        spin_unlock_irqrestore(&speaker->lock, flags);
        return -ENODEV;
    }
    uint32_t generation = __atomic_add_fetch(&speaker->generation, 1, __ATOMIC_RELAXED);
    drv_out8(speaker->pit_command, 0xb6U);
    drv_out8(speaker->pit_channel2, (uint8_t)divisor);
    drv_out8(speaker->pit_channel2, (uint8_t)(divisor >> 8));
    drv_out8(speaker->control, value | 0x03U);
    value = drv_in8(speaker->control);
    int ret = value == 0xffU ? -ENODEV : (value & 3U) != 3U ? -EIO : 0;
    if (ret)
        __atomic_store_n(&speaker->closing, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&speaker->lock, flags);
    if (ret)
        return ret;
    if (duration_ms) {
        /* Class active-call drain pins this instance. Poll cancellation in
         * short task-context slices instead of delaying remove for five seconds. */
        for (uint32_t remaining = duration_ms; remaining;) {
            if (__atomic_load_n(&speaker->closing, __ATOMIC_ACQUIRE))
                return -ESHUTDOWN;
            if (__atomic_load_n(&speaker->generation, __ATOMIC_RELAXED) != generation)
                return 0; /* Explicit stop or a newer tone supersedes us. */
            uint32_t slice = remaining > 10U ? 10U : remaining;
            drv_mdelay(slice);
            remaining -= slice;
        }
        flags = spin_lock_irqsave(&speaker->lock);
        if (__atomic_load_n(&speaker->closing, __ATOMIC_ACQUIRE)) {
            ret = -ESHUTDOWN;
        } else if (__atomic_load_n(&speaker->generation, __ATOMIC_RELAXED) == generation) {
            ret = pc_speaker_stop_locked(speaker);
            if (ret)
                __atomic_store_n(&speaker->closing, 1, __ATOMIC_RELEASE);
        }
        spin_unlock_irqrestore(&speaker->lock, flags);
    }
    return ret;
}

static int pc_speaker_write(device_t *dev, const void *buf, size_t count)
{
    pc_speaker_t *speaker = dev ? dev->drv_priv : NULL;
    if (!speaker || !buf || count != sizeof(a20_audio_tone_t))
        return -EINVAL;
    a20_audio_tone_t tone;
    memcpy(&tone, buf, sizeof(tone));
    int ret = pc_speaker_tone(speaker, tone.frequency_hz, tone.duration_ms);
    return ret < 0 ? ret : (int)count;
}

static int pc_speaker_stop(device_t *dev)
{
    pc_speaker_t *speaker = dev ? dev->drv_priv : NULL;
    if (!speaker)
        return -ENODEV;
    uint64_t flags = spin_lock_irqsave(&speaker->lock);
    if (__atomic_load_n(&speaker->closing, __ATOMIC_ACQUIRE)) {
        spin_unlock_irqrestore(&speaker->lock, flags);
        return -ESHUTDOWN;
    }
    __atomic_add_fetch(&speaker->generation, 1, __ATOMIC_RELAXED);
    int ret = pc_speaker_stop_locked(speaker);
    if (ret)
        __atomic_store_n(&speaker->closing, 1, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&speaker->lock, flags);
    return ret;
}

static int pc_speaker_tone_device(device_t *dev,
                                  const a20_audio_tone_t *tone)
{
    pc_speaker_t *speaker = dev ? dev->drv_priv : NULL;
    if (!speaker || !tone)
        return -ENODEV;
    return pc_speaker_tone(speaker, tone->frequency_hz, tone->duration_ms);
}

static int pc_speaker_execution_stop(void *context)
{
    pc_speaker_t *speaker = context;
    __atomic_store_n(&speaker->closing, 1, __ATOMIC_RELEASE);
    uint64_t flags;
    if (!spin_trylock_irqsave(&speaker->lock, &flags))
        return -EBUSY;
    __atomic_add_fetch(&speaker->generation, 1, __ATOMIC_RELAXED);
    int ret = 0;
    if (!speaker->stopped && speaker->hardware_owned)
        ret = pc_speaker_stop_locked(speaker);
    if (!ret)
        speaker->stopped = 1;
    spin_unlock_irqrestore(&speaker->lock, flags);
    return ret;
}

static int pc_speaker_probe(device_t *dev)
{
    resource_t *pit = (resource_t *)drv_device_get_resource(dev, RES_IOPORT, 0);
    resource_t *control = (resource_t *)drv_device_get_resource(dev, RES_IOPORT, 1);
    if (!pit || !control || pit->end < pit->start || pit->end > 0xffffU ||
        pit->end - pit->start < 1U || control->end < control->start ||
        control->end > 0xffffU)
        return -ENODEV;
    int ret = device_probe_use_remove(dev);
    if (ret)
        return ret;
    pc_speaker_t *speaker = (pc_speaker_t *)drv_alloc(sizeof(*speaker));
    if (!speaker)
        return -ENOMEM;
    memset(speaker, 0, sizeof(*speaker));
    speaker->pit_channel2 = (uint16_t)pit->start;
    speaker->pit_command = (uint16_t)(pit->start + 1U);
    speaker->control = (uint16_t)control->start;
    spin_init(&speaker->lock);
    dev->drv_priv = speaker;
    ret = device_add_execution_action(dev, pc_speaker_execution_stop, drv_free, speaker);
    if (ret) {
        dev->drv_priv = NULL;
        drv_free(speaker);
        return ret;
    }
    /* Claim both port windows before the first IN/OUT. A partial claim never
     * authorizes hardware writes and is released by managed probe cleanup. */
    ret = device_claim_resource(dev, pit);
    if (!ret)
        ret = device_claim_resource(dev, control);
    if (ret)
        return ret;
    speaker->hardware_owned = 1;
    uint64_t flags = spin_lock_irqsave(&speaker->lock);
    ret = pc_speaker_stop_locked(speaker);
    spin_unlock_irqrestore(&speaker->lock, flags);
    return ret;
}

static int pc_speaker_remove(device_t *dev)
{
    pc_speaker_t *speaker = dev ? dev->drv_priv : NULL;
    if (speaker) {
        if (!speaker->stopped)
            return -EBUSY;
        /* Final devres release follows class active-call drain. */
        dev->drv_priv = NULL;
    }
    return 0;
}

static const audio_dev_ops_t pc_speaker_ops = {
    .caps = {
        .version = 1,
        .flags = A20_AUDIO_CAP_TONE,
        .min_rate = PC_SPEAKER_MIN_HZ,
        .max_rate = PC_SPEAKER_MAX_HZ,
    },
    .write = pc_speaker_write,
    .tone = pc_speaker_tone_device,
    .stop = pc_speaker_stop,
};


static const firmware_device_id_t firmware_ids[] = {
    {.id = "PNP0800"}, {0}
};
static driver_t pc_speaker_driver = {
    .name = "pc-speaker",
    .firmware_ids = firmware_ids,
    .bus = &platform_bus,
    .probe = pc_speaker_probe,
    .remove = pc_speaker_remove,
    .class_ops = &pc_speaker_ops,
    .class_type = DEV_CLASS_AUDIO,
};

uintptr_t DriverEntry(void)
{
    int r = drv_driver_register(&pc_speaker_driver);
    drv_log("[PC-SPKR] driver registered in core: %d\n", r);
    return r == 0 ? 0 : 1;
}
