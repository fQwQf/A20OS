#ifndef _DRIVERS_AUDIO_AUDIO_CORE_H
#define _DRIVERS_AUDIO_AUDIO_CORE_H

#include "drivers/core/driver_core.h"
#include "uapi/a20/audio.h"

typedef struct audio_dev_ops {
    a20_audio_caps_t caps;
    a20_audio_format_t pcm_format;
    int (*read)(device_t *dev, void *buf, size_t count);
    int (*write)(device_t *dev, const void *buf, size_t count);
    int (*set_format)(device_t *dev, const a20_audio_format_t *format);
    int (*tone)(device_t *dev, const a20_audio_tone_t *tone);
    int (*stop)(device_t *dev);
    int (*drain)(device_t *dev);
    int (*poll)(device_t *dev, short events);
    int (*ioctl)(device_t *dev, unsigned long req, void *arg);
    int (*close)(device_t *dev);
} audio_dev_ops_t;

int audio_device_ioctl(device_t *dev, const audio_dev_ops_t *ops,
                       unsigned long req, void *arg);
int audio_device_close(device_t *dev, const audio_dev_ops_t *ops);

#endif
