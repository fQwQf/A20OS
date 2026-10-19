#ifndef A20_ABI_LINUX_RLIMIT_POLICY_H
#define A20_ABI_LINUX_RLIMIT_POLICY_H

#include "core/types.h"

/*
 * C/Rust ABI for the resource-limit policy in
 * kernel/abi/linux/rlimit_policy.rs.
 *
 * Rust owns the resource-to-field mapping, the clamps, which resources are
 * unenforced, and the privilege rules; C keeps the usercopy of the pairs, the
 * signal-owned core limit, the park lock around the RT pair and the task
 * lookup.
 */
typedef struct rlimit_view {
    uint64_t *stack;
    uint64_t *nofile;
    uint64_t *aspace;
    uint64_t *nproc;
    uint64_t *rtprio;
    uint64_t *rtprio_max;
    const int *uid;
} rlimit_view_t;

_Static_assert(sizeof(rlimit_view_t) == 7 * sizeof(void *),
               "rlimit_view_t layout must match the Rust LimitsView");

struct task_t;

/* Adapter bridges. */
int      a20_rl_view_fill(const struct task_t *t, rlimit_view_t *out);
uint64_t a20_rl_park_lock(struct task_t *t);
void     a20_rl_park_unlock(struct task_t *t, uint64_t flags);
int      a20_rl_has_cap(struct task_t *t, int cap);
void     a20_rl_core_set(struct task_t *t, uint64_t limit);
int      a20_rl_current_uid(void);
struct task_t *a20_rl_current(void);

/* Rust policy. */
int a20_rlimit_get(struct task_t *t, const rlimit_view_t *view, int resource,
                   uint64_t core, uint64_t *pair);
int a20_rlimit_set(struct task_t *t, const rlimit_view_t *view, int resource,
                   uint64_t cur, uint64_t max);
int a20_prlimit_may_change(const rlimit_view_t *view, int is_self, int has_cap);

#endif /* A20_ABI_LINUX_RLIMIT_POLICY_H */
