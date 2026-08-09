#include "proc/sched_compat.h"

#include "core/errno.h"
#include "proc/proc.h"

int ioprio_set_task(struct task_t *caller, int which, int who, int ioprio)
{
    if (!caller)
        return -ESRCH;
    if (ioprio < 0 || (ioprio & ~0xffff))
        return -EINVAL;
    unsigned cls = ((unsigned)ioprio >> IOPRIO_CLASS_SHIFT) & IOPRIO_CLASS_MASK;
    unsigned data = (unsigned)ioprio & IOPRIO_DATA_MASK;
    if (data > 7)
        return -EINVAL;
    if (cls == IOPRIO_CLASS_NONE)
        return data == 0 ? 0 : -EINVAL;
    if (cls != IOPRIO_CLASS_RT && cls != IOPRIO_CLASS_BE &&
        cls != IOPRIO_CLASS_IDLE)
        return -EINVAL;
    if (cls == IOPRIO_CLASS_RT && !proc_has_cap(caller, CAP_SYS_ADMIN))
        return -EPERM;

    if (which == IOPRIO_WHO_PROCESS) {
        struct task_t *t = who ? proc_find_get(who) : caller;
        if (!t)
            return -ESRCH;
        if (t != caller && !proc_has_cap(caller, CAP_SYS_NICE) &&
            caller->cred.euid != t->cred.euid && caller->cred.euid != t->cred.uid) {
            proc_put(t);
            return -EPERM;
        }
        t->ioprio = ioprio;
        if (t != caller)
            proc_put(t);
        return 0;
    }
    if (which == IOPRIO_WHO_PGRP || which == IOPRIO_WHO_USER) {
        /* Apply to the whole group by iterating the task list; the ABI layer
         * does the iteration, so this common entry only supports PROCESS. */
        return -EINVAL;
    }
    return -EINVAL;
}

int ioprio_get_task(struct task_t *caller, int which, int who)
{
    if (!caller)
        return -ESRCH;
    if (which != IOPRIO_WHO_PROCESS)
        return -EINVAL;
    struct task_t *t = who ? proc_find_get(who) : caller;
    if (!t)
        return -ESRCH;
    int v = t->ioprio;
    if (t != caller)
        proc_put(t);
    return v;
}

int pkey_alloc(struct task_t *t, unsigned flags)
{
    if (!t)
        return -ESRCH;
    if (flags)
        return -EINVAL;
    for (int i = 0; i < PKEY_MAX_KEYS; i++) {
        if (!(t->pkey_bitset & (1u << i))) {
            t->pkey_bitset |= (1u << i);
            return i;
        }
    }
    return -ENOSPC;
}

int pkey_free(struct task_t *t, int key)
{
    if (!t)
        return -ESRCH;
    if (key < 0 || key >= PKEY_MAX_KEYS)
        return -EINVAL;
    if (!(t->pkey_bitset & (1u << key)))
        return -EINVAL;
    t->pkey_bitset &= ~(1u << key);
    return 0;
}

int pkey_valid(struct task_t *t, int key)
{
    if (!t)
        return 0;
    if (key < 0 || key >= PKEY_MAX_KEYS)
        return 0;
    return (t->pkey_bitset & (1u << key)) != 0;
}
