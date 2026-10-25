#ifdef CONFIG_ARM32

#include "core/defs.h"
#include "core/timer.h"

void timer_init(void) {
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    uint32_t v = ticks > 0x7fffffffULL ? 0x7fffffffU : (uint32_t)ticks;
    __asm__ __volatile__(
        "mcr p15, 0, %0, c14, c2, 0\n\t"
        "mov r1, #1\n\t"
        "mcr p15, 0, r1, c14, c2, 1"
        :: "r"(v)
        : "r1", "memory");
}

uint64_t timer_get_ticks(void) {
    uint32_t lo, hi;
    /* MRC c14,c0,0 is CNTFRQ (constant), not the counter. CNTPCT requires
     * an atomic 64-bit MRRC read and must not wrap at the low 32 bits. */
    __asm__ __volatile__("isb\n\tmrrc p15, 0, %0, %1, c14"
                         : "=r"(lo), "=r"(hi) :: "memory");
    return ((uint64_t)hi << 32) | lo;
}

void timer_irq_tick(void) {
}

void timer_enable(void) {
    uint32_t ctl = 1;
    __asm__ __volatile__("mcr p15, 0, %0, c14, c2, 1" :: "r"(ctl) : "memory");
}

void timer_disable(void) {
    uint32_t ctl = 0;
    __asm__ __volatile__("mcr p15, 0, %0, c14, c2, 1" :: "r"(ctl) : "memory");
}

#endif
