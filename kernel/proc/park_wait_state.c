#include "proc/park_wait_state.h"

#include <stddef.h>

static int fields_valid(const a20_park_wait_fields_t *f)
{
    return f && f->wait_seq && f->wait_deadline && f->park_state &&
           f->wait_mode && f->wake_reason;
}

static int reason_allowed(const a20_park_wait_fields_t *f, int reason)
{
    if (reason == A20_WAKE_SIGNAL &&
        __atomic_load_n(f->wait_mode, __ATOMIC_RELAXED) !=
            A20_WAIT_INTERRUPTIBLE)
        return 0;
    if ((reason == A20_WAKE_FATAL_SIGNAL || reason == A20_WAKE_TASK_EXIT) &&
        __atomic_load_n(f->wait_mode, __ATOMIC_RELAXED) ==
            A20_WAIT_UNINTERRUPTIBLE)
        return 0;
    return 1;
}

void a20_c_park_wait_init(a20_park_wait_fields_t *f)
{
    if (!fields_valid(f))
        return;
    __atomic_store_n(f->wait_seq, 0, __ATOMIC_RELAXED);
    __atomic_store_n(f->wait_deadline, 0, __ATOMIC_RELAXED);
    __atomic_store_n(f->park_state, A20_PARK_IDLE, __ATOMIC_RELAXED);
    __atomic_store_n(f->wait_mode, A20_WAIT_UNINTERRUPTIBLE,
                     __ATOMIC_RELAXED);
    __atomic_store_n(f->wake_reason, A20_WAKE_NONE, __ATOMIC_RELAXED);
}

int a20_c_park_wait_prepare(a20_park_wait_fields_t *f, int mode,
                            uint64_t deadline, uint64_t *seq_out)
{
    if (!fields_valid(f) || !seq_out || mode < A20_WAIT_UNINTERRUPTIBLE ||
        mode > A20_WAIT_KILLABLE ||
        __atomic_load_n(f->park_state, __ATOMIC_RELAXED) != A20_PARK_IDLE)
        return -1;
    uint64_t seq = __atomic_load_n(f->wait_seq, __ATOMIC_RELAXED) + 1;
    if (!seq)
        seq++;
    __atomic_store_n(f->wait_seq, seq, __ATOMIC_RELEASE);
    __atomic_store_n(f->wait_deadline, deadline, __ATOMIC_RELAXED);
    __atomic_store_n(f->wait_mode, mode, __ATOMIC_RELAXED);
    __atomic_store_n(f->wake_reason, A20_WAKE_NONE, __ATOMIC_RELAXED);
    __atomic_store_n(f->park_state, A20_PARK_PREPARING, __ATOMIC_RELAXED);
    *seq_out = seq;
    return 0;
}

void a20_c_park_wait_prepare_rollback(a20_park_wait_fields_t *f,
                                      uint64_t seq)
{
    if (!fields_valid(f) || !seq ||
        __atomic_load_n(f->wait_seq, __ATOMIC_RELAXED) != seq ||
        __atomic_load_n(f->park_state, __ATOMIC_RELAXED) !=
            A20_PARK_PREPARING)
        return;
    __atomic_store_n(f->wait_deadline, 0, __ATOMIC_RELAXED);
    __atomic_store_n(f->wait_mode, A20_WAIT_UNINTERRUPTIBLE,
                     __ATOMIC_RELAXED);
    __atomic_store_n(f->wake_reason, A20_WAKE_NONE, __ATOMIC_RELAXED);
    __atomic_store_n(f->park_state, A20_PARK_IDLE, __ATOMIC_RELAXED);
}

int a20_c_park_wait_wake(a20_park_wait_fields_t *f, uint64_t seq,
                         int reason, a20_park_timer_cancel_fn cancel_timer,
                         void *context)
{
    if (!fields_valid(f) || !seq ||
        __atomic_load_n(f->wait_seq, __ATOMIC_RELAXED) != seq ||
        !reason_allowed(f, reason))
        return 0;
    int state = __atomic_load_n(f->park_state, __ATOMIC_RELAXED);
    int result = state == A20_PARK_PREPARING ? 1 :
                 state == A20_PARK_PARKED ? 2 : 0;
    if (!result)
        return 0;
    if (cancel_timer)
        cancel_timer(context, seq);
    __atomic_store_n(f->park_state, A20_PARK_WOKEN, __ATOMIC_RELAXED);
    __atomic_store_n(f->wake_reason, reason, __ATOMIC_RELEASE);
    __atomic_store_n(f->wait_deadline, 0, __ATOMIC_RELAXED);
    return result;
}

int a20_c_park_wait_cancel(a20_park_wait_fields_t *f, uint64_t seq,
                           a20_park_timer_cancel_fn cancel_timer,
                           void *context)
{
    if (!fields_valid(f) || !seq ||
        __atomic_load_n(f->wait_seq, __ATOMIC_RELAXED) != seq ||
        __atomic_load_n(f->park_state, __ATOMIC_RELAXED) !=
            A20_PARK_PREPARING)
        return 0;
    if (cancel_timer)
        cancel_timer(context, seq);
    __atomic_store_n(f->wait_deadline, 0, __ATOMIC_RELAXED);
    __atomic_store_n(f->wake_reason, A20_WAKE_CANCEL, __ATOMIC_RELEASE);
    __atomic_store_n(f->park_state, A20_PARK_WOKEN, __ATOMIC_RELAXED);
    return 1;
}

int a20_c_park_wait_commit(a20_park_wait_fields_t *f, uint64_t seq,
                           int *reason_out)
{
    if (!fields_valid(f) || !seq || !reason_out ||
        __atomic_load_n(f->wait_seq, __ATOMIC_RELAXED) != seq)
        return -1;
    int state = __atomic_load_n(f->park_state, __ATOMIC_RELAXED);
    if (state == A20_PARK_WOKEN) {
        *reason_out = __atomic_load_n(f->wake_reason, __ATOMIC_ACQUIRE);
        return 0;
    }
    if (state != A20_PARK_PREPARING)
        return -1;
    __atomic_store_n(f->park_state, A20_PARK_PARKED, __ATOMIC_RELAXED);
    return 1;
}

int a20_c_park_wait_finish(a20_park_wait_fields_t *f, uint64_t seq,
                           int timer_pending,
                           a20_park_timer_cancel_fn cancel_timer,
                           void *context)
{
    if (!fields_valid(f) || !seq ||
        __atomic_load_n(f->wait_seq, __ATOMIC_RELAXED) != seq ||
        __atomic_load_n(f->park_state, __ATOMIC_RELAXED) != A20_PARK_WOKEN)
        return 0;
    if (timer_pending && cancel_timer)
        cancel_timer(context, seq);
    __atomic_store_n(f->wait_deadline, 0, __ATOMIC_RELAXED);
    __atomic_store_n(f->wait_mode, A20_WAIT_UNINTERRUPTIBLE,
                     __ATOMIC_RELAXED);
    __atomic_store_n(f->wake_reason, A20_WAKE_NONE, __ATOMIC_RELAXED);
    __atomic_store_n(f->park_state, A20_PARK_IDLE, __ATOMIC_RELEASE);
    return 1;
}

int a20_c_park_wait_reason(const a20_park_wait_fields_t *f, uint64_t seq)
{
    if (!fields_valid(f) || !seq ||
        __atomic_load_n(f->wait_seq, __ATOMIC_ACQUIRE) != seq)
        return A20_WAKE_CANCEL;
    return __atomic_load_n(f->wake_reason, __ATOMIC_ACQUIRE);
}
