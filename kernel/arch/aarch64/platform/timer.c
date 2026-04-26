#ifdef CONFIG_AARCH64

#include "core/defs.h"
#include "core/timer.h"

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

#endif /* CONFIG_AARCH64 */
