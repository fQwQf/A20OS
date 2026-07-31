#ifndef _DRIVERS_AUDIO_HDA_INTERNAL_H
#define _DRIVERS_AUDIO_HDA_INTERNAL_H

#include "core/sync.h"

#define HDA_PCM_FORMAT   0x0011U
#define HDA_PCM_FRAME    4U
#define HDA_TIMEOUT_MS   1000U
#define HDA_MAX_NODES    64U

typedef struct __attribute__((packed)) hda_bdle {
    uint64_t address;
    uint32_t length;
    uint32_t flags;
} hda_bdle_t;

typedef struct hda_controller {
    uintptr_t regs;
    uintptr_t stream;
    hda_bdle_t *bdl;
    uint64_t bdl_dma;
    uint8_t *pcm;
    uint64_t pcm_dma;
    uint8_t codec;
    uint8_t afg;
    uint8_t dac;
    uint8_t pin;
    uint8_t path[HDA_MAX_NODES];
    uint8_t path_select[HDA_MAX_NODES];
    uint8_t path_len;
    uint32_t afg_pcm;
    uint32_t afg_formats;
    uint8_t pending[HDA_PCM_FRAME];
    uint8_t pending_len;
    uint32_t pending_generation;
    size_t write_pos;
    size_t queued_bytes;
    uint32_t last_lpib;
    uint32_t stream_starts;
    uint32_t stream_underruns;
    int stream_running;
    int stream_draining;
    int codec_configured;
    int dma64;
    volatile uint32_t generation;
    spinlock_t state_lock;
    mutex_t lock;
} hda_controller_t;

static inline volatile void *hda_reg(hda_controller_t *hda, uint32_t offset)
{
    return (volatile void *)(hda->regs + offset);
}

int hda_codec_discover_and_setup(hda_controller_t *hda, const char **stage);
void hda_codec_disable(hda_controller_t *hda);

#endif
