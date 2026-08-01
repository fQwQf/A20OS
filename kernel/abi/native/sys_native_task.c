/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * This file is part of the mechanically split Native Phase 2 ABI.
 * See sys_phase2.c for shared helpers and forward declarations.
 */
#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/version.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/cpu.h"
#include "core/random.h"
#include "trap_frame.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "fs/xattr.h"
#include "net/socket.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);
extern int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                           uint16_t type, a20_rights_t rights,
                                           uint64_t expiry_tick, uint32_t remaining_ops,
                                           uint32_t temporal_flags, uint8_t security_label);
extern int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                          uint16_t expected_type, a20_rights_t required_rights,
                                          a20_handle_entry_t *out);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);

/* ===== Task (0x0200) continued ===== */

int64_t sys_a20_task_kill(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    int32_t sig = (int32_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_SIGNAL, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    if (!target) return -A20_ERR_BAD_HANDLE;

    if (sig == 9) {
        proc_put(target);
        proc_exit(128 + 9);
    }

    /* Checkpoint-based delivery: record the signal and wake the target if it
     * is parked at a checkpoint (event/futex wait).  The handler runs on the
     * next explicit checkpoint (signal_check / pthread_testcancel), never
     * asynchronously. */
    struct a20_ht_internal *tht = task_get_a20_ht(target);
    if (tht)
        a20_ht_sig_pend(tht, sig);
    proc_interrupt_wait(target, PROC_WAKE_SIGNAL);
    proc_put(target);
    return A20_OK;
}

int64_t sys_a20_signal_check(const a20_syscall_args_t *args)
{
    (void)args;
    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return 0;
    return (int64_t)a20_ht_sig_take(ht);
}

int64_t sys_a20_signal_mask(const a20_syscall_args_t *args)
{
    uint64_t new_mask = A20_ARG(0);
    uint64_t *old_out = (uint64_t *)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    uint64_t old = a20_ht_sig_set_blocked(ht, new_mask);
    if (old_out && copy_to_user(old_out, &old, sizeof(old)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_task_info(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_task_info_t *out = (a20_task_info_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    if (!target) return -A20_ERR_BAD_HANDLE;

    a20_task_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.version = 1;
    info.pid = target->pid;
    info.ppid = target->ppid;
    info.thread_count = (target->tgid == target->pid) ? 1 : 0;
    /* Fill VM stats from target's mm_struct */
    if (target->mm) {
        info.vm_size = target->mm->total_vm * 4096ULL;
        info.vm_rss = target->mm->rss * 4096ULL;
    }
    /* CPU time: convert ticks to nanoseconds at 100ns/tick */
    info.user_time_ns = target->total_time * 10000000ULL;
    info.sys_time_ns = 0; /* kernel time not separately tracked */

    proc_put(target);
    if (copy_to_user(out, &info, sizeof(info)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_thread_sleep(const a20_syscall_args_t *args)
{
    uint64_t duration_ns = A20_ARG(0);
    uint64_t now_ns = timer_get_ticks() * (1000000000ULL / TICKS_PER_SEC);
    uint64_t deadline_ns = now_ns + duration_ns;
    while (timer_get_ticks() * (1000000000ULL / TICKS_PER_SEC) < deadline_ns) {
        proc_yield();
    }
    return A20_OK;
}

int64_t sys_a20_thread_yield(const a20_syscall_args_t *args)
{
    (void)args;
    proc_yield();
    return A20_OK;
}

int64_t sys_a20_thread_get_cpu(const a20_syscall_args_t *args)
{
    uint32_t *out = (uint32_t *)A20_ARG(0);
    uint32_t cpu = cpu_current_id();
    if (!out || copy_to_user(out, &cpu, sizeof(cpu)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_thread_exit(const a20_syscall_args_t *args)
{
    int32_t code = (int32_t)A20_ARG(0);
    proc_exit(code);
    return 0;
}

int64_t sys_a20_task_get_sched(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_sched_args_t *out = (a20_sched_args_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

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

int64_t sys_a20_task_get_usage(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_rusage_t *out = (a20_rusage_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    a20_rusage_t usage;
    memset(&usage, 0, sizeof(usage));
    if (target) {
        usage.user_time_ns = target->total_time * 10000000ULL;
        usage.max_rss = target->mm ? target->mm->rss * 4096ULL : 0;
        usage.sys_time_ns = (target->child_utime + target->child_stime) * 10000000ULL;
    }
    proc_put(target);
    if (copy_to_user(out, &usage, sizeof(usage)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}
