#ifndef _UAPI_A20_AUDIO_H
#define _UAPI_A20_AUDIO_H

#include "core/types.h"

#define A20_AUDIO_CAP_TONE  (1U << 0)
#define A20_AUDIO_CAP_PCM   (1U << 1)

#define A20_AUDIO_IOCTL_GET_CAPS 0x41300001UL
#define A20_AUDIO_IOCTL_TONE     0x41300002UL
#define A20_AUDIO_IOCTL_STOP     0x41300003UL
#define A20_AUDIO_IOCTL_SET_FORMAT 0x41300004UL

#define A20_AUDIO_FORMAT_S16_LE 1U

typedef struct a20_audio_caps {
    uint32_t version;
    uint32_t flags;
    uint32_t min_rate;
    uint32_t max_rate;
} a20_audio_caps_t;

typedef struct a20_audio_tone {
    uint32_t frequency_hz;
    uint32_t duration_ms;
} a20_audio_tone_t;

typedef struct a20_audio_format {
    uint32_t rate;
    uint16_t channels;
    uint16_t format;
} a20_audio_format_t;

#endif
