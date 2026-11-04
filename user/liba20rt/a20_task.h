#ifndef _A20_TASK_H
#define _A20_TASK_H

#include "a20_types.h"
#include "a20_syscall.h"
#include "../../kernel/include/uapi/a20/task_signal.h"
#include "../../kernel/include/uapi/a20/thread_exit.h"
#include "../../kernel/include/uapi/a20/task_status.h"

static inline void a20_task_exit(int code)
{
    a20_syscall6(A20_SYS_task_exit, (uint64_t)(int32_t)code, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}

/* Explicit versioned exits; a successful call does not return. Scalar exit
 * wrappers retain their original contract and ignore unused registers. */
static inline a20_status_t a20_task_exit_extended(a20_exit_args_t *request) {
    if (sizeof(uintptr_t) != 8) return -A20_ERR_NOT_SUPPORTED;
    request->size = sizeof(*request); request->version = 1;
    return a20_syscall6(A20_SYS_task_exit, A20_EXIT_STRUCT, (uint64_t)(uintptr_t)request, 0, 0, 0, 0);
}
static inline a20_status_t a20_thread_exit_extended(a20_exit_args_t *request) {
    if (sizeof(uintptr_t) != 8) return -A20_ERR_NOT_SUPPORTED;
    request->size = sizeof(*request); request->version = 1;
    return a20_syscall6(A20_SYS_thread_exit, A20_EXIT_STRUCT, (uint64_t)(uintptr_t)request, 0, 0, 0, 0);
}

static inline a20_status_t a20_task_spawn(a20_task_spawn_args_t *args)
{
    args->size = sizeof(*args);
    args->version = 1;
    return a20_syscall6(A20_SYS_task_spawn, (uint64_t)(uintptr_t)args, 0, 0, 0, 0, 0);
}

/* Poll instead of block: -A20_ERR_WOULD_BLOCK when the task is still running. */
#define A20_TASK_WAIT_NONBLOCK 0x1u

static inline a20_status_t a20_task_wait(a20_handle_t task, a20_flags_t flags,
                                          a20_task_status_t *out)
{
    return a20_syscall6(A20_SYS_task_wait, task, flags, (uint64_t)(uintptr_t)out,
                        0, 0, 0);
}

static inline a20_status_t a20_task_kill(a20_handle_t task, uint32_t reason)
{
    return a20_syscall6(A20_SYS_task_kill, task, (uint64_t)reason, 0, 0, 0, 0);
}

static inline a20_status_t a20_task_info(a20_handle_t task, a20_task_info_t *out)
{
    return a20_syscall6(A20_SYS_task_info, task, (uint64_t)(uintptr_t)out, 0, 0, 0, 0);
}

static inline a20_status_t a20_thread_create(a20_thread_create_args_t *args)
{
    args->size = sizeof(*args);
    args->version = 1;
    return a20_syscall6(A20_SYS_thread_create, (uint64_t)(uintptr_t)args, 0, 0, 0, 0, 0);
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
    return a20_syscall6(A20_SYS_thread_get_cpu, (uint64_t)(uintptr_t)cpu, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_sched_set(a20_handle_t task,
                                          a20_sched_args_t *args)
{
    args->size = sizeof(*args);
    args->version = 1;
    return a20_syscall6(A20_SYS_task_set_sched, task, (uint64_t)(uintptr_t)args,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_sched_get(a20_handle_t task,
                                          a20_sched_args_t *out)
{
    return a20_syscall6(A20_SYS_task_get_sched, task, (uint64_t)(uintptr_t)out,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_sched_lock(int32_t mode, int32_t depth)
{
    return a20_syscall6(A20_SYS_sched_lock, (uint32_t)mode, (uint32_t)depth,
                        0, 0, 0, 0);
}

/* Install and return a handle for the calling task in its own table. */
static inline int64_t a20_task_self(void)
{
    return a20_syscall6(A20_SYS_task_self, 0, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_task_set_limit(a20_handle_t task,
                                               a20_resource_limits_t *limits)
{
    return a20_syscall6(A20_SYS_task_set_limits, task, (uint64_t)(uintptr_t)limits,
                        0, 0, 0, 0);
}

static inline a20_status_t a20_task_get_limit(a20_handle_t task,
                                               a20_resource_limits_t *out)
{
    return a20_syscall6(A20_SYS_task_get_limits, task, (uint64_t)(uintptr_t)out,
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
    return a20_syscall6(A20_SYS_signal_mask, new_mask, (uint64_t)(uintptr_t)old_mask, 0, 0, 0, 0);
}

/* These variants opt into isolated thread masks/queues. The original calls
 * above retain their process-wide checkpoint semantics. */
static inline a20_status_t a20_thread_signal(a20_handle_t thread,
                                             uint32_t sig) {
    return a20_syscall6(A20_SYS_task_kill, thread, sig, A20_SIGNAL_THREAD, 0, 0,
                        0);
}
static inline a20_status_t
a20_thread_signal_control(a20_signal_control_t *args) {
    return a20_syscall6(A20_SYS_signal_check, A20_SIGNAL_STRUCT,
                        (uint64_t)(uintptr_t)args, 0, 0, 0, 0);
}
static inline int64_t a20_thread_signal_check(uint32_t flags,
                                              uint64_t selected) {
    a20_signal_control_t args = {sizeof(args),
                                 1,
                                 A20_SIGNAL_CONTROL_CHECK,
                                 (uint32_t)A20_SIGNAL_THREAD | flags,
                                 selected,
                                 0,
                                 0};
    a20_status_t status = a20_thread_signal_control(&args);
    return status < 0 ? status : (int64_t)args.result;
}
static inline a20_status_t a20_thread_signal_mask(uint64_t mask, uint64_t *old,
                                                  uint32_t flags) {
    a20_signal_control_t args = {sizeof(args),
                                 1,
                                 A20_SIGNAL_CONTROL_MASK,
                                 (uint32_t)A20_SIGNAL_THREAD | flags,
                                 0,
                                 mask,
                                 0};
    a20_status_t status = a20_thread_signal_control(&args);
    if (status == A20_OK && old)
        *old = args.result;
    return status;
}
/* Returns the child pid to its parent and zero to the child. Child outputs
 * and manifest child_handle values are written only into the child's VM. */
static inline a20_status_t a20_task_clone(a20_clone_args_t *args) {
    return a20_syscall6(A20_SYS_task_clone, (uint64_t)(uintptr_t)args, 0, 0, 0, 0, 0);
}
static inline a20_status_t a20_task_execve(const char *path,
        char *const argv[], char *const envp[], const a20_exec_bootstrap_args_t *bootstrap) {
    if (!bootstrap) return a20_syscall6(A20_SYS_execve, (uint64_t)(uintptr_t)path,
        (uint64_t)(uintptr_t)argv, (uint64_t)(uintptr_t)envp, 0, 0, 0);
    if (sizeof(uintptr_t) != 8) return -A20_ERR_NOT_SUPPORTED;
    a20_exec_bootstrap_args_t request = *bootstrap;
    request.path = (uint64_t)(uintptr_t)path;
    request.argv = (uint64_t)(uintptr_t)argv;
    request.envp = (uint64_t)(uintptr_t)envp;
    return a20_syscall6(A20_SYS_execve, A20_EXEC_STRUCT,
        (uint64_t)(uintptr_t)&request, 0, 0, 0, 0);
}
#endif
