#include "proc/park.h"

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/lifetime.h"
#include "proc/signal.h"
#include "core/cpu.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/timer.h"

static proc_wait_token_t
proc_wait_token_none(proc_park_prepare_error_t prepare_error)
{
    proc_wait_token_t token = {
        .prepare_error = prepare_error,
    };
    return token;
}

static int proc_try_wake_locked_common(task_t *task, uint64_t seq,
                                       proc_wake_reason_t reason,
                                       uint64_t *remote_cpus,
                                       uint64_t *priority_cpus)
{
    if (!task || !seq || task->wait_seq != seq ||
        task->state == PROC_UNUSED || task->state == PROC_ZOMBIE)
        return 0;

    /* PARK_SIGNAL_MODE_PROTOCOL */
    if (reason == PROC_WAKE_SIGNAL &&
        task->wait_mode != PROC_WAIT_INTERRUPTIBLE)
        return 0;
    if ((reason == PROC_WAKE_FATAL_SIGNAL ||
         reason == PROC_WAKE_TASK_EXIT) &&
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
        if (task->on_rq) {
            int priority_preempt =
                proc_sched_should_preempt_locked(task, target_cpu);
            if (target_cpu != cpu_current_id()) {
                if (remote_cpus && target_cpu < 64) {
                    *remote_cpus |= 1ULL << target_cpu;
                    if (priority_preempt && priority_cpus)
                        *priority_cpus |= 1ULL << target_cpu;
                } else {
                    proc_sched_request_cpu(target_cpu, priority_preempt);
                }
            } else if (priority_preempt) {
                proc_sched_request_cpu(target_cpu, 1);
            }
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
        return proc_wait_token_none(PROC_PARK_PREPARE_INVALID);

    /*
     * A task owns only one active park token.  A non-IDLE state here means a
     * caller forgot finish(); do not overwrite the live sequence.
     */
    if (task->park_state != PROC_PARK_IDLE) {
#if defined(CONFIG_DEBUG_KERNEL) && CONFIG_DEBUG_KERNEL
        panic("park: nested prepare pid=%d state=%d seq=%lu",
              task->pid, task->park_state, (unsigned long)task->wait_seq);
#endif
        return proc_wait_token_none(PROC_PARK_PREPARE_INVALID);
    }

    task->wait_seq++;
    if (task->wait_seq == 0)
        task->wait_seq++;
    task->wait_deadline = deadline;
    task->wait_mode = mode;
    task->wake_reason = PROC_WAKE_NONE;
    task->park_state = PROC_PARK_PREPARING;
    int timer_result = deadline
        ? proc_wait_timer_register_locked(task, deadline, task->wait_seq)
        : PROC_PARK_PREPARE_OK;
    if (timer_result != PROC_PARK_PREPARE_OK) {
        task->wait_deadline = 0;
        task->wake_time = 0;
        task->wake_reason = PROC_WAKE_NONE;
        task->wait_mode = PROC_WAIT_UNINTERRUPTIBLE;
        task->park_state = PROC_PARK_IDLE;
        return proc_wait_token_none(
            (proc_park_prepare_error_t)timer_result);
    }
    proc_sched_assert_task_locked(task);

    proc_wait_token_t token = {
        .task = task,
        .seq = task->wait_seq,
        .prepare_error = PROC_PARK_PREPARE_OK,
    };

    /*
     * Close the "signal/exit already pending before prepare" side of the
     * publication race.  A sender which queues after these checks observes
     * PREPARING/PARKED and wins the same sequence through proc_try_wake().
     */
    if (__atomic_load_n(&task->exit_pending, __ATOMIC_ACQUIRE) &&
        mode != PROC_WAIT_UNINTERRUPTIBLE) {
        (void)proc_try_wake_locked_common(
            task, token.seq, PROC_WAKE_TASK_EXIT, NULL, NULL);
    } else if (mode == PROC_WAIT_KILLABLE) {
        if (signal_task_has_fatal(task))
            (void)proc_try_wake_locked_common(
                task, token.seq, PROC_WAKE_FATAL_SIGNAL, NULL, NULL);
    } else if (mode == PROC_WAIT_INTERRUPTIBLE) {
        if (signal_task_has_fatal(task))
            (void)proc_try_wake_locked_common(
                task, token.seq, PROC_WAKE_FATAL_SIGNAL, NULL, NULL);
        else if (signal_task_has_unblocked(task))
            (void)proc_try_wake_locked_common(
                task, token.seq, PROC_WAKE_SIGNAL, NULL, NULL);
    }
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
    return proc_try_wake_locked_common(task, seq, reason, NULL, NULL);
}

int proc_try_wake(task_t *task, uint64_t seq,
                  proc_wake_reason_t reason)
{
    uint64_t remote_cpus = 0;
    uint64_t priority_cpus = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int woke = proc_try_wake_locked_common(
        task, seq, reason, &remote_cpus, &priority_cpus);
    spin_unlock_irqrestore(&proc_lock, flags);
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && cpu < 64; cpu++) {
        if (remote_cpus & (1ULL << cpu)) {
            proc_sched_request_cpu(
                cpu, (priority_cpus & (1ULL << cpu)) != 0);
        }
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

/*
 * proc_park_commit_donate — time-slice donation for synchronous IPC
 * (docs/hybrid-kernel/02-mainstream-plan.md M1).
 *
 * Identical to proc_park_commit up to marking the caller BLOCKED, but
 * instead of a full runqueue sched() it hands the CPU directly to
 * @donate_to when the target is fully parked on this CPU (the classic
 * L4 direct switch: the server works on the donor's behalf).  All
 * ineligible cases fall back to the normal sched() path, so this is
 * always safe: donation depth is implicitly one because the donor is
 * BLOCKED (not runnable) until the target or the normal scheduler runs
 * it again.
 */
proc_wake_reason_t proc_park_commit_donate(proc_wait_token_t token,
                                           task_t *donate_to)
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

    unsigned cur_cpu = cpu_current_id();
    int can_donate =
#if CONFIG_NR_CPUS > 1
        /* The donated task is switched to without a runqueue selection and
         * without the IPI/reschedule bookkeeping the SMP scheduler requires
         * (kernel/proc/current.c: PER_CPU_CURRENT_VALIDATION).  The donation
         * fast path is therefore UP-only; SMP always takes the verified
         * normal park/wake path. */
        0 &&
#endif
        donate_to && donate_to != task &&
        donate_to->park_state == PROC_PARK_PARKED &&
        donate_to->state == PROC_BLOCKED &&
        !donate_to->on_cpu && !donate_to->on_rq &&
        !donate_to->dispatching &&
        donate_to->cpu_id == cur_cpu;

    if (!can_donate) {
        proc_sched_assert_task_locked(task);
        spin_unlock_irqrestore(&proc_lock, flags);
        sched();
        goto out_reason;
    }

    /* Manual wake mirroring proc_try_wake_locked_common's PARKED branch,
     * then hand over the CPU with a dispatch reference as if the target
     * had been picked from the runqueue. */
    {
        uint64_t now = timer_get_ticks();
        proc_wait_timer_cancel_locked(donate_to, donate_to->wait_seq);
        donate_to->park_state = PROC_PARK_WOKEN;
        donate_to->wake_reason = PROC_WAKE_EVENT;
        donate_to->wait_deadline = 0;
        donate_to->wake_time = 0;
        donate_to->state = PROC_READY;
        if (donate_to->sched_level > 0)
            donate_to->sched_level--;
        donate_to->exec_start = now;
        donate_to->eevdf_last_account = now;
        proc_get(donate_to); /* dispatch ref, consumed by context_switch */
        donate_to->dispatching = 1;
        donate_to->owner_cpu = cur_cpu;
        spin_unlock_irqrestore(&proc_lock, flags);
        context_switch(donate_to);
    }

out_reason:
    flags = spin_lock_irqsave(&proc_lock);
    proc_wake_reason_t reason =
        task->wait_seq == token.seq ? task->wake_reason : PROC_WAKE_CANCEL;
    spin_unlock_irqrestore(&proc_lock, flags);
    return reason;
}

proc_wake_reason_t proc_park_wait(proc_wait_mode_t mode, uint64_t deadline)
{
    proc_wait_token_t token = proc_park_prepare(mode, deadline);
    if (!token.task &&
        token.prepare_error == PROC_PARK_PREPARE_TIMEOUT_CAPACITY)
        return PROC_WAKE_TIMEOUT_CAPACITY;
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
    uint64_t priority_cpus = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (unsigned i = 0; i < wake_q->count; i++) {
        proc_wake_q_item_t *item = &wake_q->items[i];
        if (proc_try_wake_locked_common(item->task, item->seq, item->reason,
                                        &remote_cpus, &priority_cpus))
            woke++;
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    for (unsigned i = 0; i < wake_q->count; i++) {
        proc_put(wake_q->items[i].task);
        proc_lifetime_note_wake_remove();
        wake_q->items[i].task = NULL;
    }
    wake_q->count = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && cpu < 64; cpu++) {
        if (remote_cpus & (1ULL << cpu))
            proc_sched_request_cpu(
                cpu, (priority_cpus & (1ULL << cpu)) != 0);
    }
    return woke;
}
