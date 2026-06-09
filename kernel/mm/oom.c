#include "mm/oom.h"
#include "mm/frame.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/timer.h"

#define OOM_COOLDOWN_TICKS MS_TO_TICKS(2000)
#define OOM_MIN_FREE_PAGES 256

static volatile int oom_in_progress;
static uint64_t oom_last_kill_tick;
static unsigned long oom_kill_count;
static int oom_last_victim_pid;
static int oom_last_victim_score;
static unsigned long oom_free_pages_at_kill;

static int oom_pick_victim_pid(void)
{
    int victim_pid = -1;
    int best_score = -1;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t == proc_idle_task() || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
            continue;
        if (t->pid <= 2)
            continue;
        int score = t->policy.oom_score_adj;
        if (!t->mm)
            score -= 100;
        if (score > best_score ||
            (score == best_score && t->pid > victim_pid)) {
            best_score = score;
            victim_pid = t->pid;
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return victim_pid;
}

int oom_try_reclaim(void)
{
    if (__atomic_load_n(&oom_in_progress, __ATOMIC_RELAXED))
        return 0;

    size_t free = pfa_free_count();
    if (free >= OOM_MIN_FREE_PAGES)
        return 0;

    uint64_t now = timer_get_ticks();
    if (now - oom_last_kill_tick < OOM_COOLDOWN_TICKS)
        return 0;

    __atomic_store_n(&oom_in_progress, 1, __ATOMIC_RELAXED);

    int victim_pid = oom_pick_victim_pid();
    if (victim_pid <= 0) {
        __atomic_store_n(&oom_in_progress, 0, __ATOMIC_RELAXED);
        return 0;
    }

    task_t *victim = proc_find(victim_pid);
    if (!victim) {
        __atomic_store_n(&oom_in_progress, 0, __ATOMIC_RELAXED);
        return 0;
    }

    oom_last_kill_tick = now;
    oom_kill_count++;
    oom_last_victim_pid = victim->pid;
    oom_last_victim_score = victim->policy.oom_score_adj;
    oom_free_pages_at_kill = pfa_free_count();
    kerr("[OOM] Killing pid=%d name=%s oom_score_adj=%d free_frames=%lu\n",
         victim->pid, victim->name, victim->policy.oom_score_adj,
         (unsigned long)pfa_free_count());

    proc_force_exit(victim, -9);

    __atomic_store_n(&oom_in_progress, 0, __ATOMIC_RELAXED);
    return 1;
}

void oom_get_stats(oom_stats_t *out)
{
    if (!out)
        return;
    out->kills = oom_kill_count;
    out->last_kill_tick = oom_last_kill_tick;
    out->last_victim_pid = oom_last_victim_pid;
    out->last_victim_score = oom_last_victim_score;
    out->free_pages_at_kill = oom_free_pages_at_kill;
    out->free_pages_now = pfa_free_count();
    out->in_progress = __atomic_load_n(&oom_in_progress, __ATOMIC_RELAXED);
}
