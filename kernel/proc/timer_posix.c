#include "proc/posix_timer.h"
#include "core/errno.h"
#include "core/string.h"
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
 */

#define COMPAT_TIMER_MAX 32

typedef struct {
    int used;
    int owner_pid;
    int signo;
    int target_tid;      /* SIGEV_THREAD_ID delivery target, 0 = process-wide */
    uint64_t interval[2];
    uint64_t value[2];
    uint64_t expire_tick;
} posix_timer_t;

static posix_timer_t g_posix_timers[COMPAT_TIMER_MAX];

static uint64_t posix_timer_timespec_to_ticks(uint64_t sec, uint64_t nsec)
{
    return sec * TICKS_PER_SEC +
           (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
}

int posix_timer_create(int owner_pid, int signo, int target_tid)
{
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        if (!g_posix_timers[i].used) {
            memset(&g_posix_timers[i], 0, sizeof(g_posix_timers[i]));
            g_posix_timers[i].used = 1;
            g_posix_timers[i].owner_pid = owner_pid;
            g_posix_timers[i].signo = signo;
            g_posix_timers[i].target_tid = target_tid;
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
    return 0;
}

int posix_timer_get_time(int owner_pid, int id, uint64_t out[4])
{
    if (id < 0 || id >= COMPAT_TIMER_MAX || !g_posix_timers[id].used)
        return -EINVAL;
    if (g_posix_timers[id].owner_pid != owner_pid)
        return -EINVAL;
    out[0] = g_posix_timers[id].interval[0];
    out[1] = g_posix_timers[id].interval[1];
    out[2] = g_posix_timers[id].value[0];
    out[3] = g_posix_timers[id].value[1];
    return 0;
}

int posix_timer_set_time(int owner_pid, int id, const uint64_t ts[4])
{
    if (id < 0 || id >= COMPAT_TIMER_MAX || !g_posix_timers[id].used)
        return -EINVAL;
    if (g_posix_timers[id].owner_pid != owner_pid)
        return -EINVAL;
    memcpy(g_posix_timers[id].interval, ts, sizeof(uint64_t) * 2);
    memcpy(g_posix_timers[id].value, ts + 2, sizeof(uint64_t) * 2);
    g_posix_timers[id].expire_tick =
        timer_get_ticks() + posix_timer_timespec_to_ticks(ts[2], ts[3]);
    return 0;
}

int posix_timer_getoverrun(int owner_pid, int id)
{
    if (id < 0 || id >= COMPAT_TIMER_MAX || !g_posix_timers[id].used)
        return -EINVAL;
    if (g_posix_timers[id].owner_pid != owner_pid)
        return -EINVAL;
    return 0;
}

void posix_timer_tick(void)
{
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        if (!g_posix_timers[i].used || g_posix_timers[i].signo == 0)
            continue;
        if (g_posix_timers[i].expire_tick > 0 &&
            now >= g_posix_timers[i].expire_tick) {
            if (g_posix_timers[i].target_tid > 0) {
                /* SIGEV_THREAD_ID: deliver to the specific thread. */
                task_t *target = proc_find_get(g_posix_timers[i].target_tid);
                if (target) {
                    (void)signal_send_task(target, g_posix_timers[i].signo);
                    proc_put(target);
                }
            } else {
                signal_send(g_posix_timers[i].owner_pid,
                            g_posix_timers[i].signo);
            }
            if (g_posix_timers[i].interval[0] || g_posix_timers[i].interval[1]) {
                uint64_t interval_ticks = posix_timer_timespec_to_ticks(
                    g_posix_timers[i].interval[0], g_posix_timers[i].interval[1]);
                g_posix_timers[i].expire_tick = now + interval_ticks;
            } else {
                g_posix_timers[i].expire_tick = 0;
            }
        }
    }
}
