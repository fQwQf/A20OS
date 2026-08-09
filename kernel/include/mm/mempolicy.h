#ifndef _MM_MEMPOLICY_H
#define _MM_MEMPOLICY_H

/*
 * NUMA memory policy helpers (mempolicy(2) family).
 *
 * A20OS currently has a single NUMA node, so the policies are validated and
 * stored per-process but have no physical NUMA effect.  The entry points are
 * ABI-independent; the ABI layer translates the user node masks.
 */

#include "core/types.h"

struct task_t;

/* set_mempolicy(2): @mode is MPOL_*, @nmask is a kernel copy of the node
 * mask (up to MAX_NUMNODES bits, truncated to a u64 for the single-node
 * model).  Returns 0 or a negative errno. */
int mempolicy_set(struct task_t *t, int mode, uint64_t nmask);

/* mbind(2): apply a policy to a memory range.  The range is validated and
 * the policy stored per-task; no physical NUMA action occurs. */
int mempolicy_mbind(struct task_t *t, uint64_t addr, size_t len, int mode,
                    uint64_t nmask, unsigned flags);

/* get_mempolicy(2) support: returns the current default mode. */
int mempolicy_get_mode(struct task_t *t);

#endif /* _MM_MEMPOLICY_H */
