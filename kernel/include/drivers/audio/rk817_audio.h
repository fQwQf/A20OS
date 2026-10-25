/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_RK817_AUDIO_H
#define A20_RK817_AUDIO_H
#include "core/types.h"
#define RK817_AUDIO_MAGIC 0x524b4155U
/* Electrically validated board topology. Providers must be reconstructed at
 * probe time, including those used behind the register/clock callbacks. */
typedef struct rk817_audio_platform {
    uint32_t magic, dma_irq[2], dma_request;
    uint64_t dma_pa, provider_pa[5];
    int (*prepare)(void);
    int (*codec_register)(uint8_t reg, uint8_t *value, int write);
} rk817_audio_platform_t;
#endif
