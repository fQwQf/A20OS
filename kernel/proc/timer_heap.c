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
 * proc/sched.c.  The heap is protected by proc_lock; the two next-scan
 * deadlines are updated atomically and consumed by proc_next_timer_interval()
 * (arch timer rearm) and sched_scan_timers() (sched()/idle).
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

/* Tokenized deadline min-heap, protected by proc_lock. */
static wait_timer_t wait_timer_heap[WAIT_TIMER_HEAP_MAX];
static unsigned wait_timer_count;
static unsigned long wait_timer_full_failures;
static unsigned long wait_timer_duplicate_rejections;
static unsigned long wait_timer_stale_expirations;

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

void proc_set_alarm_expire(task_t *t, uint64_t alarm_expire)
{
    if (!t)
        return;
    __atomic_store_n(&t->alarm_expire, alarm_expire, __ATOMIC_RELAXED);
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

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    while (wait_timer_count && wait_timer_heap[0].deadline <= now) {
        wait_timer_t timer = wait_timer_remove_index_locked(0);
        if (timer.task && timer.wait_seq) {
            timer.task->sched_level = 0;
            if (!proc_try_wake_locked(timer.task, timer.wait_seq,
                                      PROC_WAKE_TIMEOUT))
                wait_timer_stale_expirations++;
        }
        proc_put(timer.task);
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    if (scan_alarms) {
        uint64_t next_alarm = SCHED_NO_DEADLINE;
        int more_due;
        do {
            task_t *sigalrm_tasks[SCHED_SIGNAL_BATCH];
            int sigalrm_count = 0;
            more_due = 0;
            next_alarm = SCHED_NO_DEADLINE;

            flags = spin_lock_irqsave(&proc_lock);
            for (task_t *t = proc_first_task_locked(); t;
                 t = proc_next_task_locked(t)) {
                if (t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
                    continue;

                uint64_t alarm =
                    __atomic_load_n(&t->alarm_expire, __ATOMIC_RELAXED);
                if (alarm > 0 && now >= alarm) {
                    if (sigalrm_count >= SCHED_SIGNAL_BATCH) {
                        more_due = 1;
                        if (alarm < next_alarm)
                            next_alarm = alarm;
                        continue;
                    }
                    task_t *owned = proc_get(t);
                    if (!owned) {
                        __atomic_store_n(&t->alarm_expire, 0,
                                         __ATOMIC_RELAXED);
                        continue;
                    }
                    uint64_t interval =
                        __atomic_load_n(&t->itimer_real_interval,
                                        __ATOMIC_RELAXED);
                    alarm = interval ? now + interval : 0;
                    __atomic_store_n(&t->alarm_expire, alarm,
                                     __ATOMIC_RELAXED);
                    sigalrm_tasks[sigalrm_count++] = owned;
                }
                if (alarm > 0 && alarm < next_alarm)
                    next_alarm = alarm;
            }
            spin_unlock_irqrestore(&proc_lock, flags);

            for (int i = 0; i < sigalrm_count; i++) {
                (void)signal_send_task(sigalrm_tasks[i], SIGALRM);
                proc_put(sigalrm_tasks[i]);
            }
        } while (more_due);

        if (sched_note_deadline(&next_alarm_scan, next_alarm))
            sched_rearm_timer();
    }

#ifdef CONFIG_ABI_LINUX
    extern void posix_timer_tick(void);
    posix_timer_tick();
#endif

#ifdef CONFIG_ABI_NATIVE
    a20_timer_tick();
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
