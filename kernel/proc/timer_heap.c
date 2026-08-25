#include "proc/proc_internal.h"

#include "proc/park.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/klog.h"
#include "core/string.h"
#include "proc/signal.h"
#ifdef CONFIG_ABI_LINUX
#endif
#ifdef CONFIG_ABI_NATIVE
#include "ipc/ipc.h"
#endif

/*
 * Tokenized deadline min-heap and timer/alarm scan machinery, split out of
 * proc/sched.c.  The wait-timer heap is protected by g_wait_timer_lock, and
 * the two next-scan deadlines are updated atomically and consumed by
 * proc_next_timer_interval() (arch timer rearm) and sched_scan_timers()
 * (sched()/idle).  Lock order: park_lock -> g_wait_timer_lock; the timer scan
 * collects expired entries under g_wait_timer_lock, releases it, then wakes
 * each target under its park_lock (never holding both).
 */

typedef struct wait_timer {
    uint64_t deadline;
    task_t *task;
    uint64_t wait_seq;
} wait_timer_t;

#ifndef CONFIG_WAIT_TIMER_HEAP_MAX
#ifdef CONFIG_MCU
#define WAIT_TIMER_HEAP_MAX 64
#else
#define WAIT_TIMER_HEAP_MAX 8192
#endif
#else
#define WAIT_TIMER_HEAP_MAX CONFIG_WAIT_TIMER_HEAP_MAX
#endif

/* Tokenized deadline min-heap.  Protected by g_wait_timer_lock; the *_locked
 * accessors require it to be held (callers hold the target's park_lock first). */
static spinlock_t g_wait_timer_lock = SPINLOCK_INIT;
static wait_timer_t wait_timer_heap[WAIT_TIMER_HEAP_MAX];
static unsigned wait_timer_count;
static unsigned long wait_timer_full_failures;
static unsigned long wait_timer_duplicate_rejections;
static unsigned long wait_timer_stale_expirations;

/*
 * Per-task SIGALRM (ITIMER_REAL) deadline min-heap.  Protected by
 * g_alarm_timer_lock; entries hold a task reference released on remove.
 * proc_wake_child_waiters_locked-style global scans are avoided: the heap
 * root is the next alarm deadline and expiry pops only due entries.
 */
typedef struct alarm_timer {
    uint64_t deadline;
    task_t *task;
} alarm_timer_t;

#define ALARM_TIMER_HEAP_MAX WAIT_TIMER_HEAP_MAX

static spinlock_t g_alarm_timer_lock = SPINLOCK_INIT;
static alarm_timer_t alarm_timer_heap[ALARM_TIMER_HEAP_MAX];
static unsigned alarm_timer_count;

static void alarm_timer_swap(unsigned a, unsigned b)
{
    alarm_timer_t tmp = alarm_timer_heap[a];
    alarm_timer_heap[a] = alarm_timer_heap[b];
    alarm_timer_heap[b] = tmp;
    alarm_timer_heap[a].task->alarm_timer_index = (int)a;
    alarm_timer_heap[b].task->alarm_timer_index = (int)b;
}

static void alarm_timer_sift_up(unsigned index)
{
    while (index > 0) {
        unsigned parent = (index - 1) / 2;
        if (alarm_timer_heap[parent].deadline <=
            alarm_timer_heap[index].deadline)
            break;
        alarm_timer_swap(parent, index);
        index = parent;
    }
}

static void alarm_timer_sift_down(unsigned index)
{
    for (;;) {
        unsigned left = index * 2 + 1;
        unsigned right = left + 1;
        unsigned smallest = index;
        if (left < alarm_timer_count &&
            alarm_timer_heap[left].deadline <
            alarm_timer_heap[smallest].deadline)
            smallest = left;
        if (right < alarm_timer_count &&
            alarm_timer_heap[right].deadline <
            alarm_timer_heap[smallest].deadline)
            smallest = right;
        if (smallest == index)
            break;
        alarm_timer_swap(index, smallest);
        index = smallest;
    }
}

static alarm_timer_t alarm_timer_remove_index_locked(unsigned index)
{
    alarm_timer_t removed = {0};
    if (index >= alarm_timer_count)
        return removed;

    removed = alarm_timer_heap[index];
    if (removed.task)
        removed.task->alarm_timer_index = -1;
    alarm_timer_count--;
    if (index != alarm_timer_count) {
        alarm_timer_heap[index] = alarm_timer_heap[alarm_timer_count];
        alarm_timer_heap[index].task->alarm_timer_index = (int)index;
        if (index > 0 &&
            alarm_timer_heap[index].deadline <
            alarm_timer_heap[(index - 1) / 2].deadline)
            alarm_timer_sift_up(index);
        else
            alarm_timer_sift_down(index);
    }
    memset(&alarm_timer_heap[alarm_timer_count], 0,
           sizeof(alarm_timer_heap[alarm_timer_count]));
    return removed;
}

/* Requires g_alarm_timer_lock held.  Drops the entry's task reference. */
static void alarm_timer_cancel_locked(task_t *t)
{
    if (!t || t->alarm_timer_index < 0)
        return;
    unsigned index = (unsigned)t->alarm_timer_index;
    if (index >= alarm_timer_count ||
        alarm_timer_heap[index].task != t)
        return;
    alarm_timer_t removed = alarm_timer_remove_index_locked(index);
    if (removed.task)
        proc_put(removed.task);
}

void proc_alarm_cancel(task_t *t)
{
    if (!t)
        return;
    uint64_t flags = spin_lock_irqsave(&g_alarm_timer_lock);
    alarm_timer_cancel_locked(t);
    spin_unlock_irqrestore(&g_alarm_timer_lock, flags);
}

static uint64_t next_wake_scan = SCHED_NO_DEADLINE;
static uint64_t next_alarm_scan = SCHED_NO_DEADLINE;

unsigned proc_wait_timer_count_locked(void)
{
    return wait_timer_count;
}

unsigned proc_wait_timer_capacity(void)
{
    return WAIT_TIMER_HEAP_MAX;
}

unsigned long proc_wait_timer_full_failures_locked(void)
{
    return wait_timer_full_failures;
}

unsigned long proc_wait_timer_duplicate_rejections_locked(void)
{
    return wait_timer_duplicate_rejections;
}

unsigned long proc_wait_timer_stale_expirations_locked(void)
{
    return wait_timer_stale_expirations;
}

unsigned proc_wait_timer_violations_locked(void)
{
    unsigned violations = wait_timer_count > WAIT_TIMER_HEAP_MAX;
    for (unsigned i = 0; i < wait_timer_count; i++) {
        wait_timer_t *timer = &wait_timer_heap[i];
        if (!timer->task || !timer->deadline || !timer->wait_seq)
            violations++;
        if (timer->task &&
            (timer->task->wait_timer_index != (int)i ||
             timer->task->wait_seq != timer->wait_seq ||
             timer->task->wait_deadline != timer->deadline))
            violations++;
        if (i > 0 &&
            wait_timer_heap[(i - 1) / 2].deadline > timer->deadline)
            violations++;
    }
    return violations;
}

#define SCHED_TICK_INTERVAL       (TICKS_PER_SEC / 100)
#define SCHED_MIN_TIMER_INTERVAL  (TICKS_PER_SEC / 10000 ? TICKS_PER_SEC / 10000 : 1)

#ifdef CONFIG_MCU
#define SCHED_SIGNAL_BATCH 8
#else
#define SCHED_SIGNAL_BATCH 128
#endif

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
    if (removed.task)
        removed.task->wait_timer_index = -1;
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
        return PROC_PARK_PREPARE_INVALID;
    if (t->wait_timer_index >= 0) {
        wait_timer_duplicate_rejections++;
        return PROC_PARK_PREPARE_TIMEOUT_DUPLICATE;
    }
    if (wait_timer_count >= WAIT_TIMER_HEAP_MAX) {
        wait_timer_full_failures++;
        return PROC_PARK_PREPARE_TIMEOUT_CAPACITY;
    }

    uint64_t old_first =
        wait_timer_count ? wait_timer_heap[0].deadline : SCHED_NO_DEADLINE;
    task_t *owned = proc_get(t);
    if (!owned)
        return PROC_PARK_PREPARE_TIMEOUT_REF;
    unsigned index = wait_timer_count++;
    wait_timer_heap[index].deadline = deadline;
    wait_timer_heap[index].task = owned;
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

/* Public wrappers: acquire g_wait_timer_lock around the *_locked helpers.
 * Callers typically hold the target's park_lock (park_lock -> timer lock). */
int proc_wait_timer_register(task_t *t, uint64_t deadline, uint64_t wait_seq)
{
    uint64_t flags = spin_lock_irqsave(&g_wait_timer_lock);
    int r = proc_wait_timer_register_locked(t, deadline, wait_seq);
    spin_unlock_irqrestore(&g_wait_timer_lock, flags);
    return r;
}

void proc_wait_timer_cancel(task_t *t, uint64_t wait_seq)
{
    uint64_t flags = spin_lock_irqsave(&g_wait_timer_lock);
    proc_wait_timer_cancel_locked(t, wait_seq);
    spin_unlock_irqrestore(&g_wait_timer_lock, flags);
}

void proc_set_alarm_expire(task_t *t, uint64_t alarm_expire)
{
    if (!t)
        return;

    uint64_t flags = spin_lock_irqsave(&g_alarm_timer_lock);
    alarm_timer_cancel_locked(t);
    if (alarm_expire && alarm_timer_count >= ALARM_TIMER_HEAP_MAX) {
        /* Defensive: capacity is ALARM_TIMER_HEAP_MAX >= the PID limit, so a
         * live task can never fill it; drop the alarm rather than track a
         * deadline that would never fire. */
        alarm_expire = 0;
    }
    if (alarm_expire) {
        task_t *owned = proc_get(t);
        if (owned) {
            unsigned index = alarm_timer_count++;
            alarm_timer_heap[index].deadline = alarm_expire;
            alarm_timer_heap[index].task = owned;
            t->alarm_timer_index = (int)index;
            alarm_timer_sift_up(index);
        } else {
            alarm_expire = 0;
        }
    }
    __atomic_store_n(&t->alarm_expire, alarm_expire, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&g_alarm_timer_lock, flags);

    if (sched_note_deadline(&next_alarm_scan, alarm_expire))
        sched_rearm_timer();
}

/*
 * sched_note_timer_deadline — request a sched_scan_timers() run at the
 * given tick WITHOUT arming a per-task SIGALRM alarm.  Native A20 timer
 * objects fire via a20_timer_tick() and must not signal the arming task.
 */
void sched_note_timer_deadline(uint64_t deadline)
{
    if (!deadline)
        return;
    if (sched_note_deadline(&next_alarm_scan, deadline))
        sched_rearm_timer();
}

void sched_scan_timers(uint64_t now)
{
    int scan_alarms =
        now >= __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED);
    if (scan_alarms)
        __atomic_exchange_n(&next_alarm_scan, SCHED_NO_DEADLINE,
                            __ATOMIC_RELAXED);

    struct { task_t *task; uint64_t seq; } expired[128];
    unsigned expired_count;
    uint64_t flags;
    do {
        expired_count = 0;
        flags = spin_lock_irqsave(&g_wait_timer_lock);
        while (wait_timer_count && wait_timer_heap[0].deadline <= now &&
               expired_count < 128) {
            wait_timer_t timer = wait_timer_remove_index_locked(0);
            if (timer.task && timer.wait_seq) {
                expired[expired_count].task = timer.task;
                expired[expired_count].seq = timer.wait_seq;
                expired_count++;
            } else if (timer.task) {
                proc_put(timer.task);
            }
        }
        spin_unlock_irqrestore(&g_wait_timer_lock, flags);

        for (unsigned i = 0; i < expired_count; i++) {
            task_t *t = expired[i].task;
            /* park_lock serializes the timeout wake against a concurrent
             * signal wake; the removed timer entry makes the cancel inside
             * the wake a no-op.  A task already woken (WOKEN/IDLE) is left
             * alone. */
            uint64_t plf = spin_lock_irqsave(&t->park_lock);
            int stale = t->wait_seq != expired[i].seq;
            int woke = proc_try_wake_locked_common(
                t, expired[i].seq, PROC_WAKE_TIMEOUT, NULL, NULL);
            spin_unlock_irqrestore(&t->park_lock, plf);
            if (!woke && stale)
                wait_timer_stale_expirations++;
            proc_put(t);
        }
    } while (expired_count == 128);

    if (scan_alarms) {
        uint64_t next_alarm = SCHED_NO_DEADLINE;
        int more_due;
        do {
            task_t *sigalrm_tasks[SCHED_SIGNAL_BATCH];
            int sigalrm_count = 0;
            more_due = 0;
            next_alarm = SCHED_NO_DEADLINE;

            flags = spin_lock_irqsave(&g_alarm_timer_lock);
            while (alarm_timer_count > 0) {
                if (alarm_timer_heap[0].deadline > now)
                    break;
                if (sigalrm_count >= SCHED_SIGNAL_BATCH) {
                    more_due = 1;
                    break;
                }
                alarm_timer_t expired =
                    alarm_timer_remove_index_locked(0);
                /* The heap reference rides along until the signal below. */
                sigalrm_tasks[sigalrm_count++] = expired.task;
            }
            if (alarm_timer_count > 0)
                next_alarm = alarm_timer_heap[0].deadline;
            spin_unlock_irqrestore(&g_alarm_timer_lock, flags);

            for (int i = 0; i < sigalrm_count; i++) {
                task_t *t = sigalrm_tasks[i];
                int state =
                    __atomic_load_n(&t->state, __ATOMIC_ACQUIRE);
                if (state == PROC_UNUSED || state == PROC_ZOMBIE) {
                    /* Mirror the old scan: zombies are not signalled and
                     * their stale alarm is not re-armed. */
                    __atomic_store_n(&t->alarm_expire, 0,
                                     __ATOMIC_RELAXED);
                    proc_put(t);
                    continue;
                }
                uint64_t interval =
                    __atomic_load_n(&t->itimer_real_interval,
                                    __ATOMIC_RELAXED);
                uint64_t alarm = interval ? now + interval : 0;
                proc_set_alarm_expire(t, alarm);
                (void)signal_send_task(t, SIGALRM);
                proc_put(t);
            }
        } while (more_due);

        if (sched_note_deadline(&next_alarm_scan, next_alarm))
            sched_rearm_timer();
    }

#ifdef CONFIG_ABI_LINUX
    extern void posix_timer_tick(void);
    posix_timer_tick();
#endif

    /* PSI stall accounting sample (kernel/core/psi.c). */
    extern void psi_tick(void);
    psi_tick();

#ifdef CONFIG_ABI_NATIVE
    a20_timer_tick();
    /* Periodic monitor sampling (Native ABI perf-style counters). */
    extern void a20_monitor_tick(void);
    a20_monitor_tick();
#endif

}

void proc_timer_heap_init(void)
{
    wait_timer_count = 0;
    wait_timer_full_failures = 0;
    wait_timer_duplicate_rejections = 0;
    wait_timer_stale_expirations = 0;
    __atomic_store_n(&next_wake_scan, SCHED_NO_DEADLINE, __ATOMIC_RELAXED);
    __atomic_store_n(&next_alarm_scan, SCHED_NO_DEADLINE, __ATOMIC_RELAXED);
}

int proc_sched_timers_due(uint64_t now)
{
    return now >= __atomic_load_n(&next_wake_scan, __ATOMIC_RELAXED) ||
           now >= __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED);
}
