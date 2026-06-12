#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "proc/proc_internal.h"
#include "cg/cgroup_impl.h"

/* LINUX_ABI_SCHED_STUB_BOUNDARY: policy APIs expose bounded compatibility for
 * current scheduler fields; they do not claim full Linux RT/cgroup semantics. */

static task_t *sched_task_for_pid(int pid)
{
    if (pid == 0) return proc_current();
    return proc_find(pid);
}

static size_t sched_cpu_mask_bytes(void)
{
    return (CONFIG_NR_CPUS + 7) / 8;
}

static int sched_policy_valid(int policy)
{
    return policy == SCHED_NORMAL || policy == SCHED_FIFO ||
           policy == SCHED_RR || policy == SCHED_BATCH ||
           policy == SCHED_IDLE;
}

static int sched_policy_rt(int policy)
{
    return policy == SCHED_FIFO || policy == SCHED_RR;
}

static int sched_param_valid(int policy, int prio)
{
    if (sched_policy_rt(policy))
        return prio >= 1 && prio <= 99;
    return prio == 0;
}

static int sched_nice_for_weight(uint32_t weight)
{
    for (int nice = -20; nice <= 19; nice++) {
        if (sched_weight_for_nice(nice) == weight)
            return nice;
    }
    return 0;
}

static int sched_nice_for_task(task_t *t)
{
    if (sched_policy_rt(t->sched_policy))
        return sched_nice_for_weight(t->cfs_weight);
    return t->priority;
}

static int sched_param_for_task(task_t *t)
{
    if (!sched_policy_rt(t->sched_policy))
        return 0;
    return (t->priority >= 1 && t->priority <= 99) ? t->priority : 1;
}

static uint32_t sched_effective_cpu_mask(task_t *t)
{
#if CONFIG_NR_CPUS >= 32
    uint32_t all = ~0U;
#else
    uint32_t all = (1U << CONFIG_NR_CPUS) - 1U;
#endif
    uint32_t allowed = t ? t->cpus_allowed : all;
    if (t && t->cgroup) {
        cg_node_t *node = (cg_node_t *)t->cgroup;
        allowed &= node->res.cpuset.effective_cpus;
    }
    allowed &= all;
    return allowed ? allowed : all;
}

int64_t sys_sched_get_priority_max(int policy)
{
    if (!sched_policy_valid(policy))
        return -EINVAL;
    return sched_policy_rt(policy) ? 99 : 0;
}

int64_t sys_sched_get_priority_min(int policy)
{
    if (!sched_policy_valid(policy))
        return -EINVAL;
    return sched_policy_rt(policy) ? 1 : 0;
}

int64_t sys_sched_getaffinity(int pid, size_t cpusetsize, void *mask)
{
    if (!mask || cpusetsize == 0) return -EINVAL;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;

    size_t mask_bytes = sched_cpu_mask_bytes();
    if (cpusetsize < mask_bytes) return -EINVAL;
    uint32_t allowed = sched_effective_cpu_mask(t);
    uint8_t out[(CONFIG_NR_CPUS + 7) / 8];
    memset(out, 0, sizeof(out));
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && cpu < 32; cpu++) {
        if (allowed & (1U << cpu))
            out[cpu / 8] |= (uint8_t)(1U << (cpu % 8));
    }

    if (copy_to_user(mask, out, mask_bytes) < 0) return -EFAULT;
    return (int64_t)mask_bytes;
}

int64_t sys_sched_setaffinity(int pid, size_t cpusetsize, const void *mask)
{
    if (!mask || cpusetsize == 0) return -EINVAL;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;

    size_t mask_bytes = sched_cpu_mask_bytes();
    if (cpusetsize < mask_bytes) return -EINVAL;
    uint8_t in[(CONFIG_NR_CPUS + 7) / 8];
    memset(in, 0, sizeof(in));
    if (copy_from_user(in, mask, mask_bytes) < 0) return -EFAULT;

    int nonempty = 0;
    uint32_t allowed = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (in[cpu / 8] & (uint8_t)(1U << (cpu % 8))) {
            nonempty = 1;
            if (cpu < 32)
                allowed |= 1U << cpu;
        }
    }
    if (!nonempty) return -EINVAL;
    if (allowed)
        t->cpus_allowed = allowed;
    return 0;
}

int64_t sys_sched_getparam(int pid, void *param)
{
    if (!param) return -EFAULT;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int prio = sched_param_for_task(t);
    return copy_to_user(param, &prio, sizeof(prio)) < 0 ? -EFAULT : 0;
}

int64_t sys_sched_setparam(int pid, const void *param)
{
    if (!param) return -EFAULT;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int prio;
    if (copy_from_user(&prio, param, sizeof(prio)) < 0) return -EFAULT;
    if (!sched_param_valid(t->sched_policy, prio)) return -EINVAL;
    if (sched_policy_rt(t->sched_policy))
        t->priority = prio;
    return 0;
}

int64_t sys_sched_getscheduler(int pid)
{
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    return t->sched_policy;
}

int64_t sys_sched_setscheduler(int pid, int policy, const void *param)
{
    if (!sched_policy_valid(policy)) return -EINVAL;
    if (!param) return -EFAULT;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int prio;
    if (copy_from_user(&prio, param, sizeof(prio)) < 0) return -EFAULT;
    if (!sched_param_valid(policy, prio)) return -EINVAL;
    if (sched_policy_rt(policy)) {
        t->priority = prio;
    } else if (sched_policy_rt(t->sched_policy)) {
        t->priority = sched_nice_for_weight(t->cfs_weight);
    }
    t->sched_policy = policy;
    return 0;
}

int64_t sys_sched_rr_get_interval(int pid, void *tp)
{
    if (!tp) return -EFAULT;
    if (pid < 0) return -EINVAL;
    if (!sched_task_for_pid(pid)) return -ESRCH;
    uint64_t ts[2] = {0, 1000000000ULL / 100};
    return copy_to_user(tp, ts, sizeof(ts)) < 0 ? -EFAULT : 0;
}

int64_t sys_getpriority(int which, int who)
{
    (void)which;
    task_t *t = sched_task_for_pid(who);
    return t ? 20 - sched_nice_for_task(t) : -ESRCH;
}

int64_t sys_setpriority(int which, int who, int prio)
{
    (void)which;
    task_t *t = sched_task_for_pid(who);
    if (!t) return -ESRCH;
    if (prio < -20) prio = -20;
    if (prio > 19) prio = 19;
    if (!sched_policy_rt(t->sched_policy))
        t->priority = prio;
    t->cfs_weight = sched_weight_for_nice(prio);
    return 0;
}

int64_t sys_nice(int inc)
{
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    int prio = sched_nice_for_task(t) + inc;
    return sys_setpriority(0, 0, prio) < 0 ? -EPERM : sched_nice_for_task(t);
}

int64_t sys_seteuid(int euid)
{
    return sys_setreuid(-1, euid);
}

int64_t sys_setegid(int egid)
{
    return sys_setregid(-1, egid);
}

int64_t sys_sched_setattr(int pid, const void *attr, unsigned flags)
{
    (void)flags;
    if (!attr) return -EFAULT;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    uint8_t buf[64];
    if (copy_from_user(buf, attr, sizeof(buf)) < 0) return -EFAULT;
    int prio = *(int *)(buf + 16);
    if (prio >= -20 && prio <= 19) {
        if (!sched_policy_rt(t->sched_policy))
            t->priority = prio;
        t->cfs_weight = sched_weight_for_nice(prio);
    }
    return 0;
}

int64_t sys_sched_getattr(int pid, void *attr, unsigned size, unsigned flags)
{
    (void)flags;
    if (!attr || size < 48) return -EINVAL;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    uint8_t buf[64];
    memset(buf, 0, sizeof(buf));
    *(unsigned int *)(buf + 0) = size;
    *(unsigned int *)(buf + 4) = 0;
    *(unsigned int *)(buf + 8) = 0;
    *(int *)(buf + 16) = sched_nice_for_task(t);
    return copy_to_user(attr, buf, size < sizeof(buf) ? size : sizeof(buf)) < 0 ? -EFAULT : 0;
}
