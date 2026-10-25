/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_RISCV_DMA_CACHE_H
#define A20_RISCV_DMA_CACHE_H
#include "core/types.h"
/* T-Head operations take physical addresses, not higher-half CPU pointers. */
static inline int riscv_dma_physical_span(uintptr_t address, size_t length,
                                          uintptr_t offset, uint64_t *first,
                                          uint64_t *last) {
    if (!length || address < offset || length - 1 > (uintptr_t)-1 - address)
        return 0;
    *first = (address - offset) & ~(uint64_t)63;
    *last = ((uint64_t)(address - offset) + length - 1) & ~(uint64_t)63;
    return 1;
}
int riscv_sg2002_dma_cache_prepare(void);
/* Architecture capability consumed by the generic DMA allocation boundary. */
int arch_device_dma_ready(void);
void riscv_dma_sync_device(const void *address, size_t length);
void riscv_dma_sync_cpu(const void *address, size_t length);
#endif
