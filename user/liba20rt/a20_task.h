#ifndef _A20_TASK_H
#define _A20_TASK_H

#include "a20_types.h"
#include "a20_syscall.h"

static inline void a20_task_exit(int code)
{
    a20_syscall6(A20_SYS_task_exit, (uint64_t)(int32_t)code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

static inline a20_status_t a20_task_spawn(const a20_task_spawn_args_t *args)
{
    return a20_syscall6(A20_SYS_task_spawn, (uint64_t)args, 0, 0, 0, 0, 0);
}

typedef struct {
    a20_handle_t task;
    int32_t      exit_code;
    uint32_t     exit_reason;
    uint64_t     usage_user_time;
    uint64_t     usage_sys_time;
} a20_task_wait_result_t;

static inline a20_status_t a20_task_wait(a20_handle_t task,
                                          a20_task_wait_result_t *out)
{
    return a20_syscall6(A20_SYS_task_wait, task, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_task_kill(a20_handle_t task, uint32_t reason)
{
    return a20_syscall6(A20_SYS_task_kill, task, reason, 0, 0, 0, 0);
}

static inline a20_status_t a20_task_info(a20_handle_t task, a20_task_info_t *out)
{
    return a20_syscall6(A20_SYS_task_info, task, (uint64_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_thread_create(const a20_thread_create_args_t *args,
                                              a20_handle_t *out)
{
    return a20_syscall6(A20_SYS_thread_create, (uint64_t)args, (uint64_t)out,
                        0, 0, 0, 0);
}

static inline void a20_thread_exit(void)
{
    a20_syscall6(A20_SYS_thread_exit, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

static inline a20_status_t a20_thread_sleep(a20_time_t duration)
{
    return a20_syscall6(A20_SYS_thread_sleep, duration.secs, duration.nsecs,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_thread_yield(void)
{
    return a20_syscall6(A20_SYS_thread_yield, 0, 0, 0, 0, 0, 0);
}

#endif
