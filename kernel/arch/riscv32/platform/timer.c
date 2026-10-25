#ifdef CONFIG_RISCV32

#include "core/defs.h"
#include "core/timer.h"
#include "firmware.h"

static uint64_t timer_freq;

static uint64_t aclint_mtime_read(void) {
    uint32_t lo, hi, again;
    do {
        __asm__ volatile("csrr %0, timeh" : "=r"(hi));
        __asm__ volatile("csrr %0, time" : "=r"(lo));
        __asm__ volatile("csrr %0, timeh" : "=r"(again));
    } while (hi != again);
    return ((uint64_t)hi << 32) | lo;
}

void timer_init(void) {
    timer_freq = ARCH_TIMER_FREQ;
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    uint64_t now = timer_get_ticks();
    (void)timer_freq;
    firmware_set_timer(now + ticks);
}

uint64_t timer_get_ticks(void) {
    return aclint_mtime_read();
}

void timer_irq_tick(void) {
}

void timer_enable(void) {
    arch_write_sie(arch_read_sie() | SIE_STIE);
}

void timer_disable(void) {
    arch_write_sie(arch_read_sie() & ~SIE_STIE);
}

#endif /* CONFIG_RISCV32 */
