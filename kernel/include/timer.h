#ifndef _TIMER_H
#define _TIMER_H

#include "types.h"
#include "consts.h"

/* Timer frequency — provided by arch/platform.h (CLINT_TIMER_FREQ or equivalent) */
#define TICKS_PER_SEC   CLINT_TIMER_FREQ
#define MS_TO_TICKS(ms) ((uint64_t)(ms) * TICKS_PER_SEC / 1000)
#define US_TO_TICKS(us) ((uint64_t)(us) * TICKS_PER_SEC / 1000000)

void timer_init(void);
void timer_set_interval(uint64_t ticks);
uint64_t timer_get_ticks(void);
void timer_irq_tick(void);
void timer_enable(void);
void timer_disable(void);

#endif /* _TIMER_H */
