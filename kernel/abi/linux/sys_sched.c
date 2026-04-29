#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

int64_t sys_sched_get_priority_max(int policy)
{
    return (policy == 1 || policy == 2) ? 99 : 0;
}

int64_t sys_sched_get_priority_min(int policy)
{
    return (policy == 1 || policy == 2) ? 1 : 0;
}

int64_t sys_sched_getaffinity(int pid, size_t cpusetsize, void *mask)
{
    (void)pid;
    if (!mask || cpusetsize == 0) return -EINVAL;
    uint8_t one = 1;
    if (copy_to_user(mask, &one, cpusetsize < 1 ? cpusetsize : 1) < 0) return -EFAULT;
    return (int64_t)cpusetsize;
}

int64_t sys_sched_setaffinity(int pid, size_t cpusetsize, const void *mask)
{
    (void)pid;
    if (!mask || cpusetsize == 0) return -EINVAL;
    uint8_t tmp;
    return copy_from_user(&tmp, mask, 1) < 0 ? -EFAULT : 0;
}

static task_t *sched_task_for_pid(int pid)
{
    if (pid == 0) return proc_current();
    return proc_find(pid);
}

int64_t sys_sched_getparam(int pid, void *param)
{
    if (!param) return -EFAULT;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int prio = t->priority;
    return copy_to_user(param, &prio, sizeof(prio)) < 0 ? -EFAULT : 0;
}

int64_t sys_sched_setparam(int pid, const void *param)
{
    if (!param) return -EFAULT;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int prio;
    if (copy_from_user(&prio, param, sizeof(prio)) < 0) return -EFAULT;
    t->priority = prio;
    return 0;
}

int64_t sys_sched_getscheduler(int pid)
{
    task_t *t = sched_task_for_pid(pid);
    return t ? 0 : -ESRCH;
}

int64_t sys_sched_setscheduler(int pid, int policy, const void *param)
{
    if (policy < 0 || policy > 2) return -EINVAL;
    if (param) return sys_sched_setparam(pid, param);
    return sched_task_for_pid(pid) ? 0 : -ESRCH;
}

int64_t sys_sched_rr_get_interval(int pid, void *tp)
{
    if (!tp) return -EFAULT;
    if (!sched_task_for_pid(pid)) return -ESRCH;
    uint64_t ts[2] = {0, 1000000000ULL / 100};
    return copy_to_user(tp, ts, sizeof(ts)) < 0 ? -EFAULT : 0;
}

int64_t sys_getpriority(int which, int who)
{
    (void)which;
    task_t *t = sched_task_for_pid(who);
    return t ? t->priority : -ESRCH;
}

int64_t sys_setpriority(int which, int who, int prio)
{
    (void)which;
    task_t *t = sched_task_for_pid(who);
    if (!t) return -ESRCH;
    if (prio < -20) prio = -20;
    if (prio > 19) prio = 19;
    t->priority = prio;
    return 0;
}

int64_t sys_nice(int inc)
{
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    return sys_setpriority(0, 0, t->priority + inc) < 0 ? -EPERM : t->priority;
}

int64_t sys_seteuid(int euid)
{
    return sys_setreuid(-1, euid);
}

int64_t sys_setegid(int egid)
{
    return sys_setregid(-1, egid);
}

