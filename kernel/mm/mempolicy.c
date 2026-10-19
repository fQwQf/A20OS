#include "mm/mempolicy.h"

#include "core/errno.h"
#include "core/types.h"
#include "mm/vm.h"
#include "proc/proc.h"

/*
 * NUMA memory policy.
 *
 * The validation and the state transitions live in
 * kernel/mm/mempolicy_state.rs: which modes are legal, when a mask is
 * required, which mask is actually stored, and the mbind range contract.  What
 * is left here is the task-field access the Rust module cannot name and the
 * two getters, which are pure field reads.
 */

int mempolicy_get_mode(struct task_t *t)
{
    if (!t)
        return -ESRCH;
    return t->policy.mempolicy_mode;
}

uint64_t mempolicy_get_nmask(struct task_t *t)
{
    if (!t)
        return 0;
    return t->policy.mempolicy_nmask;
}

int a20_mempolicy_has_mm(const void *t)
{
    return t && ((const task_t *)t)->mm;
}

uintptr_t a20_mempolicy_user_va_limit(void)
{
    return USER_VA_LIMIT;
}

int a20_mempolicy_store(void *t, int mode, uint64_t nmask)
{
    task_t *task = t;
    if (!task)
        return -ESRCH;
    task->policy.mempolicy_mode = mode;
    task->policy.mempolicy_nmask = nmask;
    return 0;
}

