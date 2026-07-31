#ifndef _DRIVERS_AUDIO_VIRTIO_SND_H
#define _DRIVERS_AUDIO_VIRTIO_SND_H

#include "core/types.h"

#define VIRTIO_SND_DEVICE_ID 25U

#define VIRTIO_SND_QUEUE_CONTROL 0U
#define VIRTIO_SND_QUEUE_EVENT   1U
#define VIRTIO_SND_QUEUE_TX      2U
#define VIRTIO_SND_QUEUE_RX      3U
#define VIRTIO_SND_QUEUE_COUNT   4U

#define VIRTIO_SND_CONFIG_JACKS   0U
#define VIRTIO_SND_CONFIG_STREAMS 4U
#define VIRTIO_SND_CONFIG_CHMAPS  8U

#define VIRTIO_SND_R_JACK_INFO      0x0001U
#define VIRTIO_SND_R_JACK_REMAP     0x0002U
#define VIRTIO_SND_R_PCM_INFO       0x0100U
#define VIRTIO_SND_R_PCM_SET_PARAMS 0x0101U
#define VIRTIO_SND_R_PCM_PREPARE    0x0102U
#define VIRTIO_SND_R_PCM_RELEASE    0x0103U
#define VIRTIO_SND_R_PCM_START      0x0104U
#define VIRTIO_SND_R_PCM_STOP       0x0105U
#define VIRTIO_SND_R_CHMAP_INFO     0x0200U

#define VIRTIO_SND_EVT_JACK_CONNECTED    0x1000U
#define VIRTIO_SND_EVT_JACK_DISCONNECTED 0x1001U
#define VIRTIO_SND_EVT_PCM_PERIOD_ELAPSED 0x1100U
#define VIRTIO_SND_EVT_PCM_XRUN           0x1101U

#define VIRTIO_SND_S_OK       0x8000U
#define VIRTIO_SND_S_BAD_MSG  0x8001U
#define VIRTIO_SND_S_NOT_SUPP 0x8002U
#define VIRTIO_SND_S_IO_ERR   0x8003U

#define VIRTIO_SND_D_OUTPUT 0U
#define VIRTIO_SND_D_INPUT  1U

#define VIRTIO_SND_PCM_FMT_S16   5U
#define VIRTIO_SND_PCM_RATE_48000 7U

typedef struct virtio_snd_hdr {
    uint32_t code;
} __attribute__((packed)) virtio_snd_hdr_t;

typedef struct virtio_snd_query_info {
    virtio_snd_hdr_t hdr;
    uint32_t start_id;
    uint32_t count;
    uint32_t size;
} __attribute__((packed)) virtio_snd_query_info_t;

typedef struct virtio_snd_info {
    uint32_t hda_fn_nid;
} __attribute__((packed)) virtio_snd_info_t;

typedef struct virtio_snd_pcm_info {
    virtio_snd_info_t hdr;
    uint32_t features;
    uint64_t formats;
    uint64_t rates;
    uint8_t direction;
    uint8_t channels_min;
    uint8_t channels_max;
    uint8_t padding[5];
} __attribute__((packed)) virtio_snd_pcm_info_t;

typedef struct virtio_snd_pcm_hdr {
    virtio_snd_hdr_t hdr;
    uint32_t stream_id;
} __attribute__((packed)) virtio_snd_pcm_hdr_t;

typedef struct virtio_snd_pcm_set_params {
    virtio_snd_pcm_hdr_t hdr;
    uint32_t buffer_bytes;
    uint32_t period_bytes;
    uint32_t features;
    uint8_t channels;
    uint8_t format;
    uint8_t rate;
    uint8_t padding;
} __attribute__((packed)) virtio_snd_pcm_set_params_t;

typedef struct virtio_snd_pcm_xfer {
    uint32_t stream_id;
} __attribute__((packed)) virtio_snd_pcm_xfer_t;

typedef struct virtio_snd_pcm_status {
    uint32_t status;
    uint32_t latency_bytes;
} __attribute__((packed)) virtio_snd_pcm_status_t;

typedef struct virtio_snd_event {
    virtio_snd_hdr_t hdr;
    uint32_t data;
} __attribute__((packed)) virtio_snd_event_t;

#endif
