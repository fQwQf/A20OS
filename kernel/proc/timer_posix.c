#include "proc/posix_timer.h"
#include "core/errno.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "proc/signal.h"

/*
 * POSIX interval-timer table — ABI-agnostic process subsystem.
 *
 * The Linux ABI decodes timer_create/timer_settime wire structs onto the
 * calls below; expiry is driven by proc/timer_heap.c via posix_timer_tick().
 * The table is intentionally small and static: these timers back the
 * compat surface only (Native users use timer objects / EventQ).
 *
 * Linux semantics implemented here:
 *  - timer_gettime() reports the time REMAINING until expiry, not the
 *    originally armed value; a disarmed timer reads back as zero.
 *  - Arming with it_value == 0 disarms.
 *  - TIMER_ABSTIME interprets it_value against the timer's clock.
 *  - Periodic re-arm is drift-free (expire += k * interval) and expiry
 *    bursts against a still-pending notification increment the overrun
 *    counter instead of re-queuing (standard signals coalesce anyway).
 *  - timer_getoverrun() reports the overrun of the most recently
 *    delivered/accepted notification.
 *
 * ITIMER_VIRTUAL/ITIMER_PROF (cpu-time itimers) also live here: they are
 * driven from the scheduler tick accounting site via posix_itimer_cpu_tick()
 * because their metric (task user/system ticks) only advances there.
 */

#define COMPAT_TIMER_MAX 32

/* clockid families (Linux uapi constants) */
#define PCLK_REALTIME  0
#define PCLK_MONOTONIC 1
#define PCLK_MONOTONIC_RAW 4
#define PCLK_REALTIME_COARSE 5
#define PCLK_MONOTONIC_COARSE 6
#define PCLK_BOOTTIME  7
#define PCLK_TAI       11

typedef struct {
    int used;
    int owner_pid;
    int signo;
    int target_tid;      /* SIGEV_THREAD_ID delivery target, 0 = process-wide */
    int clockid;
    uint64_t interval[2];
    uint64_t expire_tick;    /* 0 = disarmed */
    uint64_t overrun_cur;    /* overruns of the currently pending signal */
    uint64_t overrun_last;   /* overruns of the last delivered signal */
} posix_timer_t;

static posix_timer_t g_posix_timers[COMPAT_TIMER_MAX];

/* CPU-time itimer entries (ITIMER_VIRTUAL / ITIMER_PROF); see the section
 * at the bottom of this file. */
typedef struct {
    int used;
    task_t *task;            /* reference held while the entry lives */
    uint64_t expire[2];      /* metric deadlines, 0 = disarmed */
    uint64_t interval[2];    /* reload values in metric ticks */
} cpu_itimer_t;

static cpu_itimer_t g_cpu_itimers[COMPAT_TIMER_MAX];

static uint64_t posix_timer_timespec_to_ticks(uint64_t sec, uint64_t nsec)
{
    return sec * TICKS_PER_SEC +
           (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
}

static int posix_timer_clock_is_realtime(int clockid)
{
    return clockid == PCLK_REALTIME || clockid == PCLK_REALTIME_COARSE ||
           clockid == PCLK_TAI;
}

int posix_timer_clock_supported(int clockid)
{
    return clockid == PCLK_REALTIME || clockid == PCLK_MONOTONIC ||
           clockid == PCLK_MONOTONIC_RAW || clockid == PCLK_REALTIME_COARSE ||
           clockid == PCLK_MONOTONIC_COARSE || clockid == PCLK_BOOTTIME ||
           clockid == PCLK_TAI;
}

static void posix_timer_update_deadline(void)
{
    uint64_t next = 0;
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        if (!g_posix_timers[i].used || g_posix_timers[i].signo == 0)
            continue;
        uint64_t expire = g_posix_timers[i].expire_tick;
        if (expire && (next == 0 || expire < next))
            next = expire;
    }
    sched_set_posix_deadline(next);
}

int posix_timer_create(int owner_pid, int signo, int target_tid, int clockid)
{
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        if (!g_posix_timers[i].used) {
            memset(&g_posix_timers[i], 0, sizeof(g_posix_timers[i]));
            g_posix_timers[i].used = 1;
            g_posix_timers[i].owner_pid = owner_pid;
            g_posix_timers[i].signo = signo;
            g_posix_timers[i].target_tid = target_tid;
            g_posix_timers[i].clockid = clockid;
            return i;
        }
    }
    return -EAGAIN;
}

int posix_timer_delete(int owner_pid, int id)
{
    if (id < 0 || id >= COMPAT_TIMER_MAX || !g_posix_timers[id].used)
        return -EINVAL;
    if (g_posix_timers[id].owner_pid != owner_pid)
        return -EINVAL;
    memset(&g_posix_timers[id], 0, sizeof(g_posix_timers[id]));
    posix_timer_update_deadline();
    return 0;
}

int posix_timer_get_time(int owner_pid, int id, uint64_t out[4])
{
    if (id < 0 || id >= COMPAT_TIMER_MAX || !g_posix_timers[id].used)
        return -EINVAL;
    if (g_posix_timers[id].owner_pid != owner_pid)
        return -EINVAL;
    memset(out, 0, sizeof(uint64_t) * 4);
    out[0] = g_posix_timers[id].interval[0];
    out[1] = g_posix_timers[id].interval[1];
    uint64_t expire = g_posix_timers[id].expire_tick;
    if (!expire)
        return 0;
    uint64_t now = timer_get_ticks();
    uint64_t rem = expire > now ? expire - now : 0;
    out[2] = rem / TICKS_PER_SEC;
    out[3] = (rem % TICKS_PER_SEC) * 1000000000ULL / TICKS_PER_SEC;
    return 0;
}

int posix_timer_set_time(int owner_pid, int id, const uint64_t ts[4],
                         int flags)
{
    if (flags & ~1) /* TIMER_ABSTIME is the only valid flag */
        return -EINVAL;
    if (id < 0 || id >= COMPAT_TIMER_MAX || !g_posix_timers[id].used)
        return -EINVAL;
    if (g_posix_timers[id].owner_pid != owner_pid)
        return -EINVAL;

    g_posix_timers[id].interval[0] = ts[0];
    g_posix_timers[id].interval[1] = ts[1];

    uint64_t ticks = posix_timer_timespec_to_ticks(ts[2], ts[3]);
    if (!ticks) {
        /* it_value == 0 disarms. */
        g_posix_timers[id].expire_tick = 0;
        g_posix_timers[id].overrun_cur = 0;
        g_posix_timers[id].overrun_last = 0;
        posix_timer_update_deadline();
        return 0;
    }
    if (flags & 1) {
        /* TIMER_ABSTIME: ts[2..3] is an absolute time on the timer's clock. */
        uint64_t now_ts[2];
        if (posix_timer_clock_is_realtime(g_posix_timers[id].clockid))
            timekeeping_get_realtime(now_ts);
        else
            timekeeping_get_monotonic(now_ts);
        uint64_t delta_ticks = 0;
        if (ts[2] > now_ts[0] || (ts[2] == now_ts[0] && ts[3] > now_ts[1])) {
            uint64_t sec = ts[2] - now_ts[0];
            uint64_t nsec;
            if (ts[3] >= now_ts[1]) {
                nsec = ts[3] - now_ts[1];
            } else {
                sec--;
                nsec = 1000000000ULL + ts[3] - now_ts[1];
            }
            delta_ticks = posix_timer_timespec_to_ticks(sec, nsec);
        }
        /* A past absolute time expires immediately. */
        g_posix_timers[id].expire_tick = timer_get_ticks() + delta_ticks;
    } else {
        g_posix_timers[id].expire_tick = timer_get_ticks() + ticks;
    }
    posix_timer_update_deadline();
    return 0;
}

/* Is the notification signal for this timer still pending (undelivered)? */
static int posix_timer_signal_pending(posix_timer_t *tmr)
{
    task_t *t = proc_find_get(tmr->target_tid > 0 ? tmr->target_tid
                                                  : tmr->owner_pid);
    if (!t)
        return 0;
    int pending = 0;
    signal_state_t *ss = t->signals;
    if (ss) {
        uint64_t flags = spin_lock_irqsave(&ss->lock);
        uint64_t mask = tmr->target_tid > 0 ? t->thread_pending : ss->pending;
        pending = (mask & signal_mask_bit(tmr->signo)) != 0;
        spin_unlock_irqrestore(&ss->lock, flags);
    }
    proc_put(t);
    return pending;
}

int posix_timer_getoverrun(int owner_pid, int id)
{
    if (id < 0 || id >= COMPAT_TIMER_MAX || !g_posix_timers[id].used)
        return -EINVAL;
    if (g_posix_timers[id].owner_pid != owner_pid)
        return -EINVAL;
    posix_timer_t *tmr = &g_posix_timers[id];
    if (!posix_timer_signal_pending(tmr)) {
        /* The latest notification was delivered/accepted; its overrun count
         * is the current counter.  Latch it so repeated reads stay stable. */
        tmr->overrun_last = tmr->overrun_cur;
        tmr->overrun_cur = 0;
    }
    return tmr->overrun_last > 0x7fffffffULL ? 0x7fffffff
                                             : (int)tmr->overrun_last;
}

void posix_timer_tick(void)
{
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        posix_timer_t *tmr = &g_posix_timers[i];
        if (!tmr->used || tmr->signo == 0)
            continue;
        if (!tmr->expire_tick || now < tmr->expire_tick)
            continue;

        uint64_t interval_ticks = posix_timer_timespec_to_ticks(
            tmr->interval[0], tmr->interval[1]);
        uint64_t expirations = 1;
        if (interval_ticks)
            expirations += (now - tmr->expire_tick) / interval_ticks;

        if (posix_timer_signal_pending(tmr)) {
            /* Previous notification still pending: standard signals
             * coalesce, so extra expirations become overruns. */
            tmr->overrun_cur += expirations;
        } else {            /* Any previous notification was delivered; latch its overrun
             * count before queueing the next one. */
            tmr->overrun_last = tmr->overrun_cur;
            tmr->overrun_cur = 0;
            if (tmr->target_tid > 0) {
                /* SIGEV_THREAD_ID: deliver to the specific thread. */
                task_t *target = proc_find_get(tmr->target_tid);
                if (target) {
                    (void)signal_send_task(target, tmr->signo);
                    proc_put(target);
                }
            } else {
                signal_send(tmr->owner_pid, tmr->signo);
            }
            /* Expirations beyond the first occurred while this new
             * notification is pending. */
            tmr->overrun_cur = expirations - 1;
        }

        if (interval_ticks) {
            /* Drift-free re-arm; expire_tick stays > now afterwards. */
            tmr->expire_tick += expirations * interval_ticks;
            if (tmr->expire_tick <= now)
                tmr->expire_tick = now + interval_ticks;
        } else {
            tmr->expire_tick = 0;
        }
    }

    /* Reap cpu-itimer entries whose owner is gone. */
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        if (!g_cpu_itimers[i].used || !g_cpu_itimers[i].task)
            continue;
        int state = __atomic_load_n(&g_cpu_itimers[i].task->state,
                                    __ATOMIC_ACQUIRE);
        if (state == PROC_UNUSED || state == PROC_ZOMBIE) {
            proc_put(g_cpu_itimers[i].task);
            g_cpu_itimers[i].used = 0;
            g_cpu_itimers[i].task = NULL;
            g_cpu_itimers[i].expire[0] = 0;
            g_cpu_itimers[i].expire[1] = 0;
            g_cpu_itimers[i].interval[0] = 0;
            g_cpu_itimers[i].interval[1] = 0;
        }
    }
    posix_timer_update_deadline();
}

/* ============================================================
 * CPU-time itimers (ITIMER_VIRTUAL / ITIMER_PROF)
 * ============================================================
 *
 * ITIMER_VIRTUAL counts task user-mode ticks, ITIMER_PROF counts
 * user+system ticks.  The metric only advances at the scheduler tick
 * accounting site, so expiry is checked there for the current task.
 */

#define CPU_ITIMER_VIRT 0
#define CPU_ITIMER_PROF 1

static uint64_t cpu_itimer_metric(task_t *t, int which)
{
    if (which == CPU_ITIMER_VIRT)
        return t->utime_ticks;
    return t->utime_ticks + t->stime_ticks;
}

static cpu_itimer_t *cpu_itimer_find(task_t *t)
{
    for (int i = 0; i < COMPAT_TIMER_MAX; i++)
        if (g_cpu_itimers[i].used && g_cpu_itimers[i].task == t)
            return &g_cpu_itimers[i];
    return NULL;
}

static void cpu_itimer_maybe_release(cpu_itimer_t *entry)
{
    if (entry->expire[0] || entry->expire[1])
        return;
    proc_put(entry->task);
    entry->used = 0;
    entry->task = NULL;
}

int posix_itimer_set(task_t *t, int which, uint64_t value_ticks,
                     uint64_t interval_ticks)
{
    if (!t || which < 0 || which > 1)
        return -EINVAL;
    cpu_itimer_t *entry = cpu_itimer_find(t);
    if (!entry) {
        if (!value_ticks && !interval_ticks)
            return 0; /* disarming a timer that was never set */
        for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
            if (g_cpu_itimers[i].used)
                continue;
            task_t *owned = proc_get(t);
            if (!owned)
                return -EAGAIN;
            memset(&g_cpu_itimers[i], 0, sizeof(g_cpu_itimers[i]));
            g_cpu_itimers[i].used = 1;
            g_cpu_itimers[i].task = owned;
            entry = &g_cpu_itimers[i];
            break;
        }
        if (!entry)
            return -EAGAIN;
    }
    entry->interval[which] = interval_ticks;
    entry->expire[which] =
        value_ticks ? cpu_itimer_metric(t, which) + value_ticks : 0;
    cpu_itimer_maybe_release(entry);
    return 0;
}

int posix_itimer_get(task_t *t, int which, uint64_t out[2])
{
    out[0] = 0;
    out[1] = 0;
    if (!t || which < 0 || which > 1)
        return -EINVAL;
    cpu_itimer_t *entry = cpu_itimer_find(t);
    if (!entry)
        return 0;
    out[0] = entry->interval[which];
    if (entry->expire[which]) {
        uint64_t metric = cpu_itimer_metric(t, which);
        out[1] = entry->expire[which] > metric
                     ? entry->expire[which] - metric : 0;
    }
    return 0;
}

void posix_itimer_cpu_tick(task_t *cur)
{
    if (!cur)
        return;
    cpu_itimer_t *entry = cpu_itimer_find(cur);
    if (!entry)
        return;
    for (int which = 0; which < 2; which++) {
        if (!entry->expire[which])
            continue;
        uint64_t metric = cpu_itimer_metric(cur, which);
        if (metric < entry->expire[which])
            continue;
        (void)signal_send_task(cur, which == CPU_ITIMER_VIRT ? SIGVTALRM
                                                             : SIGPROF);
        entry->expire[which] = entry->interval[which]
                                   ? metric + entry->interval[which] : 0;
    }
    cpu_itimer_maybe_release(entry);
}
