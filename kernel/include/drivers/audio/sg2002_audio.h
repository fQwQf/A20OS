/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_SG2002_AUDIO_H
#define A20_SG2002_AUDIO_H
#include "core/types.h"
#define SG2002_AUDIO_PLATFORM_MAGIC 0x53474155U
#define SG2002_AUDIO_PROVIDERS 7U
typedef struct sg2002_audio_platform {
    uint32_t magic, dma_irq;
    uint64_t i2s_pa, dac_pa, dma_pa;
    uint64_t provider_pa[SG2002_AUDIO_PROVIDERS];
    size_t provider_bytes[SG2002_AUDIO_PROVIDERS];
    int (*prepare)(void);
} sg2002_audio_platform_t;
#endif
