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

void timer_init(void) {
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    __asm__ __volatile__(
        "msr cntp_tval_el0, %0\n\t"
        "mov x1, #1\n\t"
        "msr cntp_ctl_el0, x1"
        ::
            "r"(ticks)
        : "x1", "memory");
}

uint64_t timer_get_ticks(void) {
    uint64_t val;
    __asm__ __volatile__("mrs %0, cntpct_el0" : "=r"(val));
    return val;
}

void timer_irq_tick(void) {
    /* The architectural counter is free-running. */
}

void timer_enable(void) {
    uint64_t ctl = 1;
    __asm__ __volatile__("msr cntp_ctl_el0, %0" :: "r"(ctl) : "memory");
}

void timer_disable(void) {
    uint64_t ctl = 0;
    __asm__ __volatile__("msr cntp_ctl_el0, %0" :: "r"(ctl) : "memory");
}

#endif /* CONFIG_COOPERATIVE_BOOT */

#endif /* CONFIG_AARCH64 */
