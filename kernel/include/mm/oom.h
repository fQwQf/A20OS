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
    /* kswapd background reclaim (hosted builds) */
    unsigned long kswapd_passes;
    unsigned long kswapd_pages_freed;
    unsigned long kswapd_last_pass_tick;
} oom_stats_t;

void oom_get_stats(oom_stats_t *out);
int oom_try_reclaim(void);
/* Paced swap-only reclaim for the background reclaimer (no OOM kill, no
 * cooldown); returns pages reclaimed. */
int oom_swap_reclaim_pages(int target_pages);
/* Kernel-thread entry for the background reclaimer; main.c spawns it with
 * proc_alloc() after the MM is up. */
void oom_kswapd_thread(void);

#endif
