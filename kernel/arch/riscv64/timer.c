#ifdef CONFIG_RISCV64

#include "defs.h"
#include "timer.h"
#include "firmware.h"

static uint64_t timer_freq;

void timer_init(void) {
    timer_freq = CLINT_TIMER_FREQ;
    timer_set_interval(TICKS_PER_SEC / 100);
    timer_enable();
}

void timer_set_interval(uint64_t ticks) {
    uint64_t now = timer_get_ticks();
    (void)timer_freq;
    firmware_set_timer(now + ticks);
}

uint64_t timer_get_ticks(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    return val;
}

void timer_irq_tick(void) {
    /* RISC-V already uses the monotonic hardware time CSR. */
}

void timer_enable(void) {
    uint64_t sie = arch_read_sie();
    arch_write_sie(sie | SIE_STIE);
}

void timer_disable(void) {
    uint64_t sie = arch_read_sie();
    arch_write_sie(sie & ~SIE_STIE);
}

#endif /* CONFIG_RISCV64 */
