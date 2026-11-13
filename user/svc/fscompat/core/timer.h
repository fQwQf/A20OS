/* Clock adapter for kernel filesystem code compiled into the Native service. */
#ifndef A20_FSCOMPAT_TIMER_H
#define A20_FSCOMPAT_TIMER_H
#include "a20_clock.h"
#define TICKS_PER_SEC 1000000000ULL
static inline uint64_t timer_get_ticks(void)
{
    a20_time_ns_t now = 0;
    (void)a20_clock_get(A20_CLOCK_MONOTONIC, &now);
    return now;
}
#endif
