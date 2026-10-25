/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_SUNXI_AUDIO_H
#define A20_SUNXI_AUDIO_H
#include "core/types.h"
#define SUNXI_AUDIO_PLATFORM_MAGIC 0x53415043U
#define SUNXI_AUDIO_A64 1U
#define SUNXI_AUDIO_V3S 2U
/* The architecture selects an electrically connected topology and supplies
 * resources from the enumerated devices. This driver never selects a board,
 * synthesizes an interrupt number, or assumes a headphone supply is on. */
typedef struct sunxi_audio_platform {
    uint32_t magic, variant;
    uint64_t digital_pa, analog_pa, dma_pa, ccu_pa, rsb_pa;
    uint32_t dma_irq;
    int (*prepare)(void);
    int (*power)(int on);
} sunxi_audio_platform_t;
#endif
