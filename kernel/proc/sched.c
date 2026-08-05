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
#endif

typedef struct __attribute__((aligned(64))) proc_runq {
    spinlock_t lock;
    task_t *head[SCHED_LEVELS];
    task_t *tail[SCHED_LEVELS];
    uint32_t bitmap;
    unsigned nr_running;
    uint64_t eevdf_vtime;      /* system virtual time (advances with run) */
    uint64_t eevdf_weight;     /* sum of weights of runnable EEVDF tasks */
    unsigned long lock_acquires;
    unsigned long lock_contentions;
} proc_runq_t;

typedef struct __attribute__((aligned(64))) proc_cpu_sched {
    unsigned need_resched;
    unsigned long requests;
    unsigned long priority_requests;
    unsigned long ipi_sent;
    unsigned long ipi_acks;
    unsigned long consumed;
} proc_cpu_sched_t;

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
 *   runnable task; proc_runq_pick_local() must atomically move on_rq to
 *   dispatching before that point.
 * - Timer wake, signal wake, futex wake, wait queue wake, fork/exec/wait, and
 *   exit/reap must all preserve TASK_STATE_MUTATION_CONTRACT before NR_CPUS>1
 *   may be enabled without ALLOW_UNVERIFIED_SMP.
 */

static proc_runq_t sched_runq[CONFIG_NR_CPUS];
static proc_cpu_sched_t sched_cpu[CONFIG_NR_CPUS];
static unsigned sched_zombies_pending;
static unsigned long sched_runqueue_migrations;
static unsigned long sched_violations;
static unsigned long sched_local_picks;
static unsigned long sched_empty_picks;
static unsigned long sched_local_pick_active;
static unsigned long sched_local_pick_parallel_peak;

/*
 * SCHEDULER_CPU_OWNERSHIP:
 *   on_rq -> dispatching -> on_cpu -> unowned
 *
 * proc_lock serializes task-state and CPU ownership transitions except for the
 * local on_rq -> dispatching hand-off. That hand-off changes queue membership
 * and dispatch ownership atomically under the selected per-CPU runqueue lock,
 * without holding proc_lock. All enqueue, migration, unpick, switch publication,
 * Park, exit, and reap paths retain proc_lock -> runqueue lock ordering. An
 * outgoing task keeps on_cpu set across __switch; proc_switch_complete() clears
 * ownership only after the replacement task is executing on its own stack.
 */





unsigned proc_sched_select_cpu(task_t *t);


#ifdef CONFIG_MCU
#define SCHED_REAP_BATCH   8
#else
#define SCHED_REAP_BATCH   32
#endif

/* Per-CPU runqueue lock — separate from proc_lock.
 * runq_lock protects enqueue/dequeue/pick and per-runqueue state.
 * proc_lock protects task_list, task->state transitions, and zombie list.
 *
 * Ordering: proc_lock -> runq_lock (never the reverse). A local picker acquires
 * only its runqueue lock and releases it before acquiring proc_lock to publish
 * the selected task. */
static uint64_t sched_runq_lock_irq(unsigned cpu)
{
    proc_runq_t *rq = &sched_runq[cpu];
    int contended =
        __atomic_load_n(&rq->lock.locked, __ATOMIC_RELAXED) != 0;
    uint64_t flags = spin_lock_irqsave(&rq->lock);
    __atomic_fetch_add(&rq->lock_acquires, 1, __ATOMIC_RELAXED);
    if (contended)
        __atomic_fetch_add(&rq->lock_contentions, 1, __ATOMIC_RELAXED);
    return flags;
}

static void sched_runq_unlock_irq(unsigned cpu, uint64_t flags)
{
    spin_unlock_irqrestore(&sched_runq[cpu].lock, flags);
}

#define RUNQ_LOCK_IRQ(cpu) sched_runq_lock_irq(cpu)
#define RUNQ_UNLOCK_IRQ(cpu, f) sched_runq_unlock_irq((cpu), (f))

static int sched_level_clamp(int level) {
    if (level < 0) return 0;
    if (level >= SCHED_LEVELS) return SCHED_LEVELS - 1;
    return level;
}

static int sched_task_rt(task_t *t)
{
    return t && (t->sched_policy == SCHED_FIFO || t->sched_policy == SCHED_RR);
}

static int sched_task_strictly_preempts(task_t *candidate, task_t *running)
{
    if (!candidate || !running || candidate == running)
        return 0;

    int candidate_rt = sched_task_rt(candidate);
    int running_rt = sched_task_rt(running);
    if (candidate_rt != running_rt)
        return candidate_rt;
    if (candidate_rt)
        return candidate->priority > running->priority;
    /* EEVDF: an earlier virtual deadline strictly outranks. */
    return candidate->eevdf_deadline < running->eevdf_deadline;
}

/* ================================================================
 * EEVDF proportional-share core
 *
 * Normal tasks are served in Earliest-Eligible-Virtual-Deadline-First order.
 * Each task accumulates a weighted virtual run time
 * (`vruntime += dt * NICE0 / weight`), so a heavier (lower nice) task accrues
 * virtual time more slowly and is entitled to more real CPU.  The runqueue
 * also tracks a system virtual time `eevdf_vtime` that advances at the
 * aggregate rate `dt * NICE0 / total_weight`; a task is *eligible* when
 * `vruntime <= vtime` (it has not yet consumed more than its fair share).
 * Among eligible tasks the earliest `deadline = vruntime + virtual slice` is
 * picked, which is what gives short-slice (latency sensitive) tasks an early
 * turn without letting them exceed their weighted share.
 *
 * A freshly-woken task keeps its historical vruntime (sleeper bonus), but the
 * bonus is clamped to EEVDF_MAX_LAG so a task that sleeps for a long time
 * cannot then hog the CPU.
 *
 * RT tasks stay on runqueue level 0 with their existing fixed-priority
 * FIFO/RR semantics and are never charged virtual time, so they always run
 * ahead of the EEVDF list on level 1.
 */
#define EEVDF_LEVEL           1     /* runqueue slot holding the EEVDF list */
#define EEVDF_NICE0_LOAD      1024UL
#define EEVDF_BASE_SLICE      (TICKS_PER_SEC / 100)   /* 10 ms base slice */
#define EEVDF_MAX_LAG         (TICKS_PER_SEC / 100)   /* max sleeper bonus */

/* Schedulable base slice (ms), writable via /proc/a20/sched_base_slice. */
extern int g_sched_base_slice_ms;

static inline uint64_t eevdf_weight(task_t *t)
{
    uint32_t w = t->cfs_weight;
    return w ? (uint64_t)w : EEVDF_NICE0_LOAD;
}

static inline uint64_t eevdf_vslice(task_t *t)
{
    uint64_t slice = EEVDF_BASE_SLICE;
    int ms = g_sched_base_slice_ms;
    if (ms > 0)
        slice = (uint64_t)ms * (TICKS_PER_SEC / 1000);
    return (slice * EEVDF_NICE0_LOAD) / eevdf_weight(t);
}

static void eevdf_reset_deadline(task_t *t)
{
    t->eevdf_deadline = t->eevdf_vruntime + eevdf_vslice(t);
}

static void eevdf_account_run(task_t *t, uint64_t dt)
{
    t->eevdf_vruntime += (dt * EEVDF_NICE0_LOAD) / eevdf_weight(t);
    t->eevdf_deadline = t->eevdf_vruntime + eevdf_vslice(t);
}

static inline int eevdf_eligible(proc_runq_t *rq, task_t *t)
{
    return t->eevdf_vruntime <= rq->eevdf_vtime;
}

/* Charge `now - last_account` of run time to t and advance system vtime. */
static void eevdf_charge(proc_runq_t *rq, task_t *t, uint64_t now)
{
    if (!t || sched_task_rt(t) || t->pid == 0)
        return;
    uint64_t dt = now - t->eevdf_last_account;
    t->eevdf_last_account = now;
    if (!dt)
        return;
    eevdf_account_run(t, dt);
    uint64_t total = rq->eevdf_weight;
    if (total)
        rq->eevdf_vtime += (dt * EEVDF_NICE0_LOAD) / total;
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

/* Insert an EEVDF task into level 1, keeping the list sorted by deadline. */
static void sched_runq_eevdf_insert(proc_runq_t *rq, task_t *t)
{
    if (t->eevdf_deadline == 0)
        eevdf_reset_deadline(t);

    /* Clamp the sleeper bonus so a long-sleeper cannot hog the CPU. */
    if (rq->head[EEVDF_LEVEL] &&
        t->eevdf_vruntime + EEVDF_MAX_LAG < rq->eevdf_vtime) {
        t->eevdf_vruntime = rq->eevdf_vtime - EEVDF_MAX_LAG;
        eevdf_reset_deadline(t);
    }

    /* First EEVDF task on this CPU defines the system virtual time. */
    if (!rq->head[EEVDF_LEVEL])
        rq->eevdf_vtime = t->eevdf_vruntime;

    task_t *it = rq->head[EEVDF_LEVEL];
    while (it && it->eevdf_deadline <= t->eevdf_deadline)
        it = it->rq_next;

    t->rq_next = it;
    t->rq_prev = it ? it->rq_prev : rq->tail[EEVDF_LEVEL];
    if (t->rq_prev)
        t->rq_prev->rq_next = t;
    else
        rq->head[EEVDF_LEVEL] = t;
    if (it)
        it->rq_prev = t;
    else
        rq->tail[EEVDF_LEVEL] = t;
    rq->bitmap |= (1U << EEVDF_LEVEL);
    rq->eevdf_weight += eevdf_weight(t);
}

static void sched_runq_eevdf_del_weight(proc_runq_t *rq, task_t *t)
{
    uint64_t w = eevdf_weight(t);
    rq->eevdf_weight = (rq->eevdf_weight > w) ? rq->eevdf_weight - w : 0;
}

void proc_sched_runq_init(void) {
    memset(sched_runq, 0, sizeof(sched_runq));
    memset(sched_cpu, 0, sizeof(sched_cpu));
    for (unsigned i = 0; i < CONFIG_NR_CPUS; i++)
        spin_init(&sched_runq[i].lock);
    sched_runqueue_migrations = 0;
    sched_violations = 0;
    sched_local_picks = 0;
    sched_empty_picks = 0;
    sched_local_pick_active = 0;
    sched_local_pick_parallel_peak = 0;
    proc_timer_heap_init();
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

/*
 * SMP_RUNQUEUE_MIGRATION_PROTOCOL:
 * A queued task moves from source to destination while proc_lock and both
 * runqueue locks are held. Runqueue locks are always acquired by ascending CPU
 * number and released in reverse order. cpu_id changes only during the
 * on_rq=0 interval protected by those locks; the runqueue-owned task reference
 * is transferred without a put/get gap.
 */
static void sched_runq_requeue_locked(task_t *t, unsigned dst_cpu,
                                      int old_level)
{
    if (!t || !t->on_rq || t->state != PROC_READY ||
        t->cpu_id >= CONFIG_NR_CPUS || dst_cpu >= CONFIG_NR_CPUS) {
        __atomic_fetch_add(&sched_violations, 1, __ATOMIC_RELAXED);
        return;
    }

    unsigned src_cpu = t->cpu_id;
    unsigned first = src_cpu < dst_cpu ? src_cpu : dst_cpu;
    unsigned second = src_cpu < dst_cpu ? dst_cpu : src_cpu;
    uint64_t first_flags = RUNQ_LOCK_IRQ(first);
    uint64_t second_flags = 0;
    if (second != first)
        second_flags = RUNQ_LOCK_IRQ(second);

    proc_runq_t *src = &sched_runq[src_cpu];
    proc_runq_t *dst = &sched_runq[dst_cpu];
    sched_runq_unlink_at(src, t, sched_level_clamp(old_level));
    if (sched_level_clamp(old_level) == EEVDF_LEVEL)
        sched_runq_eevdf_del_weight(src, t);
    t->on_rq = 0;
    if (src_cpu != dst_cpu) {
        if (__atomic_load_n(&src->nr_running, __ATOMIC_RELAXED) > 0)
            __atomic_fetch_sub(&src->nr_running, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&dst->nr_running, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&sched_runqueue_migrations, 1,
                           __ATOMIC_RELAXED);
    }

    int new_level = sched_task_rt(t) ? 0 : EEVDF_LEVEL;
    t->cpu_id = dst_cpu;
    t->sched_level = new_level;
    t->ready_since = timer_get_ticks();
    if (new_level == 0)
        sched_runq_append_at(dst, t, 0);
    else
        sched_runq_eevdf_insert(dst, t);
    t->on_rq = 1;

    if (second != first)
        RUNQ_UNLOCK_IRQ(second, second_flags);
    RUNQ_UNLOCK_IRQ(first, first_flags);
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
    unsigned old_cpu = t->cpu_id;
    int old_level = t->sched_level;
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
        if (!sched_policy_rt_value(policy))
            eevdf_reset_deadline(t);
    }
    if (config->fields & PROC_SCHED_AFFINITY) {
        t->cpus_allowed = config->affinity & SCHED_CPU_MASK_ALL;
    }

    unsigned target_cpu = t->cpu_id;
    if (!t->on_cpu && !t->dispatching) {
        uint32_t eligible = sched_task_cpu_mask(t);
        if (target_cpu >= 32 || !(eligible & (1U << target_cpu)))
            target_cpu = proc_sched_select_cpu_locked(t);
    }

    int queue_fields_changed =
        (config->fields &
         (PROC_SCHED_POLICY | PROC_SCHED_PRIORITY | PROC_SCHED_NICE)) != 0;
    if (ready && (queue_fields_changed || target_cpu != old_cpu))
        sched_runq_requeue_locked(t, target_cpu, old_level);
    else if (!ready && !t->on_cpu && !t->dispatching && !t->on_rq)
        t->cpu_id = target_cpu;

    int queued = t->on_rq;
    int priority_preempt =
        queued && proc_sched_should_preempt_locked(t, target_cpu);
    proc_sched_assert_task_locked(t);
    spin_unlock_irqrestore(&proc_lock, lock_flags);

    if (queued &&
        (target_cpu != cpu_current_id() || priority_preempt))
        proc_sched_request_cpu(target_cpu, priority_preempt);
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

int proc_sched_should_preempt_locked(task_t *t, unsigned cpu)
{
    if (!t || cpu >= CONFIG_NR_CPUS)
        return 0;

    task_t *running = proc_current_on_cpu(cpu);
    if (!running || running->pid == 0)
        return 1;
    if (running == t || running->state != PROC_RUNNING)
        return 0;

    return sched_task_strictly_preempts(t, running);
}

void proc_sched_request_cpu(unsigned cpu, int priority)
{
    if (cpu >= CONFIG_NR_CPUS || !smp_cpu_is_online(cpu)) {
        __atomic_fetch_add(&sched_violations, 1, __ATOMIC_RELAXED);
        return;
    }

    proc_cpu_sched_t *state = &sched_cpu[cpu];
    __atomic_fetch_add(&state->requests, 1, __ATOMIC_RELAXED);
    if (priority)
        __atomic_fetch_add(&state->priority_requests, 1, __ATOMIC_RELAXED);

    unsigned already_pending =
        __atomic_exchange_n(&state->need_resched, 1, __ATOMIC_RELEASE);
    if (cpu == cpu_current_id() || already_pending)
        return;

    __atomic_fetch_add(&state->ipi_sent, 1, __ATOMIC_RELAXED);
    smp_send_reschedule(cpu);
}

void proc_sched_kick_cpu(unsigned cpu)
{
    proc_sched_request_cpu(cpu, 0);
}

void proc_sched_request_current(void)
{
    proc_sched_request_cpu(cpu_current_id(), 0);
}

void proc_sched_handle_reschedule_ipi(void)
{
    unsigned cpu = cpu_current_id();
    if (cpu >= CONFIG_NR_CPUS) {
        __atomic_fetch_add(&sched_violations, 1, __ATOMIC_RELAXED);
        return;
    }
    __atomic_fetch_add(&sched_cpu[cpu].ipi_acks, 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void proc_sched_tick(int from_user)
{
    task_t *cur = proc_current();
    if (!cur)
        return;
    if (from_user)
        cur->total_time++;
    if (cur->pid == 0 || cur->state != PROC_RUNNING)
        return;

    uint64_t now = timer_get_ticks();
    eevdf_charge(&sched_runq[cpu_current_id()], cur, now);

    uint64_t slice = EEVDF_BASE_SLICE;
    if (slice == 0)
        slice = 1;
    if (now - cur->exec_start >= slice)
        proc_sched_request_cpu(cpu_current_id(), 0);
}

int proc_sched_safe_point(void)
{
    unsigned cpu = cpu_current_id();
    if (cpu >= CONFIG_NR_CPUS ||
        !__atomic_load_n(&sched_cpu[cpu].need_resched, __ATOMIC_ACQUIRE))
        return 0;
    proc_yield();
    return 1;
}

static void sched_consume_resched(unsigned cpu)
{
    if (cpu >= CONFIG_NR_CPUS)
        return;
    if (__atomic_exchange_n(&sched_cpu[cpu].need_resched, 0,
                            __ATOMIC_ACQ_REL))
        __atomic_fetch_add(&sched_cpu[cpu].consumed, 1,
                           __ATOMIC_RELAXED);
}

void proc_sched_diag_snapshot(proc_sched_diag_t *diag)
{
    if (!diag)
        return;
    memset(diag, 0, sizeof(*diag));
    diag->runqueue_migrations =
        __atomic_load_n(&sched_runqueue_migrations, __ATOMIC_RELAXED);
    diag->scheduler_violations =
        __atomic_load_n(&sched_violations, __ATOMIC_RELAXED);
    diag->runqueue_local_picks =
        __atomic_load_n(&sched_local_picks, __ATOMIC_RELAXED);
    diag->runqueue_empty_picks =
        __atomic_load_n(&sched_empty_picks, __ATOMIC_RELAXED);
    diag->runqueue_parallel_pick_peak =
        __atomic_load_n(&sched_local_pick_parallel_peak, __ATOMIC_RELAXED);
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        proc_runq_t *rq = &sched_runq[cpu];
        proc_cpu_sched_t *state = &sched_cpu[cpu];
        diag->runqueue_lock_acquires +=
            __atomic_load_n(&rq->lock_acquires, __ATOMIC_RELAXED);
        diag->runqueue_lock_contentions +=
            __atomic_load_n(&rq->lock_contentions, __ATOMIC_RELAXED);
        diag->resched_requests +=
            __atomic_load_n(&state->requests, __ATOMIC_RELAXED);
        diag->resched_priority_requests +=
            __atomic_load_n(&state->priority_requests, __ATOMIC_RELAXED);
        diag->resched_ipi_sent +=
            __atomic_load_n(&state->ipi_sent, __ATOMIC_RELAXED);
        diag->resched_ipi_acks +=
            __atomic_load_n(&state->ipi_acks, __ATOMIC_RELAXED);
        diag->resched_consumed +=
            __atomic_load_n(&state->consumed, __ATOMIC_RELAXED);
        diag->resched_pending +=
            !!__atomic_load_n(&state->need_resched, __ATOMIC_ACQUIRE);
    }
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
    if (on_rq &&
        (task_cpu >= 64 || membership_cpus != (1ULL << task_cpu)))
        panic("sched invariant: pid=%d cpu_id=%u membership_cpus=0x%lx",
              t->pid, task_cpu, (unsigned long)membership_cpus);
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

uint64_t proc_sched_task_runq_cpu_mask_locked(task_t *t)
{
    if (!t)
        return 0;

    uint64_t rq_flags[CONFIG_NR_CPUS];
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++)
        rq_flags[cpu] = RUNQ_LOCK_IRQ(cpu);

    uint64_t mask = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && cpu < 64; cpu++) {
        proc_runq_t *rq = &sched_runq[cpu];
        for (int level = 0; level < SCHED_LEVELS; level++) {
            for (task_t *it = rq->head[level]; it; it = it->rq_next) {
                if (it == t)
                    mask |= 1ULL << cpu;
            }
        }
    }

    for (unsigned cpu = CONFIG_NR_CPUS; cpu > 0; cpu--)
        RUNQ_UNLOCK_IRQ(cpu - 1, rq_flags[cpu - 1]);
    return mask;
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
    int priority_preempt =
        queued && proc_sched_should_preempt_locked(t, target_cpu);
    proc_sched_assert_task_locked(t);
    spin_unlock_irqrestore(&proc_lock, flags);

    if (queued &&
        (target_cpu != cpu_current_id() || priority_preempt))
        proc_sched_request_cpu(target_cpu, priority_preempt);
}

void proc_sched_stop_current(int exit_code)
{
    task_t *t = proc_current();
    if (!t || t == proc_idle_task())
        return;

    task_t *notify_parent = NULL;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    /*
     * SIGCONT generation publishes a persistent pending marker before it
     * attempts to resume STOPPED.  If it raced the stop transition and found
     * the task still RUNNING, consume that fact here by declining to publish
     * STOPPED.
     */
    if (__atomic_load_n(&t->exit_pending, __ATOMIC_ACQUIRE) ||
        signal_task_has_fatal(t) ||
        signal_task_continue_pending(t)) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return;
    }
    if (t->state == PROC_RUNNING) {
        t->exit_code = exit_code;
        t->stop_report_pending = 1;
        t->continue_report_pending = 0;
        t->state = PROC_STOPPED;
        task_t *parent = t->parent;
        if (parent && parent->state != PROC_UNUSED &&
            parent->state != PROC_ZOMBIE) {
            proc_wake_child_waiters_locked(parent);
            if (!signal_task_sigchld_no_cldstop(parent))
                notify_parent = proc_get(parent);
        }
        proc_sched_assert_task_locked(t);
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    if (notify_parent) {
        (void)signal_send(notify_parent->pid, SIGCHLD);
        proc_put(notify_parent);
    }
    sched();
}

int proc_sched_resume_stopped(task_t *t, int report_continued)
{
    if (!t)
        return 0;

    unsigned target_cpu = cpu_current_id();
    int queued = 0;
    int resumed = 0;
    task_t *notify_parent = NULL;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state == PROC_STOPPED) {
        t->state = PROC_READY;
        t->stop_report_pending = 0;
        t->continue_report_pending = report_continued != 0;
        if (t->on_cpu) {
            t->cpu_id = t->owner_cpu;
        } else if (!t->dispatching && !t->on_rq) {
            t->cpu_id = proc_sched_select_cpu_locked(t);
        }
        target_cpu = t->cpu_id;
        proc_runq_enqueue_locked(t);
        queued = t->on_rq;
        resumed = 1;

        if (report_continued) {
            task_t *parent = t->parent;
            if (parent && parent->state != PROC_UNUSED &&
                parent->state != PROC_ZOMBIE) {
                proc_wake_child_waiters_locked(parent);
                if (!signal_task_sigchld_no_cldstop(parent))
                    notify_parent = proc_get(parent);
            }
        }
        proc_sched_assert_task_locked(t);
    }
    int priority_preempt =
        queued && proc_sched_should_preempt_locked(t, target_cpu);
    spin_unlock_irqrestore(&proc_lock, flags);

    if (queued &&
        (target_cpu != cpu_current_id() || priority_preempt))
        proc_sched_request_cpu(target_cpu, priority_preempt);
    if (notify_parent) {
        (void)signal_send(notify_parent->pid, SIGCHLD);
        proc_put(notify_parent);
    }
    return resumed;
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

    int q = sched_task_rt(t) ? 0 : EEVDF_LEVEL;
    if (!proc_get(t)) {
        RUNQ_UNLOCK_IRQ(cpu, rf);
        return;
    }
    t->sched_level = q;
    t->cpu_id = cpu;
    t->ready_since = timer_get_ticks();
    if (q == 0)
        sched_runq_append_at(rq, t, 0);
    else
        sched_runq_eevdf_insert(rq, t);
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
    if (q == EEVDF_LEVEL)
        sched_runq_eevdf_del_weight(rq, t);
    t->on_rq = 0;
    t->ready_since = 0;
    if (__atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED) > 0)
        __atomic_fetch_sub(&rq->nr_running, 1, __ATOMIC_RELAXED);
    RUNQ_UNLOCK_IRQ(cpu, rf);
    proc_put(t);
}

/*
 * Idle-task stealing: when the local runqueue is empty, pull a runnable task
 * from a remote CPU's runqueue so idle CPUs keep absorbing load (prevents
 * persistent per-CPU imbalance under bursty wakeups, e.g. Cargo spawning many
 * compiler threads).  The local runqueue lock is already held; each remote
 * queue is tried non-blockingly so an idle CPU never waits on a busy one.
 * The stolen runqueue reference is transferred to the local dispatch without
 * a put/get gap, matching the migration protocol.  Only EEVDF (normal) tasks
 * are stolen: RT tasks are deliberately placed by the wakeup path and must
 * not be yanked off their target CPU by an idle peer (keeps sched_setaffinity
 * and wake-affinity placement deterministic).  We prefer the least-urgent task
 * (latest EEVDF deadline) so the remote keeps its hot work.
 */
static task_t *sched_runq_steal_locked(proc_runq_t *lrq, unsigned local)
{
    for (unsigned remote = 0; remote < CONFIG_NR_CPUS; remote++) {
        if (remote == local || !smp_cpu_is_online(remote))
            continue;
        proc_runq_t *rrq = &sched_runq[remote];
        uint64_t rrf = 0;
        if (!spin_trylock_irqsave(&rrq->lock, &rrf))
            continue;

        /* Steal only a genuine surplus: keep at least one task on the remote
         * so a deliberately-placed single task is never yanked off its CPU. */
        if (__atomic_load_n(&rrq->nr_running, __ATOMIC_RELAXED) < 2) {
            spin_unlock_irqrestore(&rrq->lock, rrf);
            continue;
        }
        task_t *t = NULL;
        if (rrq->bitmap & (1U << EEVDF_LEVEL))
            t = rrq->tail[EEVDF_LEVEL];
        if (!t || t->state != PROC_READY || !t->on_rq || !t->kstack ||
            t->cg_throttled ||
            !(sched_task_cpu_mask(t) & (1U << local))) {
            spin_unlock_irqrestore(&rrq->lock, rrf);
            continue;
        }

        sched_runq_unlink_at(rrq, t, EEVDF_LEVEL);
        sched_runq_eevdf_del_weight(rrq, t);
        t->on_rq = 0;
        t->ready_since = 0;
        if (__atomic_load_n(&rrq->nr_running, __ATOMIC_RELAXED) > 0)
            __atomic_fetch_sub(&rrq->nr_running, 1, __ATOMIC_RELAXED);
        spin_unlock_irqrestore(&rrq->lock, rrf);

        /* Publish dispatch ownership under the (held) local runqueue lock. */
        t->cpu_id = local;
        t->dispatching = 1;
        t->owner_cpu = local;
        t->rq_next = NULL;
        t->rq_prev = NULL;
        __atomic_fetch_add(&sched_runqueue_migrations, 1, __ATOMIC_RELAXED);
        return t;
    }
    return NULL;
}

/*
 * SCHED_LOCAL_PICK_LOCK_SPLIT_BEGIN
 *
 * Pick the next task and transfer on_rq -> dispatching using only the current
 * CPU's runqueue lock. Queue membership, cpu_id, and the dispatch owner are
 * published as one local critical section. The caller acquires proc_lock only
 * after this function returns, to validate current-versus-next and publish the
 * context switch. A path holding a runqueue lock must never acquire proc_lock.
 */
task_t *proc_runq_pick_local(void)
{
    unsigned cpu = cpu_current_id();
    task_t *picked = NULL;
    unsigned long active =
        __atomic_add_fetch(&sched_local_pick_active, 1, __ATOMIC_ACQ_REL);
    unsigned long peak =
        __atomic_load_n(&sched_local_pick_parallel_peak, __ATOMIC_RELAXED);
    while (active > peak &&
           !__atomic_compare_exchange_n(&sched_local_pick_parallel_peak,
                                        &peak, active, 0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    __atomic_fetch_add(&sched_local_picks, 1, __ATOMIC_RELAXED);

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
        } else if (q == EEVDF_LEVEL) {
            /* Earliest eligible virtual deadline: skip over-run tasks. */
            task_t *eligible = NULL;
            for (task_t *it = rq->head[q]; it; it = it->rq_next) {
                if (eevdf_eligible(rq, it)) {
                    eligible = it;
                    break;
                }
            }
            if (eligible)
                t = eligible;
            /* else: keep the earliest deadline as a progress fallback */
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
        if (q == EEVDF_LEVEL)
            sched_runq_eevdf_del_weight(rq, t);
        if (__atomic_load_n(&rq->nr_running, __ATOMIC_RELAXED) > 0)
            __atomic_fetch_sub(&rq->nr_running, 1, __ATOMIC_RELAXED);

        if (t != proc_idle_task() && t->state == PROC_READY && t->kstack
            && !t->cg_throttled) {
            t->dispatching = 1;
            t->owner_cpu = cpu;
            picked = t;
            break;
        }
        /* The removed runqueue reference was not transferred to dispatch. */
        proc_put(t);
    }

    if (!picked)
        picked = sched_runq_steal_locked(rq, cpu);

    if (!picked)
        __atomic_fetch_add(&sched_empty_picks, 1, __ATOMIC_RELAXED);
    RUNQ_UNLOCK_IRQ(cpu, rf);
    __atomic_fetch_sub(&sched_local_pick_active, 1, __ATOMIC_RELEASE);
    return picked;
}
/* SCHED_LOCAL_PICK_LOCK_SPLIT_END */

/*
 * Reverse an unpublished on_rq -> dispatching transfer. This is needed when a
 * yielding current task still strictly outranks the selected task: the old
 * stack cannot relinquish CPU ownership before the replacement stack runs, so
 * retaining the current task avoids exposing a lower-priority interval.
 * Caller holds proc_lock and the dispatch reference becomes the runqueue
 * reference again without a put/get gap.
 */
static void sched_runq_unpick_locked(task_t *t)
{
    if (!t || !t->dispatching || t->on_rq || t->on_cpu ||
        t->owner_cpu >= CONFIG_NR_CPUS || t->state != PROC_READY) {
        __atomic_fetch_add(&sched_violations, 1, __ATOMIC_RELAXED);
        return;
    }

    unsigned cpu = t->owner_cpu;
    uint64_t rf = RUNQ_LOCK_IRQ(cpu);
    proc_runq_t *rq = &sched_runq[cpu];
    int q = sched_task_rt(t) ? 0 : sched_level_clamp(t->sched_level);
    t->dispatching = 0;
    t->owner_cpu = PROC_CPU_NONE;
    t->cpu_id = cpu;
    t->sched_level = q;
    t->ready_since = timer_get_ticks();
    sched_runq_append_at(rq, t, q);
    t->on_rq = 1;
    __atomic_fetch_add(&rq->nr_running, 1, __ATOMIC_RELAXED);
    RUNQ_UNLOCK_IRQ(cpu, rf);
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
            else if (signal_task_sigchld_auto_reap(parent))
                reap = 1;
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
    eevdf_charge(&sched_runq[cpu_current_id()], prev, now);
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
    ARCH_SCHED_SWITCH(next);
    __switch(next->kstack);
    proc_switch_complete();
}

void sched(void) {
    task_t *sched_owner = proc_current();
    ARCH_SCHED_ENTER(sched_owner);
    sched_consume_resched(cpu_current_id());
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
    if (proc_sched_timers_due(now))
        sched_scan_timers(now);

    /*
     * Local queue traversal and on_rq -> dispatching no longer serialize on
     * proc_lock across CPUs. The global lock is acquired only after the local
     * runqueue lock has been released, for state/ownership publication.
     */
    task_t *next = proc_runq_pick_local();
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    task_t *current = proc_current();
    if (next && current && current != proc_idle_task() &&
        current->state == PROC_READY && current->on_cpu &&
        current->owner_cpu == cpu_current_id() &&
        sched_task_strictly_preempts(current, next)) {
        /*
         * A READY current task is still the CPU owner until switch completion
         * and therefore cannot sit on the runqueue beside next. If it strictly
         * outranks next (not merely ties), retain it and return next atomically.
         */
        sched_runq_unpick_locked(next);
        current->state = PROC_RUNNING;
        next = NULL;
    }
    if (next)
        proc_sched_assert_task_locked(next);
    spin_unlock_irqrestore(&proc_lock, flags);

    if (next) {
        next->exec_start = now;
        next->eevdf_last_account = now;
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
        uint64_t now = timer_get_ticks();
        eevdf_charge(&sched_runq[cpu_current_id()], cur, now);
        if (cur->pid >= 4)
            ktrace_sched("[SCHED] yield: pid=%d\n", cur->pid);
        proc_make_ready(cur);
    }
    sched();
}
