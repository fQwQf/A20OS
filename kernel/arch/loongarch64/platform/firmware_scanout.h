/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_LS2K0300_FIRMWARE_SCANOUT_H
#define A20_LS2K0300_FIRMWARE_SCANOUT_H

#include <stdint.h>
#include <stddef.h>
#include "core/errno.h"

int ls2k0300_scanout_prepare(uint64_t physical, size_t bytes);

/* LA264's documented 32KiB L1I, 32KiB L1D and 512KiB shared L2.
 * CPUCFG enumerates leaves in order, NOT by level: L1I=0, L1D=1, L2U=2.
 * Linux arch/loongarch/mm/cache.c and include/asm/cacheops.h use this order.
 * Unknown topology fails closed; this is not a general cache controller. */
typedef struct ls2k0300_scanout_plan {
    uintptr_t cached;
    size_t bytes;
    size_t line[2];
} ls2k0300_scanout_plan_t;

static inline int ls2k0300_cache_geometry(uint32_t cfg, uint64_t bytes) {
    unsigned sets_log = (cfg >> 16) & 255U;
    unsigned line_log = (cfg >> 24) & 127U;
    uint64_t ways = (cfg & 65535U) + 1ULL;
    return !(cfg & 0x80000000U) && line_log == 6 && sets_log < 24 &&
           ways * (1ULL << sets_log) * 64 == bytes;
}

static inline int ls2k0300_scanout_plan(uint64_t physical, size_t bytes,
                                        uint32_t topology,
                                        const uint32_t geometry[3],
                                        uint64_t cached_dmw,
                                        ls2k0300_scanout_plan_t *out) {
    if (!out || !geometry || !bytes || (physical & 4095U) || (bytes & 4095U) ||
        physical < 0x08000000ULL || physical >= 0x0f000000ULL ||
        bytes > 0x0f000000ULL - physical)
        return -EINVAL;
    /* L1I + L1D + L2IU + L2-unified; only private/inclusive L2 flags vary. */
    if ((topology & ~0x60U) != 0x1dU || cached_dmw != 0x9000000000000011ULL ||
        !ls2k0300_cache_geometry(geometry[0], 32768) ||
        !ls2k0300_cache_geometry(geometry[1], 32768) ||
        !ls2k0300_cache_geometry(geometry[2], 524288))
        return -EOPNOTSUPP;
    *out = (ls2k0300_scanout_plan_t){
        (uintptr_t)(physical | 0x9000000000000000ULL), bytes, {64, 64}};
    return 0;
}

/* The borrowed DDR range is excluded from PFA, is never executed, and must
 * never again be accessed through a cached CPU alias after this handoff.
 * Drain L1 writebacks before evicting L2, then drain L2 to memory before UC
 * stores. No global index operation can discard unrelated dirty kernel data. */
static inline void ls2k0300_scanout_apply(const ls2k0300_scanout_plan_t *plan,
                                          void (*cacheop)(unsigned, uintptr_t),
                                          void (*drain)(void)) {
    drain();
    for (unsigned leaf = 0; leaf < 2; leaf++) {
        for (size_t offset = 0; offset < plan->bytes;
             offset += plan->line[leaf])
            cacheop(0x11U + leaf, plan->cached + offset);
        drain();
    }
}

#endif
