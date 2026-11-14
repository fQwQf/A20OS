/* Stable native scanout policy. Callers validate roles and exclude headless
 * rendering devices first. Prefer rendering, then managed output over retained
 * firmware output; keep equal ties. Backend-specific vtables do not participate. */
#ifndef _DRIVERS_GPU_GPU_SELECT_H
#define _DRIVERS_GPU_GPU_SELECT_H
#include <stdbool.h>

/* The priority is core policy, not an arbitrary number supplied by drivers. */
#define GPU_SELECT_MAX_RANK 3U
static inline unsigned gpu_select_rank(bool render, bool firmware)
{
    return (render ? 2U : 0U) + (firmware ? 0U : 1U);
}
static inline bool gpu_select_should_take(bool has_incumbent, unsigned incumbent_rank,
                                          unsigned newcomer_rank)
{
    return !has_incumbent || newcomer_rank > incumbent_rank;
}
static inline bool gpu_select_better(bool have_best, unsigned best_rank,
                                     unsigned candidate_rank)
{
    return !have_best || candidate_rank > best_rank;
}
#endif
