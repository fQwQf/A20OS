#include "arch_ops.h"
#include "timer.h"

static uint64_t timer_freq;

void timer_init(void) {
    timer_freq = CLINT_TIMER_FREQ;
    timer_set_interval(TICKS_PER_SEC / 100);
}

void timer_set_interval(uint64_t ticks) {
    uint64_t now = timer_get_ticks();
    arch_timer_program(now + ticks);
}

uint64_t timer_get_ticks(void) {
    return arch_timer_counter();
}

void timer_enable(void) {
    arch_irq_enable_timer();
}

void timer_disable(void) {
    arch_irq_disable_timer();
}
