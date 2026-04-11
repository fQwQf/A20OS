#include "defs.h"
#include "timer.h"
#include "sbi.h"

static uint64_t timer_freq;

void timer_init(void) {
    timer_freq = CLINT_TIMER_FREQ;
    timer_set_interval(TICKS_PER_SEC / 100);
}

void timer_set_interval(uint64_t ticks) {
    uint64_t now = timer_get_ticks();
    sbi_set_timer(now + ticks);
}

uint64_t timer_get_ticks(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    return val;
}

void timer_enable(void) {
    uint64_t sie = r_sie();
    w_sie(sie | SIE_STIE);
}

void timer_disable(void) {
    uint64_t sie = r_sie();
    w_sie(sie & ~SIE_STIE);
}
