/*
 * A20OS Native SDK — Clock / Timer.
 */
#ifndef _A20_CLOCK_H
#define _A20_CLOCK_H

#include "a20_types.h"
#include "a20_syscall.h"

#define A20_CLOCK_MONOTONIC  0
#define A20_CLOCK_REALTIME   1

static inline a20_status_t a20_clock_get(uint32_t clock_id, a20_time_t *out)
{
    return a20_syscall6(A20_SYS_clock_get, clock_id, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_clock_resolution(uint32_t clock_id, a20_time_t *out)
{
    return a20_syscall6(A20_SYS_clock_resolution, clock_id, (uint64_t)out, 0, 0, 0, 0);
}

/* ---- Timers ---- */

static inline a20_status_t a20_timer_create(uint32_t clock_id, a20_handle_t *out)
{
    return a20_syscall6(A20_SYS_timer_create, clock_id, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_timer_set(a20_handle_t timer, a20_time_t deadline,
                                          a20_time_t interval)
{
    return a20_syscall6(A20_SYS_timer_set, timer,
                        deadline.secs, deadline.nsecs,
                        interval.secs, interval.nsecs, 0);
}

static inline a20_status_t a20_timer_cancel(a20_handle_t timer)
{
    return a20_syscall6(A20_SYS_timer_cancel, timer, 0, 0, 0, 0, 0);
}

#endif
