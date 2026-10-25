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

#endif /* _TIMER_H */
