#include "proc/proc_internal.h"

#include "core/cpu.h"
#include "core/klog.h"
#include "core/timer.h"
#include "core/string.h"
#include "drv/virtio_blk.h"
#include "net/lwip_stack.h"
#include "proc/signal.h"

typedef struct proc_runq {
    task_t *head[SCHED_LEVELS];
    task_t *tail[SCHED_LEVELS];
    uint32_t bitmap;
    unsigned nr_running;
} proc_runq_t;

static proc_runq_t sched_runq[CONFIG_NR_CPUS];
static uint64_t next_wake_scan = SCHED_NO_DEADLINE;
static uint64_t next_alarm_scan = SCHED_NO_DEADLINE;
static int sched_diag_count;

static proc_runq_t *sched_current_runq(void) {
    return &sched_runq[cpu_current_id()];
}

static proc_runq_t *sched_task_runq(task_t *t) {
    unsigned cpu = t && t->cpu_id < CONFIG_NR_CPUS ? t->cpu_id : cpu_current_id();
    return &sched_runq[cpu];
}

static int sched_level_clamp(int level) {
    if (level < 0) return 0;
    if (level >= SCHED_LEVELS) return SCHED_LEVELS - 1;
    return level;
}

void proc_sched_runq_init(void) {
    memset(sched_runq, 0, sizeof(sched_runq));
    next_wake_scan = SCHED_NO_DEADLINE;
    next_alarm_scan = SCHED_NO_DEADLINE;
    sched_diag_count = 0;
}

unsigned proc_sched_select_cpu_locked(task_t *t)
{
    unsigned current = cpu_current_id();
    if (CONFIG_NR_CPUS <= 1)
        return current;

    if (t && t == proc_current())
        return current;

    unsigned best = current;
    unsigned best_load = sched_runq[current].nr_running;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (sched_runq[cpu].nr_running < best_load) {
            best = cpu;
            best_load = sched_runq[cpu].nr_running;
        }
    }
    return best;
}

void proc_sched_kick_cpu(unsigned cpu)
{
    (void)cpu;
    /*
     * Future SMP bringup should send an IPI/reschedule interrupt here.
     * Single-CPU builds keep the hook as a no-op.
     */
}

static void sched_note_deadline(uint64_t *slot, uint64_t value)
{
    if (value == 0)
        return;
    uint64_t old = __atomic_load_n(slot, __ATOMIC_RELAXED);
    while (value < old &&
           !__atomic_compare_exchange_n(slot, &old, value, 1,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
}

void proc_set_wake_time(task_t *t, uint64_t wake_time)
{
    if (!t)
        return;
    __atomic_store_n(&t->wake_time, wake_time, __ATOMIC_RELAXED);
    sched_note_deadline(&next_wake_scan, wake_time);
}

void proc_set_alarm_expire(task_t *t, uint64_t alarm_expire)
{
    if (!t)
        return;
    __atomic_store_n(&t->alarm_expire, alarm_expire, __ATOMIC_RELAXED);
    sched_note_deadline(&next_alarm_scan, alarm_expire);
}

void proc_runq_enqueue_locked(task_t *t) {
    if (!t || t == proc_idle_task() || t->state != PROC_READY || t->on_rq)
        return;

    if (t->cpu_id >= CONFIG_NR_CPUS)
        t->cpu_id = proc_sched_select_cpu_locked(t);

    proc_runq_t *rq = sched_task_runq(t);
    int q = sched_level_clamp(t->sched_level);
    t->sched_level = q;
    t->rq_next = NULL;
    t->rq_prev = rq->tail[q];
    if (rq->tail[q])
        rq->tail[q]->rq_next = t;
    else
        rq->head[q] = t;
    rq->tail[q] = t;
    t->on_rq = 1;
    rq->nr_running++;
    rq->bitmap |= (1U << q);
}

void proc_runq_remove_locked(task_t *t) {
    if (!t || !t->on_rq)
        return;

    proc_runq_t *rq = sched_task_runq(t);
    int q = sched_level_clamp(t->sched_level);
    if (t->rq_prev)
        t->rq_prev->rq_next = t->rq_next;
    else
        rq->head[q] = t->rq_next;
    if (t->rq_next)
        t->rq_next->rq_prev = t->rq_prev;
    else
        rq->tail[q] = t->rq_prev;
    if (!rq->head[q])
        rq->bitmap &= ~(1U << q);

    t->rq_next = NULL;
    t->rq_prev = NULL;
    t->on_rq = 0;
    if (rq->nr_running > 0)
        rq->nr_running--;
}

task_t *proc_runq_pick_locked(void) {
    proc_runq_t *rq = sched_current_runq();
    while (rq->bitmap) {
        int q = 0;
        while (q < SCHED_LEVELS && !(rq->bitmap & (1U << q)))
            q++;
        if (q >= SCHED_LEVELS)
            return NULL;

        task_t *t = rq->head[q];
        if (!t) {
            rq->bitmap &= ~(1U << q);
            continue;
        }

        rq->head[q] = t->rq_next;
        if (t->rq_next)
            t->rq_next->rq_prev = NULL;
        else
            rq->tail[q] = NULL;
        if (!rq->head[q])
            rq->bitmap &= ~(1U << q);

        t->rq_next = NULL;
        t->rq_prev = NULL;
        t->on_rq = 0;
        if (rq->nr_running > 0)
            rq->nr_running--;

        if (t->state == PROC_READY && t->kstack)
            return t;
    }
    return NULL;
}

static void sched_scan_timers(uint64_t now)
{
    uint64_t next_wake = SCHED_NO_DEADLINE;
    uint64_t next_alarm = SCHED_NO_DEADLINE;

    for (int i = 0; i < MAX_PROCS; i++) {
        task_t *t = &proc_table[i];
        if (t->state == PROC_UNUSED)
            continue;

        uint64_t alarm = __atomic_load_n(&t->alarm_expire, __ATOMIC_RELAXED);
        if (alarm > 0) {
            if (now >= alarm) {
                uint64_t interval = t->itimer_real_interval;
                alarm = interval ? now + interval : 0;
                proc_set_alarm_expire(t, alarm);
                signal_send(t->pid, SIGALRM);
            }
            if (alarm > 0 && alarm < next_alarm)
                next_alarm = alarm;
        }

        uint64_t wake = __atomic_load_n(&t->wake_time, __ATOMIC_RELAXED);
        if (t->state == PROC_BLOCKED && wake > 0) {
            if (now >= wake) {
                if (sched_diag_count < 128) {
                    sched_diag_count++;
                    kdebug("[SCHEDDBG] wake pid=%d now=%lu wake=%lu\n",
                          t->pid,
                          (unsigned long)now,
                          (unsigned long)wake);
                }
                proc_set_wake_time(t, 0);
                t->sched_level = 0;
                proc_make_ready(t);
            } else if (wake < next_wake) {
                next_wake = wake;
            }
        }
    }

    __atomic_store_n(&next_wake_scan, next_wake, __ATOMIC_RELAXED);
    __atomic_store_n(&next_alarm_scan, next_alarm, __ATOMIC_RELAXED);
}

void context_switch(task_t *next) {
    if (!next || !next->kstack)
        return;
    if (next == proc_current()) {
        next->state = PROC_RUNNING;
        next->on_rq = 0;
        return;
    }
    task_t *prev = proc_set_current(next);
    next->state  = PROC_RUNNING;
    next->on_rq  = 0;
    if (prev)
        arch_set_task_pointer(prev);
    __switch(next->kstack);
}

void sched(void) {
    uint64_t now = timer_get_ticks();

    virtio_blk_poll_all();
    a20_lwip_poll();

    if (now >= __atomic_load_n(&next_wake_scan, __ATOMIC_RELAXED) ||
        now >= __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED))
        sched_scan_timers(now);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *next = proc_runq_pick_locked();
    spin_unlock_irqrestore(&proc_lock, flags);

    if (next) {
        if (sched_diag_count < 256) {
            sched_diag_count++;
            kdebug("[SCHEDDBG] pick cur=%d next=%d level=%d state=%d\n",
                  proc_current() ? proc_current()->pid : -1, next->pid,
                  next->sched_level, next->state);
        }
        context_switch(next);
        return;
    }

    if (proc_current() && proc_current()->state == PROC_READY) {
        proc_current()->state = PROC_RUNNING;
        return;
    }

    task_t *idle = proc_idle_task();
    if (proc_current() != idle)
        context_switch(idle);
}

void proc_yield(void) {
    task_t *cur = proc_current();
    task_t *idle = proc_idle_task();
    if (cur && cur != idle && cur->state == PROC_RUNNING) {
        if (cur->sched_level < SCHED_LEVELS - 1)
            cur->sched_level++;
        cur->state = PROC_READY;
        proc_make_ready(cur);
    }
    sched();
}
