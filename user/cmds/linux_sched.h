#ifndef A20_USER_LINUX_SCHED_H
#define A20_USER_LINUX_SCHED_H

#include <sched.h>
#include <sys/syscall.h>
#include <unistd.h>

/* musl intentionally leaves the process sched_* interfaces as ENOSYS stubs.
 * These kernel tools exercise Linux scheduling syscalls explicitly; do not
 * replace libc symbols or modify the upstream C library. */
static inline int a20_linux_sched_setscheduler(pid_t pid, int policy,
                                               const struct sched_param *param)
{
    return (int)syscall(SYS_sched_setscheduler, pid, policy, param);
}
static inline int a20_linux_sched_getscheduler(pid_t pid)
{
    return (int)syscall(SYS_sched_getscheduler, pid);
}
static inline int a20_linux_sched_getparam(pid_t pid, struct sched_param *param)
{
    return (int)syscall(SYS_sched_getparam, pid, param);
}
static inline int a20_linux_sched_setparam(pid_t pid, const struct sched_param *param)
{
    return (int)syscall(SYS_sched_setparam, pid, param);
}
#endif
