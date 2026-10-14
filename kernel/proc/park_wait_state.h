#ifndef _PROC_PARK_WAIT_STATE_H
#define _PROC_PARK_WAIT_STATE_H

#include <stdint.h>

/* A narrow view over the single logical wait fields already embedded in
 * task_t. It contains pointers, not mirrored state. The timer indices remain
 * outside this view and are owned by timer_heap.c. */
typedef struct a20_park_wait_fields {
    uint64_t *wait_seq;
    uint64_t *wait_deadline;
    int *park_state;
    int *wait_mode;
    int *wake_reason;
} a20_park_wait_fields_t;

typedef void (*a20_park_timer_cancel_fn)(void *context, uint64_t seq);

/* Narrow atomic64 adapters used by the Rust park state machine on RV32. */
uint64_t a20_park_wait_load_u64_relaxed(uint64_t *ptr);
uint64_t a20_park_wait_load_u64_acquire(uint64_t *ptr);
void a20_park_wait_store_u64_relaxed(uint64_t *ptr, uint64_t value);
void a20_park_wait_store_u64_release(uint64_t *ptr, uint64_t value);

enum {
    A20_PARK_IDLE = 0,
    A20_PARK_PREPARING = 1,
    A20_PARK_PARKED = 2,
    A20_PARK_WOKEN = 3,
};

enum {
    A20_WAIT_UNINTERRUPTIBLE = 0,
    A20_WAIT_INTERRUPTIBLE = 1,
    A20_WAIT_KILLABLE = 2,
};

enum {
    A20_WAKE_NONE = 0,
    A20_WAKE_TIMEOUT = 2,
    A20_WAKE_SIGNAL = 4,
    A20_WAKE_FATAL_SIGNAL = 5,
    A20_WAKE_TASK_EXIT = 6,
    A20_WAKE_CANCEL = 8,
};

/* Lock/context contract:
 * - init is called only before the task is published; it needs no park_lock.
 * - prepare, prepare_rollback, wake, cancel, and commit require park_lock.
 * - finish requires a matching WOKEN token. For timer_pending=1, the caller
 *   holds park_lock and supplies the timer-cancel callback. For timer_pending=0,
 *   the current task has no registered timer and finish is lock-free.
 * - reason is the post-sched lock-free read and uses atomic-compatible access.
 *
 * Every field pointer is valid, correctly aligned, and refers to the one set
 * of task fields (not mirrored storage). The callback runs with park_lock held,
 * takes only the timer-heap lock, and must not sleep or re-enter park logic.
 * The finish caller clears wake_time before this API release-publishes IDLE;
 * observers that acquire-load IDLE therefore see the cleared wake_time. */
void a20_c_park_wait_init(a20_park_wait_fields_t *fields);
int a20_c_park_wait_prepare(a20_park_wait_fields_t *fields, int mode,
                            uint64_t deadline, uint64_t *seq_out);
void a20_c_park_wait_prepare_rollback(a20_park_wait_fields_t *fields,
                                      uint64_t seq);
/* Returns 0 for no transition, 1 for PREPARING->WOKEN, 2 for
 * PARKED->WOKEN. Callback occurs only after validation and before publish. */
int a20_c_park_wait_wake(a20_park_wait_fields_t *fields, uint64_t seq,
                         int reason, a20_park_timer_cancel_fn cancel_timer,
                         void *context);
int a20_c_park_wait_cancel(a20_park_wait_fields_t *fields, uint64_t seq,
                           a20_park_timer_cancel_fn cancel_timer,
                           void *context);
/* Returns 0 with an early wake reason, 1 after publishing PARKED, or -1 for
 * stale/invalid tokens. */
int a20_c_park_wait_commit(a20_park_wait_fields_t *fields, uint64_t seq,
                           int *reason_out);
int a20_c_park_wait_finish(a20_park_wait_fields_t *fields, uint64_t seq,
                           int timer_pending,
                           a20_park_timer_cancel_fn cancel_timer,
                           void *context);
int a20_c_park_wait_reason(const a20_park_wait_fields_t *fields,
                           uint64_t seq);

void a20_rust_park_wait_init(a20_park_wait_fields_t *fields);
int a20_rust_park_wait_prepare(a20_park_wait_fields_t *fields, int mode,
                               uint64_t deadline, uint64_t *seq_out);
void a20_rust_park_wait_prepare_rollback(a20_park_wait_fields_t *fields,
                                         uint64_t seq);
int a20_rust_park_wait_wake(a20_park_wait_fields_t *fields, uint64_t seq,
                            int reason,
                            a20_park_timer_cancel_fn cancel_timer,
                            void *context);
int a20_rust_park_wait_cancel(a20_park_wait_fields_t *fields, uint64_t seq,
                              a20_park_timer_cancel_fn cancel_timer,
                              void *context);
int a20_rust_park_wait_commit(a20_park_wait_fields_t *fields, uint64_t seq,
                              int *reason_out);
int a20_rust_park_wait_finish(a20_park_wait_fields_t *fields, uint64_t seq,
                              int timer_pending,
                              a20_park_timer_cancel_fn cancel_timer,
                              void *context);
int a20_rust_park_wait_reason(const a20_park_wait_fields_t *fields,
                              uint64_t seq);

#endif
