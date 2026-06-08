#ifdef CONFIG_X86_64

#include "core/defs.h"
#include "core/timer.h"
#include "cpu.h"
#include "platform.h"

static uint64_t timer_freq = 1000000000ULL;

void timer_init(void) {
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_TIMER_INIT, 100000);
    lapic_write(LAPIC_LVT_TIMER, IRQ_VECTOR_TIMER | LAPIC_LVT_MASKED);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    uint32_t count = (uint32_t)(ticks / 100);
    if (count < 100) count = 100;
    lapic_write(LAPIC_TIMER_INIT, count);
}

uint64_t timer_get_ticks(void) {
    uint32_t lo, hi;
    __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
    return ((uint64_t)hi << 32) | lo;
}

void timer_irq_tick(void) {}

void timer_enable(void) {
    lapic_write(LAPIC_LVT_TIMER,
        (lapic_read(LAPIC_LVT_TIMER) & ~LAPIC_LVT_MASKED) | IRQ_VECTOR_TIMER);
}

void timer_disable(void) {
    lapic_write(LAPIC_LVT_TIMER, lapic_read(LAPIC_LVT_TIMER) | LAPIC_LVT_MASKED);
}

#endif
