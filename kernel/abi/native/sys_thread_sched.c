/* Native scheduling wrappers shared by all execution-domain adapters. */
#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "proc/proc.h"
#include "ipc/handle_table.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "core/errno.h"
#include "sys_validate.h"
#define A20_ARG(n) (args->arg[(n)])
int64_t sys_a20_task_set_sched(const a20_syscall_args_t *args) {
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_sched_args_t *uargs = (a20_sched_args_t *)(uintptr_t)A20_ARG(1);
    if (!uargs)
        return -A20_ERR_FAULT;

    a20_sched_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    /*
     * A20_HANDLE_NULL names the calling task.  The personality needs this
     * for the initial thread, which has no handle in any table, and for
     * restoring a boosted owner after its last mutex is released.
     */
    int is_self = task_h == A20_HANDLE_NULL;
    task_t *target = NULL;
    if (is_self) {
        target = cur;
    } else {
        int64_t r = a20_handle_task_get(ht, task_h, A20_OBJ_INVALID,
                                        A20_RIGHT_CONTROL, &target);
        if (r < 0)
            return r;
        if (!target)
            return -A20_ERR_BAD_HANDLE;
    }

    if ((kargs.flags & (A20_SCHED_DONATE | A20_SCHED_UNDONATE)) != 0) {
        int rc;
        if (kargs.flags == A20_SCHED_DONATE)
            rc = proc_sched_rt_donate(target, kargs.priority);
        else if (kargs.flags == A20_SCHED_UNDONATE)
            rc = proc_sched_rt_restore(target);
        else
            rc = -EINVAL;
        if (!is_self)
            proc_put(target);
        return rc == 0 ? A20_OK
                       : (rc == -EPERM ? -A20_ERR_ACCESS
                                       : -A20_ERR_INVALID_ARGUMENT);
    }

    proc_sched_config_t config = {
        .fields = kargs.flags,
        .policy = kargs.policy,
        .priority = kargs.priority,
        .nice = kargs.nice,
        .affinity = (uint32_t)kargs.affinity,
    };
    if ((kargs.flags & A20_SCHED_AFFINITY) &&
        (kargs.affinity_size < sizeof(uint64_t) || (kargs.affinity >> 32))) {
        if (!is_self)
            proc_put(target);
        return -A20_ERR_INVALID_ARGUMENT;
    }
    int sr = proc_sched_set(target, &config);
    int64_t result = sr == -EPERM ? -A20_ERR_ACCESS :
                     (sr < 0 ? -A20_ERR_INVALID_ARGUMENT : A20_OK);
    if (!is_self)
        proc_put(target);
    return result;
}

int64_t sys_a20_task_get_sched(const a20_syscall_args_t *args) {
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_sched_args_t *out = (a20_sched_args_t *)(uintptr_t)A20_ARG(1);
    if (!out)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    /* A20_HANDLE_NULL names the caller, matching task_set_sched. */
    int is_self = task_h == A20_HANDLE_NULL;
    task_t *target = NULL;
    if (is_self) {
        target = cur;
    } else {
        int64_t r = a20_handle_task_get(ht, task_h, A20_OBJ_INVALID,
                                        A20_RIGHT_STAT, &target);
        if (r < 0)
            return r;
    }
    a20_sched_args_t kinfo;
    memset(&kinfo, 0, sizeof(kinfo));
    kinfo.size = sizeof(kinfo);
    kinfo.version = 1;
    proc_sched_config_t config;
    if (target && proc_sched_get(target, &config) == 0) {
        kinfo.flags = config.fields;
        kinfo.priority = config.priority;
        kinfo.policy = config.policy;
        kinfo.nice = config.nice;
        kinfo.affinity = config.affinity;
        kinfo.affinity_size = sizeof(uint64_t);
    }
    if (!is_self)
        proc_put(target);
    if (copy_to_user(out, &kinfo, sizeof(kinfo)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_sched_lock(const a20_syscall_args_t *args) {
    if (!proc_has_cap(proc_current(), CAP_SYS_NICE))
        return -A20_ERR_ACCESS;
    int32_t mode = (int32_t)args->arg[0];
    int32_t depth = (int32_t)args->arg[1];
    int previous = proc_sched_lock_set(mode, depth);
    if (previous < 0)
        return -A20_ERR_INVALID_ARGUMENT;
    return previous;
}
#endif
