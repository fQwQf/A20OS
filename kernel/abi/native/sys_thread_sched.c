/* Native scheduling wrappers shared by all execution-domain adapters. */
#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "proc/proc.h"
#include "ipc/handle_table.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
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

    a20_handle_entry_t entry;
    int64_t r =
        a20_handle_lookup_task_like(ht, task_h, A20_RIGHT_CONTROL, &entry);
    if (r < 0)
        return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    if (!target)
        return -A20_ERR_BAD_HANDLE;

    proc_sched_config_t config = {
        .fields = kargs.flags,
        .policy = kargs.policy,
        .priority = kargs.priority,
        .nice = kargs.nice,
        .affinity = (uint32_t)kargs.affinity,
    };
    if ((kargs.flags & A20_SCHED_AFFINITY) &&
        (kargs.affinity_size < sizeof(uint64_t) || (kargs.affinity >> 32))) {
        proc_put(target);
        return -A20_ERR_INVALID_ARGUMENT;
    }
    int64_t result = proc_sched_set(target, &config) < 0
                         ? -A20_ERR_INVALID_ARGUMENT
                         : A20_OK;
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

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_task_like(ht, task_h, A20_RIGHT_STAT, &entry);
    if (r < 0)
        return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
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
    proc_put(target);
    if (copy_to_user(out, &kinfo, sizeof(kinfo)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}
#endif
