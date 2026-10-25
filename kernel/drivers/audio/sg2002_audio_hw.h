/* SPDX-License-Identifier: Apache-2.0
 * SG200x facts from SDK d4003f15: drivers/dma/cvitek/cvitek-dma.{c,h},
 * sound/soc/cvitek/{cv1835_i2s.c,local.h,cv181xdac.c}. Not DW AHB layout.
 */
#ifndef A20_SG2002_AUDIO_HW_H
#define A20_SG2002_AUDIO_HW_H
#include "core/types.h"
#include "pcm_period_ring.h"
#define SG_DMA_CH 7U
#define SG_DMA_MASK (1ULL << SG_DMA_CH)
#define SG_DMA_CFG 0x10U
#define SG_DMA_ENABLE 0x18U
#define SG_DMA_CHANNEL (0x100U + SG_DMA_CH * 0x100U)
#define SG_CH_SAR (SG_DMA_CHANNEL + 0x00U)
#define SG_CH_DAR (SG_DMA_CHANNEL + 0x08U)
#define SG_CH_CTL (SG_DMA_CHANNEL + 0x18U)
#define SG_CH_CFG (SG_DMA_CHANNEL + 0x20U)
#define SG_CH_LLP (SG_DMA_CHANNEL + 0x28U)
#define SG_CH_INT_EN (SG_DMA_CHANNEL + 0x80U)
#define SG_CH_INT_STATUS (SG_DMA_CHANNEL + 0x88U)
#define SG_CH_INT_SIGNAL (SG_DMA_CHANNEL + 0x90U)
#define SG_CH_INT_CLEAR (SG_DMA_CHANNEL + 0x98U)
#define SG_DMA_ERRORS 0x80017fe0ULL
#define SG_MICRO_BYTES 64U /* SDK BM_DMA_PERIOD_LEN, 16x32-bit transactions */
#define SG_MICROS_PER_PERIOD (PCM_PERIOD_BYTES / SG_MICRO_BYTES)
#define SG_LLI_COUNT (PCM_BYTES / SG_MICRO_BYTES)
#define SG_LLI_VALID (1ULL << 63)
#define SG_LLI_IOC (1ULL << 58)
struct sg2002_lli {
    uint64_t source, destination, block_ts, next, control;
    uint32_t source_status, destination_status;
    uint64_t next_status, reserved;
};
_Static_assert(sizeof(struct sg2002_lli) == 64, "SG200x LLI ABI");
static inline void sg2002_build_lli(struct sg2002_lli *lli, uint64_t data_pa,
                                    uint64_t lli_pa, uint64_t fifo_pa) {
    for (unsigned i = 0; i < SG_LLI_COUNT; i++) {
        lli[i] = (struct sg2002_lli){
            .source = data_pa + (uint64_t)i * SG_MICRO_BYTES,
            .destination = fifo_pa,
            .block_ts = SG_MICRO_BYTES / 4 - 1,
            .next = lli_pa + (uint64_t)((i + 1) % SG_LLI_COUNT) * sizeof(*lli),
            .control = SG_LLI_VALID | 5ULL | (1ULL << 6) | (2ULL << 8) |
                       (2ULL << 11) | (2ULL << 14) | (2ULL << 18) |
                       (1ULL << 38) | (7ULL << 39) | (1ULL << 47),
        };
        if ((i + 1) % SG_MICROS_PER_PERIOD == 0)
            lli[i].control |= SG_LLI_IOC;
    }
}
#endif
