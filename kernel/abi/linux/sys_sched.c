#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "proc/proc_internal.h"
#include "cg/cgroup_impl.h"

/* LINUX_ABI_SCHED_STUB_BOUNDARY: policy APIs expose bounded compatibility for
 * current scheduler fields; they do not claim full Linux RT/cgroup semantics. */

static task_t *sched_task_for_pid(int pid)
{
    if (pid == 0) return proc_get(proc_current());
    return proc_find_get(pid);
}

static size_t sched_cpu_mask_bytes(void)
{
    size_t word_bits = sizeof(unsigned long) * 8;
    return ((CONFIG_NR_CPUS + word_bits - 1) / word_bits) *
           sizeof(unsigned long);
}

static int sched_policy_rt(int policy)
{
    return policy == SCHED_FIFO || policy == SCHED_RR;
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

int64_t sys_sched_get_priority_max(int policy)
{
    int min, max;
    if (proc_sched_priority_range(policy, &min, &max) < 0)
        return -EINVAL;
    return max;
}

int64_t sys_sched_get_priority_min(int policy)
{
    int min, max;
    if (proc_sched_priority_range(policy, &min, &max) < 0)
        return -EINVAL;
    return min;
}

int64_t sys_sched_getaffinity(int pid, size_t cpusetsize, void *mask)
{
    if (!mask || cpusetsize == 0) return -EINVAL;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;

    size_t mask_bytes = sched_cpu_mask_bytes();
    if (cpusetsize < mask_bytes) {
        proc_put(t);
        return -EINVAL;
    }
    uint32_t allowed = proc_sched_effective_affinity(t);
    uint8_t out[mask_bytes];
    memset(out, 0, sizeof(out));
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS && cpu < 32; cpu++) {
        if (allowed & (1U << cpu))
            out[cpu / 8] |= (uint8_t)(1U << (cpu % 8));
    }

    if (copy_to_user(mask, out, mask_bytes) < 0) {
        proc_put(t);
        return -EFAULT;
    }
    proc_put(t);
    return (int64_t)mask_bytes;
}

int64_t sys_getcpu(unsigned *cpu, unsigned *node, void *cache)
{
    (void)cache;
    unsigned current = cpu_current_id();
    unsigned numa_node = 0;

    if (cpu && copy_to_user(cpu, &current, sizeof(current)) < 0)
        return -EFAULT;
    if (node && copy_to_user(node, &numa_node, sizeof(numa_node)) < 0)
        return -EFAULT;
    return 0;
}

int64_t sys_sched_setaffinity(int pid, size_t cpusetsize, const void *mask)
{
    if (!mask || cpusetsize == 0) return -EINVAL;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;

    size_t mask_bytes = sched_cpu_mask_bytes();
    if (cpusetsize < mask_bytes) {
        proc_put(t);
        return -EINVAL;
    }
    uint8_t in[mask_bytes];
    memset(in, 0, sizeof(in));
    if (copy_from_user(in, mask, mask_bytes) < 0) {
        proc_put(t);
        return -EFAULT;
    }

    uint32_t allowed = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (in[cpu / 8] & (uint8_t)(1U << (cpu % 8))) {
            if (cpu < 32)
                allowed |= 1U << cpu;
        }
    }
    proc_sched_config_t config = {
        .fields = PROC_SCHED_AFFINITY,
        .affinity = allowed,
    };
    int64_t result = proc_sched_set(t, &config) < 0 ? -EINVAL : 0;
    proc_put(t);
    return result;
}

int64_t sys_sched_getparam(int pid, void *param)
{
    if (!param) return -EFAULT;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int prio = sched_param_for_task(t);
    int64_t result =
        copy_to_user(param, &prio, sizeof(prio)) < 0 ? -EFAULT : 0;
    proc_put(t);
    return result;
}

int64_t sys_sched_setparam(int pid, const void *param)
{
    if (!param) return -EFAULT;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int prio;
    if (copy_from_user(&prio, param, sizeof(prio)) < 0) {
        proc_put(t);
        return -EFAULT;
    }
    proc_sched_config_t config = {
        .fields = PROC_SCHED_PRIORITY,
        .priority = prio,
    };
    int64_t result = proc_sched_set(t, &config) < 0 ? -EINVAL : 0;
    proc_put(t);
    return result;
}

int64_t sys_sched_getscheduler(int pid)
{
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int result = t->sched_policy |
                 (t->sched_reset_on_fork ? SCHED_RESET_ON_FORK : 0);
    proc_put(t);
    return result;
}

int64_t sys_sched_setscheduler(int pid, int policy, const void *param)
{
    int reset_on_fork = !!(policy & SCHED_RESET_ON_FORK);
    policy &= ~SCHED_RESET_ON_FORK;
    if (!param) return -EFAULT;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    int prio;
    if (copy_from_user(&prio, param, sizeof(prio)) < 0) {
        proc_put(t);
        return -EFAULT;
    }
    proc_sched_config_t config = {
        .fields = PROC_SCHED_POLICY | PROC_SCHED_PRIORITY,
        .policy = policy,
        .priority = prio,
        .reset_on_fork = reset_on_fork,
    };
    int64_t result = proc_sched_set(t, &config) < 0 ? -EINVAL : 0;
    proc_put(t);
    return result;
}

int64_t sys_sched_rr_get_interval(int pid, void *tp)
{
    if (!tp) return -EFAULT;
    if (pid < 0) return -EINVAL;
    task_t *t = sched_task_for_pid(pid);
    if (!t) return -ESRCH;
    proc_put(t);
    uint64_t ts[2] = {0, 1000000000ULL / 100};
    return copy_to_user(tp, ts, sizeof(ts)) < 0 ? -EFAULT : 0;
}

int64_t sys_getpriority(int which, int who)
{
    (void)which;
    task_t *t = sched_task_for_pid(who);
    if (!t)
        return -ESRCH;
    int result = 20 - sched_nice_for_task(t);
    proc_put(t);
    return result;
}

int64_t sys_setpriority(int which, int who, int prio)
{
    (void)which;
    task_t *t = sched_task_for_pid(who);
    if (!t) return -ESRCH;
    if (prio < -20) prio = -20;
    if (prio > 19) prio = 19;
    proc_sched_config_t config = {
        .fields = PROC_SCHED_NICE,
        .nice = prio,
    };
    int64_t result = proc_sched_set(t, &config) < 0 ? -EINVAL : 0;
    proc_put(t);
    return result;
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
    if (copy_from_user(buf, attr, sizeof(buf)) < 0) {
        proc_put(t);
        return -EFAULT;
    }
    int prio = *(int *)(buf + 16);
    if (prio >= -20 && prio <= 19) {
        if (!sched_policy_rt(t->sched_policy))
            t->priority = prio;
        t->cfs_weight = sched_weight_for_nice(prio);
    }
    proc_put(t);
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
    int64_t result =
        copy_to_user(attr, buf, size < sizeof(buf) ? size : sizeof(buf)) < 0
            ? -EFAULT : 0;
    proc_put(t);
    return result;
}
