#ifndef _UAPI_A20_AUDIO_H
#define _UAPI_A20_AUDIO_H

typedef __UINT16_TYPE__ a20_audio_u16_t;
typedef __UINT32_TYPE__ a20_audio_u32_t;

#define A20_AUDIO_CAP_TONE  (1U << 0)
#define A20_AUDIO_CAP_PCM   (1U << 1)

#define A20_AUDIO_IOCTL_GET_CAPS 0x41300001UL
#define A20_AUDIO_IOCTL_TONE     0x41300002UL
#define A20_AUDIO_IOCTL_STOP     0x41300003UL
#define A20_AUDIO_IOCTL_SET_FORMAT 0x41300004UL

#define A20_AUDIO_FORMAT_S16_LE 1U

typedef struct a20_audio_caps {
    a20_audio_u32_t version;
    a20_audio_u32_t flags;
    a20_audio_u32_t min_rate;
    a20_audio_u32_t max_rate;
} a20_audio_caps_t;

typedef struct a20_audio_tone {
    a20_audio_u32_t frequency_hz;
    a20_audio_u32_t duration_ms;
} a20_audio_tone_t;

typedef struct a20_audio_format {
    a20_audio_u32_t rate;
    a20_audio_u16_t channels;
    a20_audio_u16_t format;
} a20_audio_format_t;

#endif
