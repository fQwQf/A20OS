#ifdef CONFIG_AARCH64

#include "core/defs.h"
#include "core/timer.h"

#ifdef CONFIG_COOPERATIVE_BOOT
/*
 * VBox's ARM firmware runs this payload at EL1 but traps both CNTP_* and
 * CNTV_* accesses (ESR_EL1.EC=0x00).  Do not touch either interface during
 * serial bring-up.  The monotonic counter is deliberately software-only:
 * it lets initialization and polling-only paths make progress, but does not
 * claim wall-clock accuracy or preemptive scheduling until a usable VBox
 * timer device has been discovered.
 */
static uint64_t vbox_soft_ticks;

void timer_init(void) {
}

void timer_set_interval(uint64_t ticks) {
    (void)ticks;
}

uint64_t timer_get_ticks(void) {
    return vbox_soft_ticks++;
}

void timer_irq_tick(void) {
    vbox_soft_ticks++;
}

void timer_enable(void) {
}

void timer_disable(void) {
}

#else

uint64_t aarch64_timer_freq(void) {
    uint64_t frequency;
    __asm__ __volatile__("mrs %0, cntfrq_el0" : "=r"(frequency));
    return frequency;
}

void timer_init(void) {
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
#ifdef AARCH64_TIMER_ERRATUM_SUN50I_A64
    /* TVAL uses the same unstable counter internally. Compute the deadline
     * from the filtered read, as upstream's sun50i A64 workaround does. */
    uint64_t deadline = timer_get_ticks() + ticks;
    __asm__ __volatile__("msr cntp_cval_el0, %0\n\tisb" :: "r"(deadline) : "memory");
    timer_enable();
#else
    __asm__ __volatile__("msr cntp_tval_el0, %0\n\t"
                         "mov x1, #1\n\t"
                         "msr cntp_ctl_el0, x1" ::"r"(ticks)
                         : "x1", "memory");
#endif
}

uint64_t timer_get_ticks(void) {
    uint64_t val;
#ifdef AARCH64_TIMER_ERRATUM_SUN50I_A64
    /* A64's counter can glitch at carries. Reject low-nine-bit all-zero or
     * all-one samples; 150 attempts bounds the wait (Linux v6.17
     * drivers/clocksource/arm_arch_timer.c, sun50i UNKNOWN1 workaround). */
    for (unsigned retry = 0; retry < 150; retry++) {
        __asm__ __volatile__("isb\n\tmrs %0, cntpct_el0" : "=r"(val) :: "memory");
        if (((val + 1) & 0x1ffUL) > 1)
            return val;
    }
#else
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(val));
#endif
    return val;
}

void timer_irq_tick(void) {
    /* The architectural counter is free-running. */
}

void timer_enable(void) {
    uint64_t ctl = 1;
    __asm__ __volatile__("msr cntp_ctl_el0, %0" ::"r"(ctl) : "memory");
}

void timer_disable(void) {
    uint64_t ctl = 0;
    __asm__ __volatile__("msr cntp_ctl_el0, %0" ::"r"(ctl) : "memory");
}

#endif /* CONFIG_COOPERATIVE_BOOT */

#endif /* CONFIG_AARCH64 */
