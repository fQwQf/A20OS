#include "proc/proc_internal.h"

#include "core/cpu.h"
#include "core/klog.h"
#include "core/timer.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/smp.h"
#include "core/progress.h"
#include "core/panic.h"
#include "proc/signal.h"
#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"
#ifdef CONFIG_ABI_NATIVE
#include "abi/native/ipc_internal.h"
#endif

typedef struct __attribute__((aligned(64))) proc_runq {
    spinlock_t lock;
    task_t *head[SCHED_LEVELS];
    task_t *tail[SCHED_LEVELS];
    uint32_t bitmap;
    unsigned nr_running;
} proc_runq_t;

/*
 * SCHEDULER_CONCURRENCY_PREREQS:
 * - secondary CPU bootstrap must call smp_secondary_init(), set a unique
 *   arch_current_cpu_id(), initialize that CPU's idle task/current slot, then
 *   enter sched() with interrupts enabled.
 * - IPI reschedule must be safe from any CPU after proc_make_ready() publishes a
 *   remote READY task and before the target CPU picks it from its runqueue.
 * - Cross-CPU wakeup must hold proc_lock before choosing target cpu_id, then
 *   acquire only the target runqueue lock for enqueue. Reverse order is banned.
 * - context_switch() must be the only path that publishes PROC_RUNNING for a
 *   runnable task; proc_runq_pick_locked() must atomically move on_rq to
 *   dispatching before that point.
 * - Timer wake, signal wake, futex wake, wait queue wake, fork/exec/wait, and
 *   exit/reap must all preserve TASK_STATE_MUTATION_CONTRACT before NR_CPUS>1
 *   may be enabled without ALLOW_UNVERIFIED_SMP.
 */

static proc_runq_t sched_runq[CONFIG_NR_CPUS];
static uint64_t next_wake_scan = SCHED_NO_DEADLINE;
static uint64_t next_alarm_scan = SCHED_NO_DEADLINE;
static unsigned sched_zombies_pending;

/*
 * SCHEDULER_CPU_OWNERSHIP:
 *   on_rq -> dispatching -> on_cpu -> unowned
 *
 * proc_lock serializes scheduler ownership transitions. The selected runqueue
 * lock is nested below it while queue membership changes. An outgoing task
 * keeps on_cpu set across __switch; proc_switch_complete() clears ownership
 * only after the replacement task is executing on its own kernel stack.
 */

typedef struct wait_timer {
    uint64_t deadline;
    task_t *task;
    uint64_t wait_seq;
} wait_timer_t;

#ifdef CONFIG_MCU
#define WAIT_TIMER_HEAP_MAX 64
#else
#define WAIT_TIMER_HEAP_MAX 8192
#endif

/* Tokenized deadline min-heap, protected by proc_lock. */
static wait_timer_t wait_timer_heap[WAIT_TIMER_HEAP_MAX];
static unsigned wait_timer_count;

unsigned proc_wait_timer_count_locked(void)
{
    return wait_timer_count;
}

unsigned proc_sched_select_cpu(task_t *t);

#define SCHED_TICK_INTERVAL       (TICKS_PER_SEC / 100)
#define SCHED_MIN_TIMER_INTERVAL  (TICKS_PER_SEC / 10000 ? TICKS_PER_SEC / 10000 : 1)
#define SCHED_AGING_THRESHOLD     (TICKS_PER_SEC / 20 ? TICKS_PER_SEC / 20 : 1)

#ifdef CONFIG_MCU
#define SCHED_SIGNAL_BATCH 8
#define SCHED_REAP_BATCH   8
#else
#define SCHED_SIGNAL_BATCH 128
#define SCHED_REAP_BATCH   32
#endif

/* Per-CPU runqueue lock — separate from proc_lock.
 * runq_lock protects enqueue/dequeue/pick and per-runqueue state.
 * proc_lock protects task_list, task->state transitions, and zombie list.
 *
 * Ordering: proc_lock → runq_lock (never the reverse). */
#define RUNQ_LOCK(cpu)     (&sched_runq[(cpu)].lock)
#define RUNQ_LOCK_IRQ(cpu) spin_lock_irqsave(RUNQ_LOCK(cpu))
#define RUNQ_UNLOCK_IRQ(cpu, f) spin_unlock_irqrestore(RUNQ_LOCK(cpu), (f))

static int sched_level_clamp(int level) {
    if (level < 0) return 0;
    if (level >= SCHED_LEVELS) return SCHED_LEVELS - 1;
    return level;
}

static int sched_task_rt(task_t *t)
{
    return t && (t->sched_policy == SCHED_FIFO || t->sched_policy == SCHED_RR);
}

static void sched_runq_unlink_at(proc_runq_t *rq, task_t *t, int q)
{
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
}

static void sched_runq_append_at(proc_runq_t *rq, task_t *t, int q)
{
    t->rq_next = NULL;
    t->rq_prev = rq->tail[q];
    if (rq->tail[q])
        rq->tail[q]->rq_next = t;
    else
        rq->head[q] = t;
    rq->tail[q] = t;
    rq->bitmap |= (1U << q);
}

static void sched_promote_aged_locked(proc_runq_t *rq, uint64_t now)
{
    for (int q = 1; q < SCHED_LEVELS; q++) {
        task_t *it = rq->head[q];
        while (it) {
            task_t *next = it->rq_next;
            if (!sched_task_rt(it) && it->state == PROC_READY &&
                it->ready_since > 0 &&
                now - it->ready_since >= SCHED_AGING_THRESHOLD) {
                sched_runq_unlink_at(rq, it, q);
                it->sched_level = 0;
                it->ready_since = now;
                sched_runq_append_at(rq, it, 0);
            }
            it = next;
        }
    }
}

void proc_sched_runq_init(void) {
    memset(sched_runq, 0, sizeof(sched_runq));
    for (unsigned i = 0; i < CONFIG_NR_CPUS; i++)
        spin_init(&sched_runq[i].lock);
    next_wake_scan = SCHED_NO_DEADLINE;
    next_alarm_scan = SCHED_NO_DEADLINE;
    wait_timer_count = 0;
}

uint64_t proc_next_timer_interval(uint64_t now)
{
    uint64_t next = now + SCHED_TICK_INTERVAL;
    uint64_t wake = __atomic_load_n(&next_wake_scan, __ATOMIC_RELAXED);
    uint64_t alarm = __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED);

    if (wake < next)
        next = wake;
    if (alarm < next)
        next = alarm;
    if (next <= now)
        return SCHED_MIN_TIMER_INTERVAL;

    uint64_t delta = next - now;
    if (delta < SCHED_MIN_TIMER_INTERVAL)
        delta = SCHED_MIN_TIMER_INTERVAL;
    return delta;
}

#if CONFIG_NR_CPUS >= 32
#define SCHED_CPU_MASK_ALL (~0U)
#else
#define SCHED_CPU_MASK_ALL ((1U << CONFIG_NR_CPUS) - 1U)
#endif

static uint32_t sched_task_cpu_mask(task_t *t)
{
    uint32_t mask = SCHED_CPU_MASK_ALL;

    if (t)
        mask &= t->cpus_allowed;

    if (t && t->cgroup) {
        cg_node_t *node = (cg_node_t *)t->cgroup;
        mask &= node->res.cpuset.effective_cpus;
    }

    return mask & SCHED_CPU_MASK_ALL & smp_online_cpu_mask();
}

static int sched_policy_valid(int policy)
{
    return policy == SCHED_NORMAL || policy == SCHED_FIFO ||
           policy == SCHED_RR || policy == SCHED_BATCH ||
           policy == SCHED_IDLE;
}

static int sched_policy_rt_value(int policy)
{
    return policy == SCHED_FIFO || policy == SCHED_RR;
}

static int sched_nice_value(task_t *t)
{
    if (!sched_policy_rt_value(t->sched_policy))
        return t->priority;
    for (int nice = -20; nice <= 19; nice++) {
        if (sched_weight_for_nice(nice) == t->cfs_weight)
            return nice;
    }
    return 0;
}

static int sched_task_linked_locked(task_t *target)
{
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t == target)
            return 1;
    }
    return 0;
}

uint32_t proc_sched_effective_affinity(task_t *t)
{
    return sched_task_cpu_mask(t);
}

int proc_sched_priority_range(int policy, int *min, int *max)
{
    if (!sched_policy_valid(policy) || !min || !max)
        return -1;
    *min = sched_policy_rt_value(policy) ? 1 : 0;
    *max = sched_policy_rt_value(policy) ? 99 : 0;
    return 0;
}

int proc_sched_get(task_t *t, proc_sched_config_t *out)
{
    if (!t || !out)
        return -1;
    uint64_t lock_flags = spin_lock_irqsave(&proc_lock);
    if (!sched_task_linked_locked(t)) {
        spin_unlock_irqrestore(&proc_lock, lock_flags);
        return -1;
    }
    out->fields = PROC_SCHED_POLICY | PROC_SCHED_PRIORITY |
                  PROC_SCHED_AFFINITY | PROC_SCHED_NICE;
    out->policy = t->sched_policy;
    out->priority = sched_policy_rt_value(t->sched_policy) ? t->priority : 0;
    out->nice = sched_nice_value(t);
    out->affinity = proc_sched_effective_affinity(t);
    out->reset_on_fork = t->sched_reset_on_fork;
    spin_unlock_irqrestore(&proc_lock, lock_flags);
    return 0;
}

int proc_sched_set(task_t *t, const proc_sched_config_t *config)
{
    const uint32_t all_fields = PROC_SCHED_POLICY | PROC_SCHED_PRIORITY |
                                PROC_SCHED_AFFINITY | PROC_SCHED_NICE;
    if (!t || !config || (config->fields & ~all_fields))
        return -1;

    uint64_t lock_flags = spin_lock_irqsave(&proc_lock);
    if (!sched_task_linked_locked(t))
        goto invalid;
    int policy = (config->fields & PROC_SCHED_POLICY)
                     ? config->policy : t->sched_policy;
    int priority = (config->fields & PROC_SCHED_PRIORITY)
                       ? config->priority
                       : (sched_policy_rt_value(policy) &&
                          sched_policy_rt_value(t->sched_policy)
                              ? t->priority : 0);
    if (!sched_policy_valid(policy) ||
        (sched_policy_rt_value(policy)
             ? (priority < 1 || priority > 99)
             : priority != 0))
        goto invalid;
    if ((config->fields & PROC_SCHED_NICE) &&
        (config->nice < -20 || config->nice > 19))
        goto invalid;
    if (config->fields & PROC_SCHED_AFFINITY) {
        uint32_t eligible = config->affinity & smp_online_cpu_mask();
        if (t->cgroup) {
            cg_node_t *node = (cg_node_t *)t->cgroup;
            eligible &= node->res.cpuset.effective_cpus;
        }
        if (!eligible)
            goto invalid;
        if ((t->on_cpu || t->dispatching) &&
            (t->owner_cpu >= 32 ||
             !(eligible & (1U << t->owner_cpu))))
            goto invalid;
    }

    int ready = t->on_rq && t->state == PROC_READY;
    if (t->on_rq)
        proc_runq_remove_locked(t);

    int old_nice = sched_nice_value(t);
    int old_rt = sched_policy_rt_value(t->sched_policy);
    if (config->fields & PROC_SCHED_POLICY) {
        t->sched_policy = policy;
        t->sched_reset_on_fork = config->reset_on_fork;
        if (sched_policy_rt_value(policy))
            t->sched_level = 0;
        else if (old_rt)
            t->priority = old_nice;
    }
    if (sched_policy_rt_value(policy))
        t->priority = priority;
    if (config->fields & PROC_SCHED_NICE) {
        if (!sched_policy_rt_value(policy))
            t->priority = config->nice;
        t->cfs_weight = sched_weight_for_nice(config->nice);
    }
    if (config->fields & PROC_SCHED_AFFINITY) {
        t->cpus_allowed = config->affinity & SCHED_CPU_MASK_ALL;
        if (!t->on_cpu && !t->dispatching &&
            (t->cpu_id >= 32 ||
             !(t->cpus_allowed & (1U << t->cpu_id))))
            t->cpu_id = proc_sched_select_cpu(t);
    }
    if (ready)
        proc_runq_enqueue_locked(t);
    spin_unlock_irqrestore(&proc_lock, lock_flags);
    return 0;

invalid:
    spin_unlock_irqrestore(&proc_lock, lock_flags);
    return -1;
}

unsigned proc_sched_select_cpu(task_t *t)
{
    unsigned current = cpu_current_id();
    if (CONFIG_NR_CPUS <= 1)
        return current;

    uint32_t mask = sched_task_cpu_mask(t);
    if (!mask)
        return current;

    if (t && t == proc_current() && current < 32 && (mask & (1U << current)))
        return current;

    unsigned best = current;
    unsigned best_load = ~0U;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && cpu < 32; cpu++) {
        if (!(mask & (1U << cpu)))
            continue;
        unsigned load = __atomic_load_n(&sched_runq[cpu].nr_running,
                                        __ATOMIC_RELAXED);
        task_t *running = proc_current_on_cpu(cpu);
        if (running && running->pid != 0 && running->state == PROC_RUNNING)
            load++;
        if (load < best_load) {
            best = cpu;
            best_load = load;
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
    if (cpu >= CONFIG_NR_CPUS || cpu == cpu_current_id())
        return;
    smp_send_reschedule(cpu);
}

void proc_sched_assert_task_locked(task_t *t)
{
#if CONFIG_DEBUG_SCHED_STATE
    if (!t)
        return;
    uintptr_t caller = (uintptr_t)__builtin_return_address(0);
    if (t->park_state == PROC_PARK_PREPARING &&
        t->state != PROC_RUNNING)
        panic("sched invariant: pid=%d preparing state=%d", t->pid, t->state);
    if (t->park_state == PROC_PARK_PARKED &&
        t->state != PROC_BLOCKED)
        panic("sched invariant: pid=%d parked state=%d", t->pid, t->state);

    /*
     * Take a stable cross-runqueue snapshot. A picker does not need proc_lock,
     * so checking one queue at a time would race with a dequeue between the
     * scan and the final on_rq read and report a false invariant failure.
     * No scheduler path holds two runqueue locks, so CPU order is deadlock-free.
     */
    uint64_t rq_flags[CONFIG_NR_CPUS];
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        rq_flags[cpu] = RUNQ_LOCK_IRQ(cpu);
    }
    unsigned memberships = 0;
    uint64_t membership_cpus = 0;
    uint32_t task_rq_bitmap = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        proc_runq_t *rq = &sched_runq[cpu];
        if (cpu == t->cpu_id)
            task_rq_bitmap = rq->bitmap;
        for (int level = 0; level < SCHED_LEVELS; level++) {
            for (task_t *it = rq->head[level]; it; it = it->rq_next) {
                if (it == t) {
                    memberships++;
                    if (cpu < 64)
                        membership_cpus |= 1ULL << cpu;
                }
            }
        }
    }
    int on_rq = t->on_rq;
    int dispatching = t->dispatching;
    int on_cpu = t->on_cpu;
    unsigned owner_cpu = t->owner_cpu;
    int state = t->state;
    unsigned task_cpu = t->cpu_id;
    for (unsigned cpu = CONFIG_NR_CPUS; cpu > 0; cpu--)
        RUNQ_UNLOCK_IRQ(cpu - 1, rq_flags[cpu - 1]);
    if (on_rq && state != PROC_READY)
        panic("sched invariant: pid=%d on_rq state=%d task_cpu=%u queues=0x%lx bitmap=0x%x caller=0x%lx",
              t->pid, state, task_cpu, (unsigned long)membership_cpus,
              task_rq_bitmap, (unsigned long)caller);
    if ((state == PROC_BLOCKED || state == PROC_RUNNING ||
         state == PROC_STOPPED || state == PROC_ZOMBIE ||
         state == PROC_UNUSED) && on_rq)
        panic("sched invariant: pid=%d non-ready state=%d queued",
              t->pid, state);
    if (memberships > 1 || (!!on_rq != (memberships == 1)))
        panic("sched invariant: pid=%d on_rq=%d memberships=%u",
              t->pid, on_rq, memberships);
    if (on_rq && (dispatching || on_cpu))
        panic("sched invariant: pid=%d on_rq=%d dispatching=%d on_cpu=%d",
              t->pid, on_rq, dispatching, on_cpu);
    if (dispatching && on_cpu)
        panic("sched invariant: pid=%d dispatching and on_cpu owner=%u",
              t->pid, owner_cpu);
    if (dispatching && state != PROC_READY)
        panic("sched invariant: pid=%d dispatching state=%d owner=%u",
              t->pid, state, owner_cpu);
    if (!!(dispatching || on_cpu) != (owner_cpu != PROC_CPU_NONE))
        panic("sched invariant: pid=%d dispatching=%d on_cpu=%d owner=%u",
              t->pid, dispatching, on_cpu, owner_cpu);
    if ((dispatching || on_cpu) && owner_cpu >= CONFIG_NR_CPUS)
        panic("sched invariant: pid=%d invalid owner=%u",
              t->pid, owner_cpu);
    if (state == PROC_RUNNING && t != proc_idle_task() && !on_cpu)
        panic("sched invariant: running pid=%d without cpu ownership",
              t->pid);
    if (state == PROC_UNUSED && (on_rq || dispatching || on_cpu))
        panic("sched invariant: unused pid=%d rq=%d dispatch=%d cpu=%d",
              t->pid, on_rq, dispatching, on_cpu);
#else
    (void)t;
#endif
}

unsigned proc_sched_task_runq_memberships_locked(task_t *t)
{
    if (!t)
        return 0;

    uint64_t rq_flags[CONFIG_NR_CPUS];
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++)
        rq_flags[cpu] = RUNQ_LOCK_IRQ(cpu);

    unsigned memberships = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        proc_runq_t *rq = &sched_runq[cpu];
        for (int level = 0; level < SCHED_LEVELS; level++) {
            for (task_t *it = rq->head[level]; it; it = it->rq_next) {
                if (it == t)
                    memberships++;
            }
        }
    }

    for (unsigned cpu = CONFIG_NR_CPUS; cpu > 0; cpu--)
        RUNQ_UNLOCK_IRQ(cpu - 1, rq_flags[cpu - 1]);
    return memberships;
}

void proc_make_ready(task_t *t)
{
    if (!t)
        return;

    unsigned target_cpu = cpu_current_id();
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state == PROC_UNUSED || t->state == PROC_ZOMBIE) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return;
    }
    if (t->park_state == PROC_PARK_PREPARING ||
        t->park_state == PROC_PARK_PARKED) {
        (void)proc_try_wake_locked(t, t->wait_seq, PROC_WAKE_EVENT);
        proc_sched_assert_task_locked(t);
        spin_unlock_irqrestore(&proc_lock, flags);
        return;
    }

    /*
     * A remote RUNNING task is already making progress and must not be
     * converted to READY behind its owner CPU. The local current task reaches
     * this path for yield and publishes READY without becoming queueable until
     * switch completion.
     */
    if (t->state == PROC_RUNNING &&
        (t != proc_current() || !t->on_cpu ||
         t->owner_cpu != cpu_current_id())) {
        proc_sched_assert_task_locked(t);
        spin_unlock_irqrestore(&proc_lock, flags);
        return;
    }

    int was_blocked = t->state == PROC_BLOCKED;
    if (t->state != PROC_READY) {
        t->state = PROC_READY;
        if (t->wake_time == 0 && t->sched_level > 0)
            t->sched_level--;
    }
    if (t->on_cpu) {
        t->cpu_id = t->owner_cpu;
    } else if (!t->dispatching && !t->on_rq) {
        if (!was_blocked)
            t->cpu_id = proc_sched_select_cpu_locked(t);
    }
    target_cpu = t->cpu_id;
    proc_runq_enqueue_locked(t);
    int queued = t->on_rq;
    proc_sched_assert_task_locked(t);
    spin_unlock_irqrestore(&proc_lock, flags);

    if (queued && target_cpu != cpu_current_id())
        proc_sched_kick_cpu(target_cpu);
}

void proc_sched_stop_current(int exit_code)
{
    task_t *t = proc_current();
    if (!t || t == proc_idle_task())
        return;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state == PROC_RUNNING) {
        t->exit_code = exit_code;
        t->state = PROC_STOPPED;
        proc_sched_assert_task_locked(t);
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    sched();
}

static int sched_note_deadline(uint64_t *slot, uint64_t value)
{
    if (value == 0)
        return 0;
#ifdef CONFIG_MCU
    /* Cortex-M3 has no lock-free 64-bit compare/exchange. Timer deadlines are
     * shared only with interrupt/preemption paths on this single-core target. */
    uint64_t flags = arch_irq_save();
    uint64_t old = *slot;
    if (value < old)
        *slot = value;
    arch_irq_restore(flags);
    return value < old;
#else
    uint64_t old = __atomic_load_n(slot, __ATOMIC_RELAXED);
    while (value < old &&
           !__atomic_compare_exchange_n(slot, &old, value, 1,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    return value < old;
#endif
}

static void sched_rearm_timer(void)
{
    uint64_t now = timer_get_ticks();
    timer_set_interval(proc_next_timer_interval(now));
}

static void wait_timer_heap_swap(unsigned a, unsigned b)
{
    wait_timer_t tmp = wait_timer_heap[a];
    wait_timer_heap[a] = wait_timer_heap[b];
    wait_timer_heap[b] = tmp;
    wait_timer_heap[a].task->wait_timer_index = (int)a;
    wait_timer_heap[b].task->wait_timer_index = (int)b;
}

static void wait_timer_sift_up(unsigned index)
{
    while (index > 0) {
        unsigned parent = (index - 1) / 2;
        if (wait_timer_heap[parent].deadline <=
            wait_timer_heap[index].deadline)
            break;
        wait_timer_heap_swap(parent, index);
        index = parent;
    }
}

static void wait_timer_sift_down(unsigned index)
{
    for (;;) {
        unsigned left = index * 2 + 1;
        unsigned right = left + 1;
        unsigned smallest = index;
        if (left < wait_timer_count &&
            wait_timer_heap[left].deadline <
            wait_timer_heap[smallest].deadline)
            smallest = left;
        if (right < wait_timer_count &&
            wait_timer_heap[right].deadline <
            wait_timer_heap[smallest].deadline)
            smallest = right;
        if (smallest == index)
            break;
        wait_timer_heap_swap(index, smallest);
        index = smallest;
    }
}

static wait_timer_t wait_timer_remove_index_locked(unsigned index)
{
    wait_timer_t removed = {0};
    if (index >= wait_timer_count)
        return removed;

    removed = wait_timer_heap[index];
    wait_timer_count--;
    if (index != wait_timer_count) {
        wait_timer_heap[index] = wait_timer_heap[wait_timer_count];
        wait_timer_heap[index].task->wait_timer_index = (int)index;
        if (index > 0 &&
            wait_timer_heap[index].deadline <
            wait_timer_heap[(index - 1) / 2].deadline)
            wait_timer_sift_up(index);
        else
            wait_timer_sift_down(index);
    }
    memset(&wait_timer_heap[wait_timer_count], 0,
           sizeof(wait_timer_heap[wait_timer_count]));
    if (removed.task && removed.task->wait_seq == removed.wait_seq)
        removed.task->wait_timer_index = -1;
    __atomic_store_n(&next_wake_scan,
                     wait_timer_count ? wait_timer_heap[0].deadline :
                                        SCHED_NO_DEADLINE,
                     __ATOMIC_RELAXED);
    return removed;
}

int proc_wait_timer_register_locked(task_t *t, uint64_t deadline,
                                    uint64_t wait_seq)
{
    if (!t || !deadline || !wait_seq)
        return -1;
    if (t->wait_timer_index >= 0)
        proc_wait_timer_cancel_locked(t, t->wait_seq);
    if (wait_timer_count >= WAIT_TIMER_HEAP_MAX)
        return -1;

    uint64_t old_first =
        wait_timer_count ? wait_timer_heap[0].deadline : SCHED_NO_DEADLINE;
    unsigned index = wait_timer_count++;
    wait_timer_heap[index].deadline = deadline;
    wait_timer_heap[index].task = proc_get(t);
    if (!wait_timer_heap[index].task) {
        wait_timer_count--;
        memset(&wait_timer_heap[wait_timer_count], 0,
               sizeof(wait_timer_heap[wait_timer_count]));
        return -1;
    }
    wait_timer_heap[index].wait_seq = wait_seq;
    t->wait_timer_index = (int)index;
    t->wake_time = deadline;
    wait_timer_sift_up(index);
    __atomic_store_n(&next_wake_scan, wait_timer_heap[0].deadline,
                     __ATOMIC_RELAXED);
    if (wait_timer_heap[0].deadline < old_first)
        sched_rearm_timer();
    return 0;
}

void proc_wait_timer_cancel_locked(task_t *t, uint64_t wait_seq)
{
    if (!t || t->wait_timer_index < 0)
        return;
    unsigned index = (unsigned)t->wait_timer_index;
    if (index >= wait_timer_count ||
        wait_timer_heap[index].task != t ||
        wait_timer_heap[index].wait_seq != wait_seq)
        return;
    wait_timer_t removed = wait_timer_remove_index_locked(index);
    t->wake_time = 0;
    proc_put(removed.task);
}

void proc_set_alarm_expire(task_t *t, uint64_t alarm_expire)
{
    if (!t)
        return;
    __atomic_store_n(&t->alarm_expire, alarm_expire, __ATOMIC_RELAXED);
    if (sched_note_deadline(&next_alarm_scan, alarm_expire))
        sched_rearm_timer();
}

/* Enqueue a task onto its CPU's runqueue. Caller must hold proc_lock. */
void proc_runq_enqueue_locked(task_t *t) {
    if (!t || t == proc_idle_task() || t->state != PROC_READY)
        return;

    unsigned cpu = t->cpu_id < CONFIG_NR_CPUS ? t->cpu_id : cpu_current_id();

    uint64_t rf = RUNQ_LOCK_IRQ(cpu);
    proc_runq_t *rq = &sched_runq[cpu];

    if (t->on_rq || t->dispatching || t->on_cpu) {
        RUNQ_UNLOCK_IRQ(cpu, rf);
        return;
    }
#if CONFIG_DEBUG_SCHED_STATE
    if (t->owner_cpu != PROC_CPU_NONE)
        panic("runq enqueue: pid=%d unowned with owner=%u",
              t->pid, t->owner_cpu);
#endif

    int q = sched_task_rt(t) ? 0 : sched_level_clamp(t->sched_level);
    if (!proc_get(t)) {
        RUNQ_UNLOCK_IRQ(cpu, rf);
        return;
    }
    t->sched_level = q;
    t->cpu_id = cpu;
    t->ready_since = timer_get_ticks();
    sched_runq_append_at(rq, t, q);
    t->on_rq = 1;
    __atomic_fetch_add(&rq->nr_running, 1, __ATOMIC_RELAXED);
    RUNQ_UNLOCK_IRQ(cpu, rf);
}

/* Remove a queued task. Caller must hold proc_lock. */
void proc_runq_remove_locked(task_t *t) {
    if (!t || !t->on_rq)
        return;

    unsigned cpu = t->cpu_id < CONFIG_NR_CPUS ? t->cpu_id : cpu_current_id();
    uint64_t rf = RUNQ_LOCK_IRQ(cpu);
    proc_runq_t *rq = &sched_runq[cpu];

    int q = sched_level_clamp(t->sched_level);
    sched_runq_unlink_at(rq, t, q);
    t->on_rq = 0;
    t->ready_since = 0;
    if (__atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&rq->nr_running, 1, __ATOMIC_RELAXED);
    RUNQ_UNLOCK_IRQ(cpu, rf);
    proc_put(t);
}

/*
 * Pick the next task and transfer on_rq -> dispatching while both proc_lock and
 * the local runqueue lock serialize observers. Caller must hold proc_lock.
 */
task_t *proc_runq_pick_locked(void) {
    unsigned cpu = cpu_current_id();
    uint64_t rf = RUNQ_LOCK_IRQ(cpu);
    proc_runq_t *rq = &sched_runq[cpu];
    sched_promote_aged_locked(rq, timer_get_ticks());

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

        if (q == 0) {
            task_t *best = NULL;
            for (task_t *it = rq->head[q]; it; it = it->rq_next) {
                if (!sched_task_rt(it))
                    continue;
                if (!best || it->priority > best->priority)
                    best = it;
            }
            if (best)
                t = best;
        }

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
        t->ready_since = 0;
        if (__atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED) > 0)
            __atomic_fetch_sub(&rq->nr_running, 1, __ATOMIC_RELAXED);

        if (t != proc_idle_task() && t->state == PROC_READY && t->kstack
            && !t->cg_throttled) {
            t->dispatching = 1;
            t->owner_cpu = cpu;
            RUNQ_UNLOCK_IRQ(cpu, rf);
            return t;
        }
        /* The removed runqueue reference was not transferred to dispatch. */
        proc_put(t);
    }

    RUNQ_UNLOCK_IRQ(cpu, rf);
    return NULL;
}

static void sched_scan_timers(uint64_t now)
{
    int scan_alarms =
        now >= __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED);
    if (scan_alarms)
        __atomic_exchange_n(&next_alarm_scan, SCHED_NO_DEADLINE,
                            __ATOMIC_RELAXED);

    uint64_t next_alarm = SCHED_NO_DEADLINE;
    int sigalrm_pids[SCHED_SIGNAL_BATCH];
    int sigalrm_count = 0;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    while (wait_timer_count && wait_timer_heap[0].deadline <= now) {
        wait_timer_t timer = wait_timer_remove_index_locked(0);
        if (timer.task && timer.wait_seq) {
            timer.task->sched_level = 0;
            (void)proc_try_wake_locked(timer.task, timer.wait_seq,
                                       PROC_WAKE_TIMEOUT);
        }
        proc_put(timer.task);
    }

    if (scan_alarms) {
        for (task_t *t = proc_first_task_locked(); t;
             t = proc_next_task_locked(t)) {
            if (t->state == PROC_UNUSED)
                continue;

            uint64_t alarm =
                __atomic_load_n(&t->alarm_expire, __ATOMIC_RELAXED);
            if (alarm > 0) {
                if (now >= alarm) {
                    uint64_t interval = t->itimer_real_interval;
                    alarm = interval ? now + interval : 0;
                    __atomic_store_n(&t->alarm_expire, alarm,
                                     __ATOMIC_RELAXED);
                    if (sigalrm_count <
                        (int)(sizeof(sigalrm_pids) /
                              sizeof(sigalrm_pids[0])))
                        sigalrm_pids[sigalrm_count++] = t->pid;
                }
                if (alarm > 0 && alarm < next_alarm)
                    next_alarm = alarm;
            }
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    for (int i = 0; i < sigalrm_count; i++)
        signal_send(sigalrm_pids[i], SIGALRM);

#ifdef CONFIG_ABI_LINUX
    extern void posix_timer_tick(void);
    posix_timer_tick();
#endif

#ifdef CONFIG_ABI_NATIVE
    a20_timer_tick();
#endif

    if (scan_alarms &&
        sched_note_deadline(&next_alarm_scan, next_alarm))
        sched_rearm_timer();
}

/* Scan for reapable zombies — called from idle loop, not hot path.
 *
 * Safely reaps orphaned zombies (parent=idle, ppid=0, CLONE_THREAD,
 * or SIGCHLD ignored).  All work is done under proc_lock to prevent
 * races with proc_wait4() which may reap the same zombie.
 */
void sched_reap_zombies(void)
{
    task_t *to_reap[SCHED_REAP_BATCH];
    int count;

    do {
        count = 0;
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        task_t *current = proc_current();
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (t == proc_idle_task() || t == current ||
                proc_task_is_current_any_cpu(t))
                continue;
            if (t->state != PROC_ZOMBIE)
                continue;
            task_t *parent = t->parent;
            int reap = 0;
            if (!parent || parent == proc_idle_task() ||
                t->ppid == 0 || (t->clone_flags & CLONE_THREAD))
                reap = 1;
            else if (parent->signals) {
                signal_state_t *ss = (signal_state_t *)parent->signals;
                sigaction_t *act = &ss->actions[SIGCHLD];
                if (act->sa_handler == SIG_IGN || (act->sa_flags & SA_NOCLDWAIT))
                    reap = 1;
            }
            if (reap && count < (int)(sizeof(to_reap) / sizeof(to_reap[0]))) {
                task_t *owned = proc_get(t);
                if (owned)
                    to_reap[count++] = owned;
            }
        }

        /* Reserve and detach the zombies while still holding proc_lock.
         * proc_destroy_task() reacquires proc_lock, so destruction itself must
         * happen after unlock, but the nodes must no longer be reachable from
         * the global task list during that window. */
        for (int i = 0; i < count; i++) {
            to_reap[i]->state = PROC_UNUSED;
            proc_unlink_task_locked(to_reap[i]);
        }

        spin_unlock_irqrestore(&proc_lock, flags);

        for (int i = 0; i < count; i++) {
            proc_destroy_task(to_reap[i]);
            proc_put(to_reap[i]);
        }
    } while (count > 0);
}

void proc_sched_note_zombie(void)
{
    __atomic_store_n(&sched_zombies_pending, 1, __ATOMIC_RELEASE);
}

void context_switch(task_t *next) {
    if (!next || !next->kstack)
        return;

    uint64_t now = timer_get_ticks();

    task_t *prev = proc_current();
    if (prev && prev->cgroup && prev->cg_cpu_start > 0) {
        uint64_t elapsed_ticks = now - prev->cg_cpu_start;
        uint64_t elapsed_ns = elapsed_ticks * 1000000000ULL / TICKS_PER_SEC;
        int throttled = cg_cpu_account(prev->cgroup, elapsed_ns, now);
        if (throttled)
            prev->cg_throttled = 1;
        if (prev->cgroup)
            cg_cpu_check_unthrottle(prev->cgroup, now);
    }

    next->cg_cpu_start = now;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    unsigned cpu = cpu_current_id();
    if (next == proc_current()) {
        int had_dispatch_ref = next->dispatching;
        next->state = PROC_RUNNING;
        next->on_rq = 0;
        next->dispatching = 0;
        next->on_cpu = 1;
        next->owner_cpu = cpu;
        proc_sched_assert_task_locked(next);
        if (had_dispatch_ref)
            proc_put(next);
        spin_unlock_irqrestore(&proc_lock, flags);
        return;
    }
#if CONFIG_DEBUG_SCHED_STATE
    if (next != proc_idle_task() &&
        (!next->dispatching || next->on_cpu ||
         next->owner_cpu != cpu))
        panic("context switch: next=%d dispatch=%d on_cpu=%d owner=%u cpu=%u",
              next->pid, next->dispatching, next->on_cpu,
              next->owner_cpu, cpu);
    if (next->on_rq)
        panic("context switch: queued next pid=%d", next->pid);
#endif

    /*
     * Publish the current slot before dispatching -> on_cpu. Remote observers
     * holding proc_lock therefore see either a selected task or an owned task,
     * never an unowned dequeue gap.
     */
    int had_dispatch_ref = next->dispatching;
    task_t *old = proc_set_current(next);
    next->state  = PROC_RUNNING;
    next->on_rq  = 0;
    next->dispatching = 0;
    next->on_cpu = 1;
    next->owner_cpu = cpu;
    proc_sched_assert_task_locked(next);
    if (had_dispatch_ref)
        proc_put(next);
    spin_unlock_irqrestore(&proc_lock, flags);
    if (prev && prev->pid >= 4 && next->pid >= 4)
        ktrace_sched("[SCHED] ctxsw: %d -> %d\n", prev->pid, next->pid);
    if (old)
        arch_set_task_pointer(old);
    __switch(next->kstack);
    proc_switch_complete();
}

void sched(void) {
    task_t *sched_owner = proc_current();
    ARCH_SCHED_ENTER(sched_owner);
    uint64_t now = timer_get_ticks();

    /* Run event-driven network bottom-halves before picking the next task.
     * This replaces the old generic block/network polling hot path. */
    kernel_progress_run_bottom_halves();

    /* Thread-heavy workloads can keep waking a parent before idle runs.  Reap
     * older auto-reap zombies here, but never the current task: an exiting
     * current task is still running on its own kernel stack until switch-out. */
    if (__atomic_exchange_n(&sched_zombies_pending, 0, __ATOMIC_ACQ_REL))
        sched_reap_zombies();

    /* Timer scanning: only scan when a deadline has actually been reached,
     * avoiding O(n) traversal on every sched() call. */
    if (now >= __atomic_load_n(&next_wake_scan, __ATOMIC_RELAXED) ||
        now >= __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED))
        sched_scan_timers(now);

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *next = proc_runq_pick_locked();
    if (next)
        proc_sched_assert_task_locked(next);
    spin_unlock_irqrestore(&proc_lock, flags);

    if (next) {
        next->exec_start = now;
        context_switch(next);
        goto out;
    }

    /*
     * A wake can race after the empty runqueue pick while the blocked task is
     * still executing on this sched() stack. Recheck under proc_lock, which
     * serializes against proc_try_wake(). If that late wake published
     * READY + on_rq, consume its queue entry before allowing the current task
     * to continue as RUNNING.
     */
    int keep_current = 0;
    flags = spin_lock_irqsave(&proc_lock);
    task_t *cur = proc_current();
    if (cur && (cur->state == PROC_READY || cur->state == PROC_RUNNING)) {
        if (cur->on_rq)
            proc_runq_remove_locked(cur);
        cur->state = PROC_RUNNING;
        proc_sched_assert_task_locked(cur);
        keep_current = 1;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    if (keep_current)
        goto out;

    task_t *idle = proc_idle_task();
    if (cur != idle) {
        if (cur && cur->pid >= 4)
            ktrace_sched("[SCHED] fall-to-idle: cur=%d state=%d\n", cur->pid, cur->state);
        context_switch(idle);
    }

out:
    /* A switched-out task returns here only when that same task is resumed. */
    ARCH_SCHED_LEAVE(sched_owner);
}

void proc_yield(void) {
    task_t *cur = proc_current();
    task_t *idle = proc_idle_task();
    if (cur && cur != idle && cur->state == PROC_RUNNING) {
        /* Only demote to a *lower* priority level (higher number) after the
         * task has used a full time-slice worth of CPU time at its current
         * level.  A single 10 ms preemption tick is not enough to justify
         * demotion — the task may just be doing brief work between I/O. */
        uint64_t now = timer_get_ticks();
        uint64_t elapsed = now - cur->exec_start;
        uint64_t slice = TICKS_PER_SEC / 100;
        if (!sched_task_rt(cur) && elapsed >= slice &&
            cur->sched_level < SCHED_LEVELS - 1)
            cur->sched_level++;
        if (cur->pid >= 4)
            ktrace_sched("[SCHED] yield: pid=%d\n", cur->pid);
        proc_make_ready(cur);
    }
    sched();
}
