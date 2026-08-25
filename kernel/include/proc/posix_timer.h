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

/* Allocate a timer owned by owner_pid.  signo 0 = no notification.
 * target_tid > 0 delivers to that thread only.  Returns slot id or -1. */
int  posix_timer_create(int owner_pid, int signo, int target_tid);

/* Release a timer; only the owning process may delete it. */
int  posix_timer_delete(int owner_pid, int id);

/* Read [interval_sec, interval_nsec, value_sec, value_nsec]. */
int  posix_timer_get_time(int owner_pid, int id, uint64_t out[4]);

/* Arm from [interval_sec, interval_nsec, value_sec, value_nsec]. */
int  posix_timer_set_time(int owner_pid, int id, const uint64_t ts[4]);

int  posix_timer_getoverrun(int owner_pid, int id);

/* Expiry scan; invoked from the scheduler timer heap. */
void posix_timer_tick(void);

#endif /* _PROC_POSIX_TIMER_H */
