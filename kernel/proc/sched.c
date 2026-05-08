#include "proc/proc_internal.h"

#include "core/cpu.h"
#include "core/klog.h"
#include "core/timer.h"
#include "core/string.h"
#include "drv/virtio_blk.h"
#include "net/lwip_stack.h"
#include "proc/signal.h"

typedef struct proc_runq {
    spinlock_t lock;
    task_t *head[SCHED_LEVELS];
    task_t *tail[SCHED_LEVELS];
    uint32_t bitmap;
    unsigned nr_running;
} proc_runq_t;

static proc_runq_t sched_runq[CONFIG_NR_CPUS];
static uint64_t next_wake_scan = SCHED_NO_DEADLINE;
static uint64_t next_alarm_scan = SCHED_NO_DEADLINE;

/* Per-CPU runqueue lock — separate from proc_lock.
 * runq_lock protects enqueue/dequeue/pick and per-runqueue state.
 * proc_lock protects task_list, task->state transitions, and zombie list.
 *
 * Ordering: proc_lock → runq_lock (never the reverse). */
static spinlock_t sched_runq_lock[CONFIG_NR_CPUS];

#define RUNQ_LOCK(cpu)     (&sched_runq_lock[(cpu)])
#define RUNQ_LOCK_IRQ(cpu) spin_lock_irqsave(RUNQ_LOCK(cpu))
#define RUNQ_UNLOCK_IRQ(cpu, f) spin_unlock_irqrestore(RUNQ_LOCK(cpu), (f))

static proc_runq_t *sched_current_runq(void) {
    return &sched_runq[cpu_current_id()];
}

static proc_runq_t *sched_task_runq(task_t *t) {
    unsigned cpu = t && t->cpu_id < CONFIG_NR_CPUS ? t->cpu_id : cpu_current_id();
    return &sched_runq[cpu];
}

static int proc_runq_contains_locked(proc_runq_t *rq, task_t *t)
{
    if (!rq || !t)
        return 0;
    for (int q = 0; q < SCHED_LEVELS; q++) {
        for (task_t *p = rq->head[q]; p; p = p->rq_next) {
            if (p == t)
                return 1;
        }
    }
    return 0;
}

static int sched_level_clamp(int level) {
    if (level < 0) return 0;
    if (level >= SCHED_LEVELS) return SCHED_LEVELS - 1;
    return level;
}

void proc_sched_runq_init(void) {
    memset(sched_runq, 0, sizeof(sched_runq));
    for (unsigned i = 0; i < CONFIG_NR_CPUS; i++)
        spin_init(&sched_runq_lock[i]);
    next_wake_scan = SCHED_NO_DEADLINE;
    next_alarm_scan = SCHED_NO_DEADLINE;
}

unsigned proc_sched_select_cpu(task_t *t)
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

/* Keep old name as compat wrapper for proc_internal.h callers */
unsigned proc_sched_select_cpu_locked(task_t *t)
{
    return proc_sched_select_cpu(t);
}

void proc_sched_kick_cpu(unsigned cpu)
{
    (void)cpu;
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

/* Enqueue a task onto its CPU's runqueue. Caller must hold runq_lock. */
void proc_runq_enqueue_locked(task_t *t) {
    if (!t || t == proc_idle_task() || t->state != PROC_READY)
        return;

    unsigned cpu = t->cpu_id < CONFIG_NR_CPUS ? t->cpu_id : cpu_current_id();

    uint64_t rf = RUNQ_LOCK_IRQ(cpu);
    proc_runq_t *rq = &sched_runq[cpu];

    if (t->on_rq) {
        if (proc_runq_contains_locked(rq, t)) {
            RUNQ_UNLOCK_IRQ(cpu, rf);
            return;
        }
        /* Stale on_rq — will re-enqueue below */
    }

    int q = sched_level_clamp(t->sched_level);
    t->sched_level = q;
    t->cpu_id = cpu;
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
    RUNQ_UNLOCK_IRQ(cpu, rf);
}

void proc_runq_remove_locked(task_t *t) {
    if (!t || !t->on_rq)
        return;

    unsigned cpu = t->cpu_id < CONFIG_NR_CPUS ? t->cpu_id : cpu_current_id();
    uint64_t rf = RUNQ_LOCK_IRQ(cpu);
    proc_runq_t *rq = &sched_runq[cpu];

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
    RUNQ_UNLOCK_IRQ(cpu, rf);
}

/* Pick next task from current CPU's runqueue. No longer does O(n) task-list scan. */
task_t *proc_runq_pick_locked(void) {
    unsigned cpu = cpu_current_id();
    uint64_t rf = RUNQ_LOCK_IRQ(cpu);
    proc_runq_t *rq = &sched_runq[cpu];

    while (rq->bitmap) {
        int q = 0;
        while (q < SCHED_LEVELS && !(rq->bitmap & (1U << q)))
            q++;
        if (q >= SCHED_LEVELS)
            break;

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

        if (t != proc_idle_task() && t->state == PROC_READY && t->kstack) {
            RUNQ_UNLOCK_IRQ(cpu, rf);
            return t;
        }
    }

    RUNQ_UNLOCK_IRQ(cpu, rf);
    return NULL;
}

static void sched_scan_timers(uint64_t now)
{
    uint64_t next_wake = SCHED_NO_DEADLINE;
    uint64_t next_alarm = SCHED_NO_DEADLINE;
    int sigalrm_pids[32];
    int sigalrm_count = 0;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED)
            continue;

        uint64_t alarm = __atomic_load_n(&t->alarm_expire, __ATOMIC_RELAXED);
        if (alarm > 0) {
            if (now >= alarm) {
                uint64_t interval = t->itimer_real_interval;
                alarm = interval ? now + interval : 0;
                __atomic_store_n(&t->alarm_expire, alarm, __ATOMIC_RELAXED);
                if (sigalrm_count < (int)(sizeof(sigalrm_pids) / sizeof(sigalrm_pids[0])))
                    sigalrm_pids[sigalrm_count++] = t->pid;
            }
            if (alarm > 0 && alarm < next_alarm)
                next_alarm = alarm;
        }

        uint64_t wake = __atomic_load_n(&t->wake_time, __ATOMIC_RELAXED);
        if (t->state == PROC_BLOCKED && wake > 0) {
            if (now >= wake) {
                __atomic_store_n(&t->wake_time, 0, __ATOMIC_RELAXED);
                t->sched_level = 0;
                t->state = PROC_READY;
                t->exec_start = now;
                spin_unlock_irqrestore(&proc_lock, flags);
                proc_runq_enqueue_locked(t);
                flags = spin_lock_irqsave(&proc_lock);
            } else if (wake < next_wake) {
                next_wake = wake;
            }
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    for (int i = 0; i < sigalrm_count; i++)
        signal_send(sigalrm_pids[i], SIGALRM);

    __atomic_store_n(&next_wake_scan, next_wake, __ATOMIC_RELAXED);
    __atomic_store_n(&next_alarm_scan, next_alarm, __ATOMIC_RELAXED);
}

/* Scan for reapable zombies — called from idle loop, not hot path. */
void sched_reap_zombies(void)
{
    for (;;) {
        int target_pid = -1;
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (t == proc_idle_task())
                continue;
            if (t->state == PROC_ZOMBIE) {
                task_t *parent = t->parent;
                if (!parent || parent == proc_idle_task() ||
                    t->ppid == 0 ||
                    (t->clone_flags & CLONE_THREAD)) {
                    target_pid = t->pid;
                    break;
                }
                if (parent->signals) {
                    signal_state_t *ss = (signal_state_t *)parent->signals;
                    sigaction_t *act = &ss->actions[SIGCHLD];
                    if (act->sa_handler == SIG_IGN || (act->sa_flags & SA_NOCLDWAIT)) {
                        target_pid = t->pid;
                        break;
                    }
                }
            }
        }
        spin_unlock_irqrestore(&proc_lock, flags);
        if (target_pid < 0)
            break;
        task_t *t = proc_find(target_pid);
        if (t)
            proc_destroy_task(t);
    }
}

/* Deferred I/O poll counter — poll every N idle iterations to reduce overhead. */
static uint64_t sched_poll_counter;

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

    /* Timer scanning: only scan when a deadline has actually been reached,
     * avoiding O(n) traversal on every sched() call. */
    if (now >= __atomic_load_n(&next_wake_scan, __ATOMIC_RELAXED) ||
        now >= __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED))
        sched_scan_timers(now);

    task_t *next = proc_runq_pick_locked();

    if (next) {
        next->exec_start = now;
        context_switch(next);
        return;
    }

    task_t *cur = proc_current();
    if (cur && cur->state == PROC_READY) {
        cur->state = PROC_RUNNING;
        return;
    }

    task_t *idle = proc_idle_task();
    if (cur != idle)
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
