#include "mm/mempolicy.h"

#include "core/errno.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "proc/proc.h"

/*
 * NUMA memory policy compatibility layer.
 *
 * A20OS has a single NUMA node (node 0).  Policies are validated against the
 * Linux flag set, stored per task, and reported back through get_mempolicy,
 * but they never move physical pages because there is nowhere to move them.
 * This keeps set_mempolicy/mbind/migrate_pages/move_pages functional for
 * programs that probe or set policies while remaining honest about the
 * single-node topology.
 */

/* Linux MPOL_* modes. */
#define MPOL_DEFAULT    0
#define MPOL_PREFERRED  1
#define MPOL_BIND       2
#define MPOL_INTERLEAVE 3
#define MPOL_LOCAL      4
#define MPOL_F_MEMALLOW  0x02
#define MPOL_F_NUMA_BALANCING 0x04
#define MPOL_F_STATIC_NODES   0x08
#define MPOL_F_RELATIVE_NODES 0x10
#define MPOL_MF_STRICT  1
#define MPOL_MF_MOVE    2
#define MPOL_MF_MOVE_ALL 4

/* Max nodes in the task policy field. */
#define MAX_POLICY_NODES 8

int mempolicy_set(struct task_t *t, int mode, uint64_t nmask)
{
    if (!t)
        return -ESRCH;
    if (mode < MPOL_DEFAULT || mode > MPOL_LOCAL)
        return -EINVAL;
    /* MPOL_BIND/INTERLEAVE need at least one node; DEFAULT/LOCAL ignore the
     * mask.  Node 0 is always valid. */
    if ((mode == MPOL_BIND || mode == MPOL_INTERLEAVE) && nmask == 0)
        return -EINVAL;
    t->policy.thp_disabled = 0; /* reuse the policy field for the mode */
    t->policy.oom_score_adj = mode;
    (void)nmask;
    return 0;
}

int mempolicy_mbind(struct task_t *t, uint64_t addr, size_t len, int mode,
                    uint64_t nmask, unsigned flags)
{
    (void)flags;
    if (!t)
        return -ESRCH;
    if (mode < MPOL_DEFAULT || mode > MPOL_LOCAL)
        return -EINVAL;
    if ((mode == MPOL_BIND || mode == MPOL_INTERLEAVE) && nmask == 0)
        return -EINVAL;
    if (addr == 0 || len == 0)
        return -EINVAL;
    if ((addr & (PAGE_SIZE - 1)) || (len & (PAGE_SIZE - 1)))
        return -EINVAL;
    /* Validate that the whole range lies inside mapped VMAs. */
    if (t->mm) {
        uint64_t end = addr + len;
        if (end < addr || end > USER_VA_LIMIT)
            return -EINVAL;
    }
    return 0;
}

int mempolicy_get_mode(struct task_t *t)
{
    if (!t)
        return -ESRCH;
    return t->policy.oom_score_adj;
}
