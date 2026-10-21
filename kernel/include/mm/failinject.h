#ifndef _MM_FAILINJECT_H
#define _MM_FAILINJECT_H

/*
 * MM-04 build-only allocation-failure injection.
 *
 * CONFIG_MM_FAIL_STAGE names exactly one allocation stage to fail, once, in a
 * build-only image; it is never set in a normal image, so mm_fail_inject()
 * below folds to a constant 0 and the call sites cost nothing.  A site that
 * fires returns the failure its allocator would have returned (NULL /
 * PFN_NONE / -ENOMEM) and does not skip the operation: a caller that ignores
 * the failure is still the caller's bug.
 *
 * Stage 1's site (an intermediate page-table node) is reached all over boot,
 * so it is gated by a scope the controlled DONTUNMAP destination map opens
 * around its leaf loop; stages 2 and 3 are only reachable from the workload's
 * own operation, so they fire unscoped.
 */
#define MM_FAIL_STAGE_PT_NODE     1
#define MM_FAIL_STAGE_HUGE_DEMOTE 2
#define MM_FAIL_STAGE_SWAP_SLOT   3

#ifdef CONFIG_MM_FAIL_STAGE
int mm_fail_inject_fire(int stage);
int mm_fail_inject_fire_scoped(int stage);
void mm_fail_inject_scope(int on);
#define mm_fail_inject(stage)        mm_fail_inject_fire(stage)
#define mm_fail_inject_scoped(stage) mm_fail_inject_fire_scoped(stage)
#define mm_fail_inject_enter()       mm_fail_inject_scope(1)
#define mm_fail_inject_leave()       mm_fail_inject_scope(0)
#else
#define mm_fail_inject(stage)        0
#define mm_fail_inject_scoped(stage) 0
#define mm_fail_inject_enter()       ((void)0)
#define mm_fail_inject_leave()       ((void)0)
#endif

#endif /* _MM_FAILINJECT_H */
