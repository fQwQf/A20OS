#ifndef _A20_TASK_H
#define _A20_TASK_H

#include "a20_types.h"
#include "a20_syscall.h"

static inline void a20_task_exit(int code)
{
    a20_syscall6(A20_SYS_task_exit, (uint64_t)(int32_t)code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

static inline a20_status_t a20_task_spawn(a20_task_spawn_args_t *args)
{
    args->size = sizeof(*args);
    args->version = 1;
    return a20_syscall6(A20_SYS_task_spawn, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_task_wait(a20_handle_t task, a20_flags_t flags,
                                          a20_task_status_t *out)
{
    return a20_syscall6(A20_SYS_task_wait, task, flags, (uint64_t)out,
                        0, 0, 0);
}

static inline a20_status_t a20_task_kill(a20_handle_t task, uint32_t reason)
{
    return a20_syscall6(A20_SYS_task_kill, task, (uint64_t)reason, 0, 0, 0, 0);
}

static inline a20_status_t a20_task_info(a20_handle_t task, a20_task_info_t *out)
{
    return a20_syscall6(A20_SYS_task_info, task, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_thread_create(a20_thread_create_args_t *args)
{
    args->size = sizeof(*args);
    args->version = 1;
    return a20_syscall6(A20_SYS_thread_create, (uint64_t)args, 0, 0, 0, 0, 0);
}

static inline void a20_thread_exit(int code)
{
    a20_syscall6(A20_SYS_thread_exit, (uint64_t)(int32_t)code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

static inline a20_status_t a20_thread_sleep(a20_time_t duration)
{
    uint64_t deadline_ns = duration.secs * 1000000000ULL + duration.nsecs;
    return a20_syscall6(A20_SYS_thread_sleep, deadline_ns, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_thread_yield(void)
{
    return a20_syscall6(A20_SYS_thread_yield, 0, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_thread_get_cpu(uint32_t *cpu)
{
    return a20_syscall6(A20_SYS_thread_get_cpu, (uint64_t)cpu, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_sched_set(a20_handle_t task,
                                          a20_sched_args_t *args)
{
    args->size = sizeof(*args);
    args->version = 1;
    return a20_syscall6(A20_SYS_task_set_sched, task, (uint64_t)args,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_sched_get(a20_handle_t task,
                                          a20_sched_args_t *out)
{
    return a20_syscall6(A20_SYS_task_get_sched, task, (uint64_t)out,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_task_set_limit(a20_handle_t task,
                                               a20_resource_limits_t *limits)
{
    return a20_syscall6(A20_SYS_task_set_limits, task, (uint64_t)limits,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_task_get_limit(a20_handle_t task,
                                               a20_resource_limits_t *out)
{
    return a20_syscall6(A20_SYS_task_get_limits, task, (uint64_t)out,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_task_get_status(a20_handle_t task,
                                                a20_task_status_t *out)
{
    (void)task;
    (void)out;
    return -A20_ERR_NOT_SUPPORTED;
}

static inline a20_status_t a20_thread_set_name(a20_handle_t thread,
                                                const char *name)
{
    (void)thread;
    (void)name;
    return -A20_ERR_NOT_SUPPORTED;
}

static inline a20_status_t a20_thread_get_name(a20_handle_t thread,
                                                char *buf, uint64_t len)
{
    (void)thread;
    (void)buf;
    (void)len;
    return -A20_ERR_NOT_SUPPORTED;
}

/*
 * Checkpoint-based signal simulation.  Signals are never delivered
 * asynchronously: they are recorded by a20_task_kill() and consumed here at
 * explicit checkpoints (event_wait return, pthread_testcancel).  Returns the
 * bitmap of delivered signals (pending & ~blocked) and clears them.
 */
static inline int64_t a20_signal_check(void)
{
    return a20_syscall6(A20_SYS_signal_check, 0, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_signal_mask(uint64_t new_mask, uint64_t *old_mask)
{
    return a20_syscall6(A20_SYS_signal_mask, new_mask, (uint64_t)old_mask, 0, 0, 0, 0);
}

#endif
