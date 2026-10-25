/* SPDX-License-Identifier: Apache-2.0
 * Borrowed LS2K0300 firmware DDR: cached boot-logo -> uncached fbdev handoff.
 * References: LoongArch Vol1 4.2.3.1 CACOP and CPUCFG cache descriptors;
 * Linux v6.17 arch/loongarch/{mm/cache.c,include/asm/cacheops.h};
 * LS2K0300 processor manual v1.01 and the official product cache topology.
 */
#include "firmware_scanout.h"

#ifdef CONFIG_BOARD_LS2K0300
static uint32_t scanout_cpucfg(unsigned reg) {
    uint32_t value;
    __asm__ __volatile__("cpucfg %0, %1" : "=r"(value) : "r"(reg));
    return value;
}
static void scanout_cacheop(unsigned op, uintptr_t address) {
    if (op == 0x11)
        __asm__ __volatile__("cacop 0x11, %0, 0" ::"r"(address) : "memory");
    else if (op == 0x12)
        __asm__ __volatile__("cacop 0x12, %0, 0" ::"r"(address) : "memory");
}
static void scanout_drain(void) {
    __asm__ __volatile__("dbar 0" ::: "memory");
}
#endif

int ls2k0300_scanout_prepare(uint64_t physical, size_t bytes) {
#ifdef CONFIG_BOARD_LS2K0300
    uint32_t geometry[3] = {scanout_cpucfg(0x11), scanout_cpucfg(0x12),
                            scanout_cpucfg(0x13)};
    uint64_t cached_dmw;
    __asm__ __volatile__("csrrd %0, 0x181" : "=r"(cached_dmw));
    ls2k0300_scanout_plan_t plan;
    int rc = ls2k0300_scanout_plan(physical, bytes, scanout_cpucfg(0x10),
                                   geometry, cached_dmw, &plan);
    if (rc)
        return rc;
    ls2k0300_scanout_apply(&plan, scanout_cacheop, scanout_drain);
    return 0;
#else
    (void)physical;
    (void)bytes;
    return -EOPNOTSUPP;
#endif
}
