#ifndef _PROC_POSIX_TIMER_H
#define _PROC_POSIX_TIMER_H

#include <stdint.h>

/*
 * Kernel-internal POSIX interval-timer table (kernel/proc/timer_posix.c).
 *
 * Timer objects and their tick-driven expiry belong to the process/timer
 * subsystem; the Linux ABI layer only decodes timer_create/settime wire
 * structs onto these calls.  proc/timer_heap.c drives posix_timer_tick().
 */

struct task_t;
typedef struct task_t task_t;

/* Is clockid a supported timer_create() clock? */
int  posix_timer_clock_supported(int clockid);

/* Allocate a timer owned by owner_pid.  signo 0 = no notification.
 * target_tid > 0 delivers to that thread only.  Returns slot id or -1. */
int  posix_timer_create(int owner_pid, int signo, int target_tid,
                        int clockid);

/* Release a timer; only the owning process may delete it. */
int  posix_timer_delete(int owner_pid, int id);

/* Read [interval_sec, interval_nsec, value_sec, value_nsec]; value is the
 * time REMAINING until expiry (Linux semantics), zero when disarmed. */
int  posix_timer_get_time(int owner_pid, int id, uint64_t out[4]);

/* Arm from [interval_sec, interval_nsec, value_sec, value_nsec].
 * flags bit 0 = TIMER_ABSTIME (value is absolute on the timer's clock);
 * value == 0 disarms. */
int  posix_timer_set_time(int owner_pid, int id, const uint64_t ts[4],
                          int flags);

int  posix_timer_getoverrun(int owner_pid, int id);

/* Expiry scan; invoked from the scheduler timer heap. */
void posix_timer_tick(void);

/* CPU-time itimers (ITIMER_VIRTUAL = 0 / ITIMER_PROF = 1).
 * posix_itimer_cpu_tick() is invoked from the scheduler tick accounting
 * site for the current task.  value/interval are in scheduler accounting
 * ticks (nominal 100 Hz), the unit of task->utime_ticks/stime_ticks. */
int  posix_itimer_set(task_t *t, int which, uint64_t value_ticks,
                      uint64_t interval_ticks);
int  posix_itimer_get(task_t *t, int which, uint64_t out[2]);
void posix_itimer_cpu_tick(task_t *cur);

#endif /* _PROC_POSIX_TIMER_H */
