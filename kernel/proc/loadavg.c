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
 * The runnable count comes from the per-CPU runqueue tallies, so the tick does
 * not walk the task list and does not have to agree with itself across CPUs.
 * The live-task census that /proc/loadavg also prints is taken when it is read,
 * not on every tick: it is display-only, so paying for it in the tick would
 * charge every CPU a full list walk 100 times a second for a value that a
 * reader looks at occasionally.
 *
 * The EMAs are stored in Q16 fixed point so a fractional load survives
 * rounding, and decay is applied per tick with a shift derived from the
 * window, matching the PSI accounting in kernel/core/psi.c.
 */

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/cpu.h"
#include "core/timer.h"
#include "core/string.h"

#define LOADAVG_FSHIFT 16
#define LOADAVG_ONE    (1U << LOADAVG_FSHIFT)

static uint64_t g_loadavg[3];      /* Q16 fixed point, 1/5/15 minute */
static uint64_t g_loadavg_last_tick;
static unsigned g_loadavg_running; /* most recent sample */

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

void proc_loadavg_tick(void)
{
    /* One sampler, not one per CPU: the EMAs decay per window, so several
     * samplers in the same tick period would weight the figures by the CPU
     * count rather than by elapsed time. */
    if (cpu_current_id() != 0)
        return;
    uint64_t now = timer_get_ticks();
    unsigned running = (unsigned)proc_runq_load_sum();
    g_loadavg_running = running;

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

/* Count live tasks and the highest live pid under tasklist_lock; each task's
 * ->state is sampled under its own park_lock (E1/E2). */
static void loadavg_task_census(unsigned *total, int *max_pid)
{
    unsigned n = 0;
    int hi = 0;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (proc_task_state_get(t) == PROC_UNUSED)
            continue;
        n++;
        if (t->pid > hi)
            hi = t->pid;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    if (total) *total = n;
    if (max_pid) *max_pid = hi;
}

void proc_loadavg_snapshot(uint64_t *avg1, uint64_t *avg5, uint64_t *avg15,
                           unsigned *running, unsigned *total, int *max_pid)
{
    if (avg1) *avg1 = __atomic_load_n(&g_loadavg[0], __ATOMIC_RELAXED);
    if (avg5) *avg5 = __atomic_load_n(&g_loadavg[1], __ATOMIC_RELAXED);
    if (avg15) *avg15 = __atomic_load_n(&g_loadavg[2], __ATOMIC_RELAXED);
    if (running) *running = __atomic_load_n(&g_loadavg_running, __ATOMIC_RELAXED);
    if (total || max_pid)
        loadavg_task_census(total, max_pid);
}
