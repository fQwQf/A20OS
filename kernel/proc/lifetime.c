#include "proc/lifetime.h"

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/string.h"
#include "core/stdio.h"

static unsigned long g_task_objects;
static unsigned long g_task_refs;
static unsigned long g_pid_entries;
static unsigned long g_wait_entries;
static unsigned long g_wake_entries;
static unsigned long g_ref_get_failures;
static unsigned long g_ref_underflows;
static unsigned long g_duplicate_destroy;
static unsigned long g_bad_final_put;

static unsigned long counter_read(const unsigned long *counter)
{
    return __atomic_load_n(counter, __ATOMIC_RELAXED);
}

static void counter_inc(unsigned long *counter)
{
    __atomic_fetch_add(counter, 1, __ATOMIC_RELAXED);
}

static void counter_dec(unsigned long *counter)
{
    unsigned long old = __atomic_load_n(counter, __ATOMIC_RELAXED);
    while (old > 0 &&
           !__atomic_compare_exchange_n(counter, &old, old - 1, 0,
                                        __ATOMIC_RELAXED,
                                        __ATOMIC_RELAXED)) {
    }
    if (old == 0)
        counter_inc(&g_ref_underflows);
}

void proc_lifetime_note_task_init(int dynamic)
{
    if (dynamic)
        counter_inc(&g_task_objects);
    counter_inc(&g_task_refs);
}

void proc_lifetime_note_task_free(void)
{
    counter_dec(&g_task_objects);
}

void proc_lifetime_note_ref_get(void)
{
    counter_inc(&g_task_refs);
}

void proc_lifetime_note_ref_put(void)
{
    counter_dec(&g_task_refs);
}

void proc_lifetime_note_ref_get_failure(void)
{
    counter_inc(&g_ref_get_failures);
}

void proc_lifetime_note_ref_underflow(void)
{
    counter_inc(&g_ref_underflows);
}

void proc_lifetime_note_duplicate_destroy(void)
{
    counter_inc(&g_duplicate_destroy);
}

void proc_lifetime_note_bad_final_put(void)
{
    counter_inc(&g_bad_final_put);
}

void proc_lifetime_note_pid_add(void)
{
    counter_inc(&g_pid_entries);
}

void proc_lifetime_note_pid_remove(void)
{
    counter_dec(&g_pid_entries);
}

void proc_lifetime_note_wait_add(void)
{
    counter_inc(&g_wait_entries);
}

void proc_lifetime_note_wait_remove(void)
{
    counter_dec(&g_wait_entries);
}

void proc_lifetime_note_wait_to_wake(void)
{
    counter_dec(&g_wait_entries);
    counter_inc(&g_wake_entries);
}

void proc_lifetime_note_wake_remove(void)
{
    counter_dec(&g_wake_entries);
}

void proc_lifetime_snapshot(proc_lifetime_stats_t *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));

    stats->task_objects = counter_read(&g_task_objects);
    stats->task_refs = counter_read(&g_task_refs);
    stats->pid_entries = counter_read(&g_pid_entries);
    stats->wait_entries = counter_read(&g_wait_entries);
    stats->wake_entries = counter_read(&g_wake_entries);
    stats->ref_get_failures = counter_read(&g_ref_get_failures);
    stats->ref_underflows = counter_read(&g_ref_underflows);
    stats->duplicate_destroy = counter_read(&g_duplicate_destroy);
    stats->bad_final_put = counter_read(&g_bad_final_put);

    proc_sched_diag_t sched_diag;
    proc_sched_diag_snapshot(&sched_diag);
    stats->runqueue_migrations = sched_diag.runqueue_migrations;
    stats->runqueue_local_picks = sched_diag.runqueue_local_picks;
    stats->runqueue_empty_picks = sched_diag.runqueue_empty_picks;
    stats->runqueue_lock_acquires = sched_diag.runqueue_lock_acquires;
    stats->runqueue_lock_contentions =
        sched_diag.runqueue_lock_contentions;
    stats->runqueue_parallel_pick_peak =
        sched_diag.runqueue_parallel_pick_peak;
    stats->resched_requests = sched_diag.resched_requests;
    stats->resched_priority_requests =
        sched_diag.resched_priority_requests;
    stats->resched_ipi_sent = sched_diag.resched_ipi_sent;
    stats->resched_ipi_acks = sched_diag.resched_ipi_acks;
    stats->resched_consumed = sched_diag.resched_consumed;
    stats->resched_pending = sched_diag.resched_pending;
    stats->scheduler_violations = sched_diag.scheduler_violations;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    stats->timeout_entries = proc_wait_timer_count_locked();
    stats->timeout_capacity = proc_wait_timer_capacity();
    stats->timeout_full_failures =
        proc_wait_timer_full_failures_locked();
    stats->timeout_duplicate_rejections =
        proc_wait_timer_duplicate_rejections_locked();
    stats->timeout_stale_expirations =
        proc_wait_timer_stale_expirations_locked();
    stats->timeout_heap_violations =
        proc_wait_timer_violations_locked();
    for (task_t *t = proc_first_task_locked(); t;
         t = proc_next_task_locked(t)) {
        stats->listed_tasks++;
        int refs = refcount_read(&t->refs);
        if (refs > 0)
            stats->listed_refs += (unsigned long)refs;
        else
            stats->state_violations++;
        if (t->state == PROC_ZOMBIE)
            stats->zombies++;
        if (t->dispatching)
            stats->dispatching_tasks++;

        unsigned memberships = proc_sched_task_runq_memberships_locked(t);
        uint64_t runqueue_cpu_mask =
            proc_sched_task_runq_cpu_mask_locked(t);
        unsigned cpu_memberships =
            proc_current_owner_memberships_locked(t);
        stats->runqueue_entries += memberships;
        if (memberships != (unsigned)!!t->on_rq)
            stats->state_violations++;
        if (t->on_rq &&
            (t->cpu_id >= 64 ||
             runqueue_cpu_mask != (1ULL << t->cpu_id)))
            stats->state_violations++;
        if ((!!t->on_rq + !!t->dispatching + !!t->on_cpu) > 1)
            stats->state_violations++;
        if (t->on_rq && t->state != PROC_READY)
            stats->state_violations++;
        if (t->dispatching &&
            (t->state != PROC_READY || t->owner_cpu >= CONFIG_NR_CPUS))
            stats->state_violations++;
        if (t->on_cpu &&
            (t->state != PROC_RUNNING || t->owner_cpu >= CONFIG_NR_CPUS))
            stats->state_violations++;
        if (!!(t->dispatching || t->on_cpu) !=
            (t->owner_cpu != PROC_CPU_NONE))
            stats->state_violations++;
        if (cpu_memberships != (unsigned)!!t->on_cpu)
            stats->state_violations++;
        if (t->destroy_started)
            stats->state_violations++;
    }
    stats->cpu_owned_tasks = proc_current_slot_count_locked();
    stats->state_violations +=
        proc_current_lifetime_violations_locked();
    spin_unlock_irqrestore(&proc_lock, flags);

    stats->lifetime_errors =
        stats->ref_get_failures + stats->ref_underflows +
        stats->duplicate_destroy + stats->bad_final_put +
        stats->state_violations + stats->timeout_heap_violations +
        stats->scheduler_violations;
}

size_t proc_lifetime_format(char *buf, size_t bufsz)
{
    if (!buf || bufsz == 0)
        return 0;

    proc_lifetime_stats_t s;
    proc_lifetime_snapshot(&s);
    int n = snprintf(
        buf, bufsz,
        "task_objects: %lu\n"
        "task_refs: %lu\n"
        "listed_tasks: %lu\n"
        "listed_refs: %lu\n"
        "pid_entries: %lu\n"
        "runqueue_entries: %lu\n"
        "dispatching_tasks: %lu\n"
        "cpu_owned_tasks: %lu\n"
        "wait_entries: %lu\n"
        "wake_entries: %lu\n"
        "timeout_entries: %lu\n"
        "timeout_capacity: %lu\n"
        "timeout_full_failures: %lu\n"
        "timeout_duplicate_rejections: %lu\n"
        "timeout_stale_expirations: %lu\n"
        "timeout_heap_violations: %lu\n"
        "runqueue_migrations: %lu\n"
        "runqueue_local_picks: %lu\n"
        "runqueue_empty_picks: %lu\n"
        "runqueue_lock_acquires: %lu\n"
        "runqueue_lock_contentions: %lu\n"
        "runqueue_parallel_pick_peak: %lu\n"
        "resched_requests: %lu\n"
        "resched_priority_requests: %lu\n"
        "resched_ipi_sent: %lu\n"
        "resched_ipi_acks: %lu\n"
        "resched_consumed: %lu\n"
        "resched_pending: %lu\n"
        "scheduler_violations: %lu\n"
        "zombies: %lu\n"
        "ref_get_failures: %lu\n"
        "ref_underflows: %lu\n"
        "duplicate_destroy: %lu\n"
        "bad_final_put: %lu\n"
        "state_violations: %lu\n"
        "lifetime_errors: %lu\n",
        s.task_objects, s.task_refs, s.listed_tasks, s.listed_refs,
        s.pid_entries, s.runqueue_entries, s.dispatching_tasks,
        s.cpu_owned_tasks, s.wait_entries, s.wake_entries,
        s.timeout_entries, s.timeout_capacity, s.timeout_full_failures,
        s.timeout_duplicate_rejections, s.timeout_stale_expirations,
        s.timeout_heap_violations, s.runqueue_migrations,
        s.runqueue_local_picks, s.runqueue_empty_picks,
        s.runqueue_lock_acquires, s.runqueue_lock_contentions,
        s.runqueue_parallel_pick_peak,
        s.resched_requests, s.resched_priority_requests,
        s.resched_ipi_sent, s.resched_ipi_acks, s.resched_consumed,
        s.resched_pending, s.scheduler_violations, s.zombies,
        s.ref_get_failures,
        s.ref_underflows, s.duplicate_destroy, s.bad_final_put,
        s.state_violations, s.lifetime_errors);
    if (n < 0)
        return 0;
    if ((size_t)n >= bufsz)
        return bufsz - 1;
    return (size_t)n;
}
