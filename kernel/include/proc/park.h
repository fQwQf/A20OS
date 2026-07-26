#ifndef _PROC_PARK_H
#define _PROC_PARK_H

#include "core/types.h"

struct task_t;

typedef enum {
    PROC_PARK_IDLE = 0,
    PROC_PARK_PREPARING,
    PROC_PARK_PARKED,
    PROC_PARK_WOKEN,
} proc_park_state_t;

typedef enum {
    PROC_WAKE_NONE = 0,
    PROC_WAKE_EVENT,
    PROC_WAKE_TIMEOUT,
    PROC_WAKE_SIGNAL,
    PROC_WAKE_EXIT,
    PROC_WAKE_CANCEL,
} proc_wake_reason_t;

typedef enum {
    PROC_WAIT_UNINTERRUPTIBLE = 0,
    PROC_WAIT_INTERRUPTIBLE,
    PROC_WAIT_KILLABLE,
} proc_wait_mode_t;

typedef struct proc_wait_token {
    struct task_t *task;
    uint64_t seq;
} proc_wait_token_t;

#define PROC_WAKE_Q_CAPACITY 64

typedef struct proc_wake_q_item {
    struct task_t *task;
    uint64_t seq;
    proc_wake_reason_t reason;
} proc_wake_q_item_t;

typedef struct proc_wake_q {
    proc_wake_q_item_t items[PROC_WAKE_Q_CAPACITY];
    unsigned count;
} proc_wake_q_t;

/*
 * A20_PARK_WAKE_PROTOCOL:
 *
 * PREPARING keeps the current task RUNNING and off every runqueue.  An early
 * wake changes only park_state to WOKEN.  Commit either consumes that early
 * wake without scheduling, or atomically publishes PARKED + PROC_BLOCKED.
 * Every wake carries wait_seq, so a delayed event or timeout cannot wake a
 * later wait by the same task.
 *
 * A token belongs to one logical wait, not to one wait queue.  The caller may
 * link the same token into several wait queues.  prepare/cancel/commit/finish
 * must never be called while an object or wait-queue lock is held; queue
 * link/unlink operations do not acquire proc_lock.
 */
proc_wait_token_t proc_park_prepare(proc_wait_mode_t mode,
                                    uint64_t deadline);
proc_wait_token_t proc_park_prepare_locked(proc_wait_mode_t mode,
                                           uint64_t deadline);
int proc_park_cancel(proc_wait_token_t token);
proc_wake_reason_t proc_park_commit(proc_wait_token_t token);
void proc_park_finish(proc_wait_token_t token);
proc_wake_reason_t proc_park_wait(proc_wait_mode_t mode, uint64_t deadline);

int proc_try_wake(struct task_t *task, uint64_t seq,
                  proc_wake_reason_t reason);
int proc_try_wake_locked(struct task_t *task, uint64_t seq,
                         proc_wake_reason_t reason);
int proc_interrupt_wait(struct task_t *task, proc_wake_reason_t reason);

void proc_wake_q_init(proc_wake_q_t *wake_q);
int proc_wake_q_add(proc_wake_q_t *wake_q, struct task_t *task,
                    uint64_t seq, proc_wake_reason_t reason);
unsigned proc_wake_q_flush(proc_wake_q_t *wake_q);

#endif
