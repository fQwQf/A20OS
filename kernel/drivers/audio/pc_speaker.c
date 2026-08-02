/*
 * PC speaker tone backend
 */
#ifdef CONFIG_PC_SPEAKER

#include "drivers/audio/pc_speaker.h"
#include "drivers/audio/audio_core.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/core/driver_class.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/lock.h"
#include "mm/slab.h"

#define PIT_INPUT_HZ 1193182U
#define PC_SPEAKER_MIN_HZ ((PIT_INPUT_HZ + 0xffffU - 1U) / 0xffffU)
#define PC_SPEAKER_MAX_HZ 20000U

typedef struct pc_speaker {
    uint16_t pit_command;
    uint16_t pit_channel2;
    uint16_t control;
    spinlock_t lock;
    uint32_t generation;
} pc_speaker_t;

static void pc_speaker_stop_locked(pc_speaker_t *speaker)
{
    uint8_t value = ioport_read8(speaker->control);
    ioport_write8(speaker->control, value & (uint8_t)~0x03U);
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
    uint32_t generation = ++speaker->generation;
    ioport_write8(speaker->pit_command, 0xb6U);
    ioport_write8(speaker->pit_channel2, (uint8_t)divisor);
    ioport_write8(speaker->pit_channel2, (uint8_t)(divisor >> 8));
    uint8_t value = ioport_read8(speaker->control);
    ioport_write8(speaker->control, value | 0x03U);
    spin_unlock_irqrestore(&speaker->lock, flags);
    if (duration_ms) {
        mdelay(duration_ms);
        flags = spin_lock_irqsave(&speaker->lock);
        if (speaker->generation == generation)
            pc_speaker_stop_locked(speaker);
        spin_unlock_irqrestore(&speaker->lock, flags);
    }
    return 0;
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
    speaker->generation++;
    pc_speaker_stop_locked(speaker);
    spin_unlock_irqrestore(&speaker->lock, flags);
    return 0;
}

static int pc_speaker_tone_device(device_t *dev,
                                  const a20_audio_tone_t *tone)
{
    pc_speaker_t *speaker = dev ? dev->drv_priv : NULL;
    if (!speaker || !tone)
        return -ENODEV;
    return pc_speaker_tone(speaker, tone->frequency_hz, tone->duration_ms);
}

static int pc_speaker_probe(device_t *dev)
{
    resource_t *pit = device_get_resource(dev, RES_IOPORT, 0);
    resource_t *control = device_get_resource(dev, RES_IOPORT, 1);
    if (!pit || !control || pit->end < pit->start + 1U)
        return -ENODEV;
    pc_speaker_t *speaker = kcalloc(1, sizeof(*speaker));
    if (!speaker)
        return -ENOMEM;
    speaker->pit_channel2 = (uint16_t)pit->start;
    speaker->pit_command = (uint16_t)(pit->start + 1U);
    speaker->control = (uint16_t)control->start;
    spin_init(&speaker->lock);
    pc_speaker_stop_locked(speaker);
    dev->drv_priv = speaker;
    return 0;
}

static int pc_speaker_remove(device_t *dev)
{
    pc_speaker_t *speaker = dev ? dev->drv_priv : NULL;
    if (speaker) {
        uint64_t flags = spin_lock_irqsave(&speaker->lock);
        speaker->generation++;
        pc_speaker_stop_locked(speaker);
        spin_unlock_irqrestore(&speaker->lock, flags);
        kfree(speaker);
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

static const device_id_t pc_speaker_ids[] = {
    { .vendor = A20_PLATFORM_VENDOR, .device = A20_DEVICE_PC_SPEAKER },
    { 0 },
};

static driver_t pc_speaker_driver = {
    .name = "pc-speaker",
    .id_table = pc_speaker_ids,
    .bus = &platform_bus,
    .probe = pc_speaker_probe,
    .remove = pc_speaker_remove,
    .class_ops = &pc_speaker_ops,
    .class_type = DEV_CLASS_AUDIO,
};

DRIVER_REGISTER(pc_speaker_driver);

#endif
