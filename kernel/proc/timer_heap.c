#include "proc/posix_timer.h"
#include "proc/proc_internal.h"

#include "proc/park.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/klog.h"
#include "core/string.h"
#include "proc/signal.h"
#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
#include "ipc/ipc.h"
#endif

/*
 * Tokenized deadline min-heap and timer/alarm scan machinery, split out of
 * proc/sched.c.  The wait-timer heap is protected by g_wait_timer_lock, and
 * the next-scan deadlines are updated atomically and consumed by
 * proc_next_timer_interval() (arch timer rearm) and sched_scan_timers()
 * (sched()/idle).  Lock order: park_lock -> g_wait_timer_lock; the timer scan
 * collects expired entries under g_wait_timer_lock, releases it, then wakes
 * each target under its park_lock (never holding both).
 */

#ifndef CONFIG_WAIT_TIMER_RUST
typedef struct wait_timer {
    uint64_t deadline;
    task_t *task;
    uint64_t wait_seq;
} wait_timer_t;
#endif

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
#ifndef CONFIG_WAIT_TIMER_RUST
static wait_timer_t wait_timer_heap[WAIT_TIMER_HEAP_MAX];
static unsigned wait_timer_count;
static unsigned long wait_timer_full_failures;
static unsigned long wait_timer_duplicate_rejections;
static unsigned long wait_timer_stale_expirations;
#else
extern void a20_wait_timer_init_rust(void);
extern int a20_wait_timer_register_rust(task_t *, uint64_t, uint64_t);
extern void a20_wait_timer_cancel_rust(task_t *, uint64_t);
extern void a20_wait_timer_expire_rust(uint64_t);
struct a20_wait_timer_stats {
    uint32_t entries;
    uint32_t capacity;
    uint64_t full_failures;
    uint64_t duplicate_rejections;
    uint64_t stale_expirations;
    uint32_t violations;
};
extern void a20_wait_timer_stats_rust(struct a20_wait_timer_stats *);
#endif

/*
 * Per-task SIGALRM (ITIMER_REAL) deadline min-heap.  Protected by
 * g_alarm_timer_lock; entries hold a task reference released on remove.
 * proc_wake_child_waiters-style global scans are avoided: the heap
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
static uint64_t next_posix_scan = SCHED_NO_DEADLINE;
/* Native callbacks require process context; IRQ signal scans must not consume them. */
static uint64_t next_native_scan = SCHED_NO_DEADLINE;

unsigned proc_wait_timer_capacity(void)
{
    return WAIT_TIMER_HEAP_MAX;
}

void proc_wait_timer_snapshot(proc_wait_timer_stats_t *stats)
{
    if (!stats)
        return;
#ifdef CONFIG_WAIT_TIMER_RUST
    struct a20_wait_timer_stats rust_stats = {0};
    extern void a20_wait_timer_stats_rust(struct a20_wait_timer_stats *);
    a20_wait_timer_stats_rust(&rust_stats);
    stats->entries = rust_stats.entries;
    stats->capacity = rust_stats.capacity;
    stats->full_failures = rust_stats.full_failures;
    stats->duplicate_rejections = rust_stats.duplicate_rejections;
    stats->stale_expirations = rust_stats.stale_expirations;
    stats->violations = rust_stats.violations;
#else
    uint64_t flags = spin_lock_irqsave(&g_wait_timer_lock);
    stats->entries = wait_timer_count;
    stats->capacity = WAIT_TIMER_HEAP_MAX;
    stats->full_failures = wait_timer_full_failures;
    stats->duplicate_rejections = wait_timer_duplicate_rejections;
    stats->stale_expirations = wait_timer_stale_expirations;
    unsigned violations = wait_timer_count > WAIT_TIMER_HEAP_MAX;
    /* Keep the diagnostic itself bounded if count metadata is corrupted. */
    for (unsigned i = 0;
         i < wait_timer_count && i < WAIT_TIMER_HEAP_MAX; i++) {
        wait_timer_t *timer = &wait_timer_heap[i];
        if (!timer->task || !timer->deadline || !timer->wait_seq)
            violations++;
        if (timer->task &&
            (timer->task->wait_timer_index != (int)i ||
             __atomic_load_n(&timer->task->wait_seq, __ATOMIC_ACQUIRE) !=
                 timer->wait_seq ||
             __atomic_load_n(&timer->task->wait_deadline, __ATOMIC_RELAXED) !=
                 timer->deadline))
            violations++;
        if (i > 0 &&
            wait_timer_heap[(i - 1) / 2].deadline > timer->deadline)
            violations++;
    }
    stats->violations = violations;
    spin_unlock_irqrestore(&g_wait_timer_lock, flags);
#endif
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
    uint64_t posix = __atomic_load_n(&next_posix_scan, __ATOMIC_RELAXED);
    uint64_t native = __atomic_load_n(&next_native_scan, __ATOMIC_RELAXED);

    if (wake < next)
        next = wake;
    if (alarm < next)
        next = alarm;
    if (posix < next)
        next = posix;
    if (native < next)
        next = native;
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

#ifdef CONFIG_WAIT_TIMER_RUST
uint64_t a20_wait_timer_lock(void)
{
    return spin_lock_irqsave(&g_wait_timer_lock);
}

void a20_wait_timer_unlock(uint64_t flags)
{
    spin_unlock_irqrestore(&g_wait_timer_lock, flags);
}

uint32_t a20_wait_timer_capacity(void)
{
    return WAIT_TIMER_HEAP_MAX;
}

task_t *a20_wait_timer_task_ref(task_t *task)
{
    return proc_get(task);
}

void a20_wait_timer_task_put(task_t *task)
{
    proc_put(task);
}

int a20_wait_timer_task_index(task_t *task)
{
    return task ? __atomic_load_n(&task->wait_timer_index, __ATOMIC_ACQUIRE)
                : -1;
}

void a20_wait_timer_set_task_index(task_t *task, int index)
{
    if (task)
        __atomic_store_n(&task->wait_timer_index, index, __ATOMIC_RELEASE);
}

void a20_wait_timer_set_wake_time(task_t *task, uint64_t deadline)
{
    if (task)
        __atomic_store_n(&task->wake_time, deadline, __ATOMIC_RELAXED);
}

void a20_wait_timer_publish_next(uint64_t deadline)
{
    __atomic_store_n(&next_wake_scan, deadline, __ATOMIC_RELAXED);
}

void a20_wait_timer_rearm_if_earlier(uint64_t deadline)
{
    uint64_t old = __atomic_load_n(&next_wake_scan, __ATOMIC_RELAXED);
    if (deadline < old) {
        __atomic_store_n(&next_wake_scan, deadline, __ATOMIC_RELAXED);
        sched_rearm_timer();
    }
}

int a20_wait_timer_entry_valid(task_t *task, unsigned index,
                               uint64_t deadline, uint64_t seq)
{
    return task &&
           a20_wait_timer_task_index(task) == (int)index &&
           __atomic_load_n(&task->wait_seq, __ATOMIC_ACQUIRE) == seq &&
           __atomic_load_n(&task->wait_deadline, __ATOMIC_RELAXED) == deadline;
}

void a20_wait_timer_after_dequeue(task_t *task, uint64_t seq)
{
    extern void proc_wait_timer_test_after_dequeue(task_t *, uint64_t);
    proc_wait_timer_test_after_dequeue(task, seq);
}
#endif

void proc_wait_timer_test_after_dequeue(task_t *task, uint64_t seq)
{
#ifdef CONFIG_SMP_SELFTEST
    extern void smp_test_wait_timer_after_dequeue(task_t *, uint64_t);
    smp_test_wait_timer_after_dequeue(task, seq);
#else
    (void)task;
    (void)seq;
#endif
}

void proc_wait_timer_test_after_fire(task_t *task, uint64_t seq, int result)
{
#ifdef CONFIG_SMP_SELFTEST
    extern void smp_test_wait_timer_after_fire(task_t *, uint64_t, int);
    smp_test_wait_timer_after_fire(task, seq, result);
#else
    (void)task;
    (void)seq;
    (void)result;
#endif
}

#ifndef CONFIG_WAIT_TIMER_RUST
static void wait_timer_heap_swap(unsigned a, unsigned b)
{
    wait_timer_t tmp = wait_timer_heap[a];
    wait_timer_heap[a] = wait_timer_heap[b];
    wait_timer_heap[b] = tmp;
    __atomic_store_n(&wait_timer_heap[a].task->wait_timer_index, (int)a,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&wait_timer_heap[b].task->wait_timer_index, (int)b,
                     __ATOMIC_RELEASE);
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
        __atomic_store_n(&removed.task->wait_timer_index, -1,
                         __ATOMIC_RELEASE);
    wait_timer_count--;
    if (index != wait_timer_count) {
        wait_timer_heap[index] = wait_timer_heap[wait_timer_count];
        __atomic_store_n(&wait_timer_heap[index].task->wait_timer_index,
                         (int)index, __ATOMIC_RELEASE);
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
    __atomic_store_n(&t->wait_timer_index, (int)index, __ATOMIC_RELEASE);
    __atomic_store_n(&t->wake_time, deadline, __ATOMIC_RELAXED);
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
    __atomic_store_n(&t->wake_time, 0, __ATOMIC_RELAXED);
    proc_put(removed.task);
}
#endif

/* Public wrappers: acquire g_wait_timer_lock around the *_locked helpers.
 * Callers typically hold the target's park_lock (park_lock -> timer lock). */
int proc_wait_timer_register(task_t *t, uint64_t deadline, uint64_t wait_seq)
{
#ifdef CONFIG_WAIT_TIMER_RUST
    return a20_wait_timer_register_rust(t, deadline, wait_seq);
#else
    uint64_t flags = spin_lock_irqsave(&g_wait_timer_lock);
    int r = proc_wait_timer_register_locked(t, deadline, wait_seq);
    spin_unlock_irqrestore(&g_wait_timer_lock, flags);
    return r;
#endif
}

void proc_wait_timer_cancel(task_t *t, uint64_t wait_seq)
{
#ifdef CONFIG_WAIT_TIMER_RUST
    a20_wait_timer_cancel_rust(t, wait_seq);
#else
    uint64_t flags = spin_lock_irqsave(&g_wait_timer_lock);
    proc_wait_timer_cancel_locked(t, wait_seq);
    spin_unlock_irqrestore(&g_wait_timer_lock, flags);
#endif
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
    if (sched_note_deadline(&next_native_scan, deadline))
        sched_rearm_timer();
}

/*
 * Full store (not min): posix_timer_tick() recomputes the earliest POSIX
 * expiry after every scan and may move it later or clear it.
 */
void sched_set_posix_deadline(uint64_t deadline)
{
    __atomic_store_n(&next_posix_scan,
                     deadline ? deadline : SCHED_NO_DEADLINE,
                     __ATOMIC_RELAXED);
    sched_rearm_timer();
}

/*
 * Wake every task whose wait-timer deadline has passed.  Safe to call from
 * IRQ context (spinlocks and atomics only): the timer interrupt invokes this
 * so an expired deadline is reaped immediately, which advances next_wake_scan
 * and keeps the one-shot timer from re-arming at the 100us floor until the
 * next sched() pass.  sched() also calls it before scanning alarms.
 */
void proc_sched_expire_wait_timers(uint64_t now)
{
#ifdef CONFIG_WAIT_TIMER_RUST
    a20_wait_timer_expire_rust(now);
#else
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
            proc_wait_timer_test_after_dequeue(t, expired[i].seq);
            uint64_t plf = spin_lock_irqsave(&t->park_lock);
            int stale = t->wait_seq != expired[i].seq;
            int woke = proc_try_wake_locked_common(
                t, expired[i].seq, PROC_WAKE_TIMEOUT, NULL, NULL);
            spin_unlock_irqrestore(&t->park_lock, plf);
            int result = stale ? 2 : (woke ? 1 : 0);
            proc_wait_timer_test_after_fire(t, expired[i].seq, result);
            if (result == 2)
                wait_timer_stale_expirations++;
            proc_put(t);
        }
    } while (expired_count == 128);
#endif
}

/*
 * Signal-timer scan (per-task SIGALRM alarms and POSIX compat timers).
 * IRQ-safe: runs both from sched() passes and from the timer IRQ
 * (proc_sched_tick) so timers fire on deadline even when every task is
 * asleep and no sched() pass would otherwise happen.
 */
void proc_sched_scan_signal_timers(uint64_t now)
{
    int scan_alarms =
        now >= __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED);
    if (scan_alarms)
        __atomic_exchange_n(&next_alarm_scan, SCHED_NO_DEADLINE,
                            __ATOMIC_RELAXED);

    if (scan_alarms) {
        uint64_t next_alarm = SCHED_NO_DEADLINE;
        uint64_t flags;
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

#if defined(CONFIG_ABI_LINUX) || defined(CONFIG_ABI_BOTH)
    posix_timer_tick();
#endif
}

void sched_scan_timers(uint64_t now)
{
    proc_sched_expire_wait_timers(now);

    proc_sched_scan_signal_timers(now);

    /* PSI stall accounting sample (kernel/core/psi.c). */
    extern void psi_tick(void);
    psi_tick();

#if defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)
    __atomic_exchange_n(&next_native_scan, SCHED_NO_DEADLINE,
                        __ATOMIC_RELAXED);
    a20_timer_tick();
    /* Periodic monitor sampling (Native ABI perf-style counters). */
    extern void a20_monitor_tick(void);
    a20_monitor_tick();
#endif

}

void proc_timer_heap_init(void)
{
#ifdef CONFIG_WAIT_TIMER_RUST
    a20_wait_timer_init_rust();
#else
    wait_timer_count = 0;
    wait_timer_full_failures = 0;
    wait_timer_duplicate_rejections = 0;
    wait_timer_stale_expirations = 0;
#endif
    __atomic_store_n(&next_wake_scan, SCHED_NO_DEADLINE, __ATOMIC_RELAXED);
    __atomic_store_n(&next_alarm_scan, SCHED_NO_DEADLINE, __ATOMIC_RELAXED);
    __atomic_store_n(&next_posix_scan, SCHED_NO_DEADLINE, __ATOMIC_RELAXED);
    __atomic_store_n(&next_native_scan, SCHED_NO_DEADLINE, __ATOMIC_RELAXED);
}

int proc_sched_timers_due(uint64_t now)
{
    return now >= __atomic_load_n(&next_wake_scan, __ATOMIC_RELAXED) ||
           now >= __atomic_load_n(&next_alarm_scan, __ATOMIC_RELAXED) ||
           now >= __atomic_load_n(&next_posix_scan, __ATOMIC_RELAXED) ||
           now >= __atomic_load_n(&next_native_scan, __ATOMIC_RELAXED);
}
