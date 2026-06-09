#ifndef _OOM_H
#define _OOM_H

/*
 * OOM_RECLAIM_LIFETIME_CONTRACT:
 * - Reclaim may free only objects with no task/mm, VMA, page-table, page-cache,
 *   VMO, or Native handle owner.
 * - oom_try_reclaim() may pick a victim and ask normal proc exit/mm teardown to
 *   release memory; it must not directly free frames still reachable from an mm.
 */

typedef struct {
    unsigned long kills;
    unsigned long last_kill_tick;
    int last_victim_pid;
    int last_victim_score;
    unsigned long free_pages_at_kill;
    unsigned long free_pages_now;
    int in_progress;
} oom_stats_t;

void oom_get_stats(oom_stats_t *out);
int oom_try_reclaim(void);

#endif
