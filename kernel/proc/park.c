#include "proc/park.h"

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/panic.h"

static proc_wait_token_t proc_wait_token_none(void)
{
    proc_wait_token_t token = {0};
    return token;
}

static int proc_try_wake_locked_common(task_t *task, uint64_t seq,
                                       proc_wake_reason_t reason,
                                       uint64_t *remote_cpus)
{
    if (!task || !seq || task->wait_seq != seq ||
        task->state == PROC_UNUSED || task->state == PROC_ZOMBIE)
        return 0;

    if (reason == PROC_WAKE_SIGNAL &&
        task->wait_mode == PROC_WAIT_UNINTERRUPTIBLE)
        return 0;

    switch (task->park_state) {
    case PROC_PARK_PREPARING:
        proc_wait_timer_cancel_locked(task, seq);
        task->park_state = PROC_PARK_WOKEN;
        task->wake_reason = reason;
        task->wait_deadline = 0;
        task->wake_time = 0;
        proc_sched_assert_task_locked(task);
        return 1;

    case PROC_PARK_PARKED: {
        unsigned target_cpu =
            (task->on_cpu || task->dispatching)
                ? task->owner_cpu : task->cpu_id;
        proc_wait_timer_cancel_locked(task, seq);
        task->park_state = PROC_PARK_WOKEN;
        task->wake_reason = reason;
        task->wait_deadline = 0;
        task->wake_time = 0;
        task->state = PROC_READY;
        if (task->sched_level > 0)
            task->sched_level--;
        /*
         * An on_cpu task is still executing the commit/sched boundary; a
         * dispatching task has already been selected. The runqueue helper
         * publishes only an unowned READY task. Switch completion handles a
         * READY task whose wake raced its final on_cpu interval.
         */
        proc_runq_enqueue_locked(task);
        if (task->on_rq && target_cpu != cpu_current_id()) {
            if (remote_cpus && target_cpu < 64)
                *remote_cpus |= 1ULL << target_cpu;
            else
                proc_sched_kick_cpu(target_cpu);
        }
        proc_sched_assert_task_locked(task);
        return 1;
    }

    default:
        return 0;
    }
}

proc_wait_token_t proc_park_prepare_locked(proc_wait_mode_t mode,
                                           uint64_t deadline)
{
    task_t *task = proc_current();
    if (!task || task == proc_idle_task() ||
        task->state == PROC_UNUSED || task->state == PROC_ZOMBIE)
        return proc_wait_token_none();

    /*
     * A task owns only one active park token.  A non-IDLE state here means a
     * caller forgot finish(); do not overwrite the live sequence.
     */
    if (task->park_state != PROC_PARK_IDLE) {
#if defined(CONFIG_DEBUG_KERNEL) && CONFIG_DEBUG_KERNEL
        panic("park: nested prepare pid=%d state=%d seq=%lu",
              task->pid, task->park_state, (unsigned long)task->wait_seq);
#endif
        return proc_wait_token_none();
    }

    task->wait_seq++;
    if (task->wait_seq == 0)
        task->wait_seq++;
    task->wait_deadline = deadline;
    task->wait_mode = mode;
    task->wake_reason = PROC_WAKE_NONE;
    task->park_state = PROC_PARK_PREPARING;
    if (deadline &&
        proc_wait_timer_register_locked(task, deadline,
                                        task->wait_seq) < 0) {
        task->wait_deadline = 0;
        task->park_state = PROC_PARK_IDLE;
        return proc_wait_token_none();
    }
    proc_sched_assert_task_locked(task);

    proc_wait_token_t token = {
        .task = task,
        .seq = task->wait_seq,
    };
    return token;
}

proc_wait_token_t proc_park_prepare(proc_wait_mode_t mode,
                                    uint64_t deadline)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    proc_wait_token_t token = proc_park_prepare_locked(mode, deadline);
    spin_unlock_irqrestore(&proc_lock, flags);
    return token;
}

int proc_try_wake_locked(task_t *task, uint64_t seq,
                         proc_wake_reason_t reason)
{
    return proc_try_wake_locked_common(task, seq, reason, NULL);
}

int proc_try_wake(task_t *task, uint64_t seq,
                  proc_wake_reason_t reason)
{
    uint64_t remote_cpus = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int woke = proc_try_wake_locked_common(task, seq, reason, &remote_cpus);
    spin_unlock_irqrestore(&proc_lock, flags);
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && cpu < 64; cpu++) {
        if (remote_cpus & (1ULL << cpu))
            proc_sched_kick_cpu(cpu);
    }
    return woke;
}

int proc_interrupt_wait(task_t *task, proc_wake_reason_t reason)
{
    if (!task)
        return 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int woke = proc_try_wake_locked(task, task->wait_seq, reason);
    spin_unlock_irqrestore(&proc_lock, flags);
    return woke;
}

int proc_park_cancel(proc_wait_token_t token)
{
    task_t *task = token.task;
    if (!task || !token.seq)
        return 0;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int cancelled = 0;
    if (task == proc_current() && task->wait_seq == token.seq &&
        task->park_state == PROC_PARK_PREPARING) {
        proc_wait_timer_cancel_locked(task, token.seq);
        task->wait_deadline = 0;
        task->wake_time = 0;
        task->wake_reason = PROC_WAKE_CANCEL;
        task->park_state = PROC_PARK_WOKEN;
        proc_sched_assert_task_locked(task);
        cancelled = 1;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return cancelled;
}

proc_wake_reason_t proc_park_commit(proc_wait_token_t token)
{
    task_t *task = token.task;
    if (!task || !token.seq)
        return PROC_WAKE_CANCEL;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (task != proc_current() || task->wait_seq != token.seq) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return PROC_WAKE_CANCEL;
    }

    if (task->park_state == PROC_PARK_WOKEN) {
        proc_wake_reason_t reason = task->wake_reason;
        spin_unlock_irqrestore(&proc_lock, flags);
        return reason;
    }
    if (task->park_state != PROC_PARK_PREPARING) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return PROC_WAKE_CANCEL;
    }

    task->park_state = PROC_PARK_PARKED;
    task->state = PROC_BLOCKED;
    proc_sched_assert_task_locked(task);
    spin_unlock_irqrestore(&proc_lock, flags);

    /*
     * sched() is mandatory even if a wake races immediately after the unlock.
     * While this task remains on_cpu the wake publishes READY but cannot queue
     * it. sched() either restores the current task to RUNNING or switches away;
     * switch completion then publishes the raced READY task exactly once.
     */
    sched();

    flags = spin_lock_irqsave(&proc_lock);
    proc_wake_reason_t reason =
        task->wait_seq == token.seq ? task->wake_reason : PROC_WAKE_CANCEL;
    spin_unlock_irqrestore(&proc_lock, flags);
    return reason;
}

void proc_park_finish(proc_wait_token_t token)
{
    task_t *task = token.task;
    if (!task || !token.seq)
        return;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (task->wait_seq == token.seq &&
        task->park_state == PROC_PARK_WOKEN) {
        proc_wait_timer_cancel_locked(task, token.seq);
        task->wait_deadline = 0;
        task->wake_time = 0;
        task->wait_mode = PROC_WAIT_UNINTERRUPTIBLE;
        task->wake_reason = PROC_WAKE_NONE;
        task->park_state = PROC_PARK_IDLE;
        proc_sched_assert_task_locked(task);
#if defined(CONFIG_DEBUG_KERNEL) && CONFIG_DEBUG_KERNEL
    } else if (task->wait_seq == token.seq &&
               task->park_state == PROC_PARK_PREPARING) {
        panic("park: finish without cancel/commit pid=%d seq=%lu",
              task->pid, (unsigned long)token.seq);
#endif
    }
    spin_unlock_irqrestore(&proc_lock, flags);
}

proc_wake_reason_t proc_park_wait(proc_wait_mode_t mode, uint64_t deadline)
{
    proc_wait_token_t token = proc_park_prepare(mode, deadline);
    if (!token.task)
        return PROC_WAKE_CANCEL;
    proc_wake_reason_t reason = proc_park_commit(token);
    proc_park_finish(token);
    return reason;
}

void proc_wake_q_init(proc_wake_q_t *wake_q)
{
    if (wake_q)
        wake_q->count = 0;
}

int proc_wake_q_add(proc_wake_q_t *wake_q, task_t *task,
                    uint64_t seq, proc_wake_reason_t reason)
{
    if (!wake_q || !task || !seq ||
        wake_q->count >= PROC_WAKE_Q_CAPACITY)
        return 0;
    wake_q->items[wake_q->count].task = task;
    wake_q->items[wake_q->count].seq = seq;
    wake_q->items[wake_q->count].reason = reason;
    wake_q->count++;
    return 1;
}

unsigned proc_wake_q_flush(proc_wake_q_t *wake_q)
{
    if (!wake_q || wake_q->count == 0)
        return 0;

    unsigned woke = 0;
    uint64_t remote_cpus = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (unsigned i = 0; i < wake_q->count; i++) {
        proc_wake_q_item_t *item = &wake_q->items[i];
        if (proc_try_wake_locked_common(item->task, item->seq, item->reason,
                                        &remote_cpus))
            woke++;
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    wake_q->count = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && cpu < 64; cpu++) {
        if (remote_cpus & (1ULL << cpu))
            proc_sched_kick_cpu(cpu);
    }
    return woke;
}
