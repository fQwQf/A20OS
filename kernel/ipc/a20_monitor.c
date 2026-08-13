/*
 * A20OS core — monitor object (Native ABI).
 *
 * A monitor is a perf-style software-event counter exposed as a handle.  It
 * samples an existing kernel statistic (per-task CPU time, page faults,
 * context switches; system-wide page faults / context switches) on demand
 * (monitor_query) and, when a period is configured, pushes the count to
 * every event queue watching the MONITOR handle as A20_EVENT_SIGNALED.
 *
 * Design reference: docs/native-abi/09-native-abi-deepening.md §3.
 * The global registry holds one reference per registered monitor; a monitor
 * unregisters itself on handle release, dropping that reference.
 */
#include "core/types.h"
#include "core/string.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/timer.h"
#include "core/perf.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "ipc/ipc.h"
#include "ipc/objstats.h"

static spinlock_t g_mon_lock = SPINLOCK_INIT;
static a20_monitor_t *g_mon_list;

static uint64_t monitor_ticks_to_ns(uint64_t ticks)
{
    return ticks * 1000000000ULL / TICKS_PER_SEC;
}

static uint64_t monitor_now_ns(void)
{
    return monitor_ticks_to_ns(timer_get_ticks());
}

a20_monitor_t *a20_monitor_create(uint32_t kind, uint32_t flags,
                                  task_t *target, uint64_t period_ns)
{
    a20_monitor_t *m = kmalloc(sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    refcount_set(&m->refcount, 1);
    spin_init(&m->lock);
    m->kind = kind;
    m->flags = flags;
    m->target = target;               /* referenced by the caller if non-NULL */
    m->target_pid = target ? target->pid : 0;
    m->period_ns = period_ns;
    m->time_start_ns = monitor_now_ns();
    m->last_sample = timer_get_ticks();
    m->queue = NULL;
    return m;
}

void a20_monitor_ref(a20_monitor_t *m)
{
    if (m) refcount_inc(&m->refcount);
}

static void a20_monitor_free(a20_monitor_t *m)
{
    if (!m) return;
    if (m->target)
        proc_put(m->target);
    kfree(m);
}

void a20_monitor_put(a20_monitor_t *m)
{
    if (m && refcount_dec_and_test(&m->refcount))
        a20_monitor_free(m);
}

/* Register for periodic sampling (drops the registry reference on unregister). */
void a20_monitor_register(a20_monitor_t *m)
{
    if (!m) return;
    a20_monitor_ref(m);
    spin_lock(&g_mon_lock);
    m->next_registered = g_mon_list;
    g_mon_list = m;
    spin_unlock(&g_mon_lock);
}

void a20_monitor_unregister(a20_monitor_t *m)
{
    if (!m) return;
    spin_lock(&g_mon_lock);
    if (m->registered) {
        a20_monitor_t **pp = &g_mon_list;
        while (*pp && *pp != m)
            pp = &(*pp)->next_registered;
        if (*pp)
            *pp = m->next_registered;
        m->next_registered = NULL;
        m->registered = 0;
        spin_unlock(&g_mon_lock);
        a20_monitor_put(m);           /* drop the registry reference */
        return;
    }
    spin_unlock(&g_mon_lock);
}

/* Handle release path (a20_object_release): drop the handle reference and
 * unregister from the periodic list, which drops the registry reference. */
void a20_monitor_release(a20_monitor_t *m)
{
    if (!m) return;
    a20_monitor_unregister(m);
    a20_monitor_put(m);
}

/* Snapshot the underlying statistic.  Returns the count; *alive is 0 when a
 * task-scoped target no longer exists. */
static uint64_t monitor_sample_locked(a20_monitor_t *m, int *alive)
{
    *alive = 1;
    if (m->kind >= A20_MONITOR_SYS_PAGE_FAULTS) {
        switch (m->kind) {
        case A20_MONITOR_SYS_PAGE_FAULTS:
            return __atomic_load_n(&g_perf_sw_page_faults, __ATOMIC_RELAXED);
        case A20_MONITOR_SYS_CTX_SWITCH:
            return __atomic_load_n(&g_perf_sw_context_switches,
                                   __ATOMIC_RELAXED);
        default:
            return 0;
        }
    }
    task_t *t = m->target;
    if (!t) { *alive = 0; return 0; }
    switch (m->kind) {
    case A20_MONITOR_TASK_CPU_TIME:
        return monitor_ticks_to_ns(t->total_time);
    case A20_MONITOR_TASK_SYS_TIME:
        return 0;   /* no user/sys split in the core scheduler */
    case A20_MONITOR_TASK_PAGE_FAULTS:
        return __atomic_load_n(&t->perf_page_faults, __ATOMIC_RELAXED);
    case A20_MONITOR_TASK_CTX_SWITCH:
        return __atomic_load_n(&t->perf_switches, __ATOMIC_RELAXED);
    case A20_MONITOR_TASK_MIGRATIONS:
        return 0;   /* no per-task migration counter yet */
    default:
        return 0;
    }
}

int64_t a20_monitor_sample(a20_monitor_t *m)
{
    if (!m) return -A20_ERR_BAD_HANDLE;
    int alive;
    spin_lock(&m->lock);
    uint64_t v = monitor_sample_locked(m, &alive);
    m->count = v;
    spin_unlock(&m->lock);
    return alive ? (int64_t)v : -A20_ERR_BAD_HANDLE;
}

/*
 * Periodic tick (called from the timer interrupt, kernel/proc/timer_heap.c).
 * For each registered monitor with a period, notify every event queue
 * watching the MONITOR handle when the period has elapsed.
 */
void a20_monitor_tick(void)
{
    spin_lock(&g_mon_lock);
    for (a20_monitor_t *m = g_mon_list; m; m = m->next_registered) {
        if (!m->period_ns)
            continue;
        uint64_t now = timer_get_ticks();
        uint64_t elapsed_ticks = now - m->last_sample;
        uint64_t period_ticks =
            m->period_ns / (1000000000ULL / TICKS_PER_SEC);
        if (period_ticks == 0)
            period_ticks = 1;
        if (elapsed_ticks < period_ticks)
            continue;
        m->last_sample = now;
        int alive;
        uint64_t v = monitor_sample_locked(m, &alive);
        m->count = v;
        a20_event_notify(m, A20_OBJ_MONITOR, A20_EVENT_SIGNALED, v, 0);
    }
    spin_unlock(&g_mon_lock);
}
