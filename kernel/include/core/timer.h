#ifndef _TIMER_H
#define _TIMER_H

#include "core/types.h"
#include "core/consts.h"

/* Timer frequency.  Most arches provide the compile-time ARCH_TIMER_FREQ
 * via arch/platform.h.  riscv64 resolves the timebase at runtime from the
 * firmware DTB (QEMU virt = 10 MHz, StarFive JH7110 = 24 MHz) so the same
 * kernel image keeps correct wall-clock and scheduling intervals on both. */
#if defined(CONFIG_RISCV64)
uint64_t riscv64_timer_freq(void);
#define TICKS_PER_SEC   riscv64_timer_freq()
#else
#define TICKS_PER_SEC   ARCH_TIMER_FREQ
#endif
#define MS_TO_TICKS(ms) ((uint64_t)(ms) * TICKS_PER_SEC / 1000)
#define US_TO_TICKS(us) ((uint64_t)(us) * TICKS_PER_SEC / 1000000)

void timer_init(void);
void timer_set_interval(uint64_t ticks);
uint64_t timer_get_ticks(void);
void timer_irq_tick(void);
void timer_enable(void);
void timer_disable(void);

#endif /* _TIMER_H */
