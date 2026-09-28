/*
 * /proc/loadavg and /proc/stat's load-column sampling.
 *
 * Linux defines the 1/5/15-minute figures as exponentially weighted moving
 * averages of the number of runnable tasks, updated at the scheduler tick.
 * This file maintains those averages so /proc/loadavg reports measured
 * values rather than a constant.
 *
 * Sampling counts RUNNABLE + RUNNING tasks, matching Linux's `nr_running`.
 * Uninterruptible sleepers are not included: A20OS has no D state, because
 * device I/O is synchronous (see kernel/core/psi.c for the same reasoning
 * applied to PSI "io").
 *
 * The EMAs are stored in Q16 fixed point so a fractional load survives
 * rounding, and decay is applied per tick with a shift derived from the
 * window, matching the PSI accounting in kernel/core/psi.c.
 */

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/timer.h"
#include "core/string.h"

#define LOADAVG_FSHIFT 16
#define LOADAVG_ONE    (1U << LOADAVG_FSHIFT)

static uint64_t g_loadavg[3];      /* Q16 fixed point, 1/5/15 minute */
static uint64_t g_loadavg_last_tick;
static unsigned g_loadavg_running; /* most recent sample */
static unsigned g_loadavg_total;   /* most recent sample */
static int g_loadavg_max_pid;

static int loadavg_alpha_shift(uint64_t window_ticks)
{
    int s = 0;
    uint64_t v = window_ticks;
    while (v >>= 1)
        s++;
    return s < 1 ? 1 : (s > 31 ? 31 : s);
}

static void loadavg_ema_update(uint64_t *avg, unsigned sample, int shift)
{
    *avg += (((uint64_t)sample << LOADAVG_FSHIFT) - *avg) >> shift;
}

/* Count runnable tasks and total live tasks under proc_lock. */
static void loadavg_sample_counts(unsigned *running, unsigned *total,
                                  int *max_pid)
{
    unsigned r = 0, n = 0;
    int hi = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED)
            continue;
        n++;
        if (t->pid > hi)
            hi = t->pid;
        if (t->state == PROC_READY || t->state == PROC_RUNNING)
            r++;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    *running = r;
    *total = n;
    *max_pid = hi;
}

void proc_loadavg_tick(void)
{
    uint64_t now = timer_get_ticks();
    unsigned running, total;
    int max_pid;
    loadavg_sample_counts(&running, &total, &max_pid);
    g_loadavg_running = running;
    g_loadavg_total = total;
    g_loadavg_max_pid = max_pid;

    if (g_loadavg_last_tick == 0) {
        g_loadavg_last_tick = now;
        return;
    }
    uint64_t elapsed = now - g_loadavg_last_tick;
    if (elapsed == 0)
        return;
    g_loadavg_last_tick = now;

    static const unsigned windows[3] = { 60, 300, 900 };
    for (int i = 0; i < 3; i++) {
        int shift = loadavg_alpha_shift((uint64_t)windows[i] * TICKS_PER_SEC);
        loadavg_ema_update(&g_loadavg[i], running, shift);
    }
}

void proc_loadavg_snapshot(uint64_t *avg1, uint64_t *avg5, uint64_t *avg15,
                           unsigned *running, unsigned *total, int *max_pid)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (avg1) *avg1 = g_loadavg[0];
    if (avg5) *avg5 = g_loadavg[1];
    if (avg15) *avg15 = g_loadavg[2];
    if (running) *running = g_loadavg_running;
    if (total) *total = g_loadavg_total;
    if (max_pid) *max_pid = g_loadavg_max_pid;
    spin_unlock_irqrestore(&proc_lock, flags);
}
