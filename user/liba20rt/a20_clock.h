/*
 * A20OS Native SDK — Clock / Timer.
 */
#ifndef _A20_CLOCK_H
#define _A20_CLOCK_H

#include "a20_types.h"
#include "a20_syscall.h"

#define A20_CLOCK_REALTIME   0
#define A20_CLOCK_MONOTONIC  1

static inline a20_status_t a20_clock_get(uint32_t clock_id, a20_time_ns_t *out_ns)
{
    return a20_syscall6(A20_SYS_clock_get, clock_id, (uint64_t)out_ns,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_clock_resolution(uint32_t clock_id,
                                                a20_time_ns_t *out_ns)
{
    return a20_syscall6(A20_SYS_clock_resolution, clock_id,
                        (uint64_t)out_ns, 0, 0, 0, 0);
}

/* ---- Timers ---- */

static inline a20_status_t a20_timer_create(a20_handle_t event_queue,
                                             uint64_t user_data,
                                             uint32_t flags,
                                             a20_handle_t *out_timer)
{
    a20_timer_create_args_t args;

    args.size       = sizeof(args);
    args.version    = 1;
    args.event_queue = event_queue;
    args.user_data  = user_data;
    args.flags      = flags;
    args.out_timer  = A20_HANDLE_NULL;

    a20_status_t r = a20_syscall6(A20_SYS_timer_create, (uint64_t)&args,
                                    0, 0, 0, 0, 0);
    if (a20_status_is_ok(r)) {
        *out_timer = args.out_timer;
    }
    return r;
}

static inline a20_status_t a20_timer_set(a20_handle_t timer,
                                          a20_time_t deadline,
                                          a20_time_t interval)
{
    uint64_t deadline_ns = deadline.secs * 1000000000ULL + deadline.nsecs;
    uint64_t interval_ns = interval.secs * 1000000000ULL + interval.nsecs;

    return a20_syscall6(A20_SYS_timer_set, timer,
                        deadline_ns, interval_ns,
                        0, 0, 0);
}

static inline a20_status_t a20_timer_cancel(a20_handle_t timer)
{
    return a20_syscall6(A20_SYS_timer_cancel, timer, 0, 0, 0, 0, 0);
}

#endif
