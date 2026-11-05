#ifndef _TIMER_H
#define _TIMER_H

#include "core/types.h"
#include "core/consts.h"

/* Architectures with a firmware-dependent timebase publish a runtime
 * frequency callback; other targets use their platform constant. */
#ifdef CONFIG_DRIVER_MODULE
/* Modules use the exported clock contract, including inline lock diagnostics;
 * architecture-private frequency callbacks are not part of their ABI. */
uint64_t clock_ticks_per_sec(void);
#define TICKS_PER_SEC clock_ticks_per_sec()
#elif ARCH_TIMER_FREQ_RUNTIME
uint64_t arch_timer_frequency(void);
#define TICKS_PER_SEC   arch_timer_frequency()
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

#ifdef CONFIG_AARCH64
/*
 * Timer IRQ entry latency: the elapsed counter cycles between the compare
 * value that fired and the first instruction of the timer handler.  Samples
 * cover timer interrupts only, not device IRQs.  Per-CPU writers are the
 * local IRQ path; readers aggregate, so a concurrent update may produce a
 * mixed snapshot (diagnostic only).
 */
void timer_irq_latency_sample(uint64_t deadline, uint64_t now);
int timer_irq_latency_read(uint64_t *out_min, uint64_t *out_avg,
                           uint64_t *out_max, uint64_t *out_count);
#endif

#endif /* _TIMER_H */
