#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef SYS_sched_setattr
#define SYS_sched_setattr 274
#endif

#ifndef SYS_sched_getattr
#define SYS_sched_getattr 275
#endif

#ifndef SYS_mount
#define SYS_mount 40
#endif

#ifndef SYS_umount2
#define SYS_umount2 39
#endif

#ifndef SYS_sched_getscheduler
#define SYS_sched_getscheduler 120
#endif

#ifndef SYS_sched_setscheduler
#define SYS_sched_setscheduler 119
#endif

#ifndef SYS_sched_getparam
#define SYS_sched_getparam 121
#endif

#ifndef SYS_sched_get_priority_max
#define SYS_sched_get_priority_max 125
#endif

#ifndef SYS_sched_get_priority_min
#define SYS_sched_get_priority_min 126
#endif

#ifndef SYS_getpriority
#define SYS_getpriority 141
#endif

#ifndef SYS_getcpu
#define SYS_getcpu 168
#endif

#ifndef SYS_setpriority
#define SYS_setpriority 140
#endif

struct sched_attr_compat {
    unsigned int size;
    unsigned int sched_policy;
    unsigned long sched_flags;
    int sched_nice;
    unsigned int sched_priority;
    unsigned long sched_runtime;
    unsigned long sched_deadline;
    unsigned long sched_period;
};

static int fail(const char *what)
{
    printf("SCHED_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int priority_bounds(void)
{
    if (syscall(SYS_sched_get_priority_min, SCHED_OTHER) != 0)
        return fail("normal-prio-min");
    if (syscall(SYS_sched_get_priority_max, SCHED_OTHER) != 0)
        return fail("normal-prio-max");
    if (syscall(SYS_sched_get_priority_min, SCHED_FIFO) != 1)
        return fail("fifo-prio-min");
    if (syscall(SYS_sched_get_priority_max, SCHED_FIFO) != 99)
        return fail("fifo-prio-max");
    errno = 0;
    if (syscall(SYS_sched_get_priority_min, 999) >= 0 || errno != EINVAL)
        return fail("invalid-policy-prio");
    return 0;
}

static int policy_param_roundtrip(void)
{
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    if (syscall(SYS_sched_getscheduler, 0) != SCHED_OTHER)
        return fail("initial-policy");
    if (syscall(SYS_sched_getparam, 0, &param) < 0 || param.sched_priority != 0)
        return fail("normal-getparam");
    if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &param) == 0)
        return fail("fifo-zero-prio");
    param.sched_priority = 1;
    if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &param) < 0)
        return fail("set-fifo");
    if (syscall(SYS_sched_getscheduler, 0) != SCHED_FIFO)
        return fail("get-fifo");
    memset(&param, 0, sizeof(param));
    if (syscall(SYS_sched_getparam, 0, &param) < 0 || param.sched_priority != 1)
        return fail("fifo-getparam");
    param.sched_priority = 0;
    if (syscall(SYS_sched_setscheduler, 0, SCHED_OTHER, &param) < 0)
        return fail("restore-other");
    return 0;
}

static int affinity_roundtrip(void)
{
    unsigned char mask[8];
    memset(mask, 0, sizeof(mask));
    long n = syscall(SYS_sched_getaffinity, 0, sizeof(mask), mask);
    if (n <= 0)
        return fail("getaffinity");
    if ((mask[0] & 1U) == 0)
        return fail("affinity-missing-cpu0");

    /*
     * Step 3.5 validates affinity accounting but deliberately does not require
     * migration of an on-CPU task. Pin to the CPU executing this process; retry
     * if preemption moved it between getcpu and setaffinity.
     */
    unsigned char one[8];
    unsigned pinned_cpu = 0;
    int pinned = 0;
    for (int attempt = 0; attempt < 32 && !pinned; attempt++) {
        unsigned cpu = 0;
        if (syscall(SYS_getcpu, &cpu, NULL, NULL) < 0 || cpu >= 64)
            return fail("getcpu");
        memset(one, 0, sizeof(one));
        one[cpu / 8] = (unsigned char)(1U << (cpu % 8));
        if (syscall(SYS_sched_setaffinity, 0, sizeof(one), one) == 0) {
            pinned_cpu = cpu;
            pinned = 1;
        } else if (errno != EINVAL) {
            return fail("setaffinity-current");
        }
    }
    if (!pinned)
        return fail("setaffinity-current-raced");

    memset(mask, 0, sizeof(mask));
    if (syscall(SYS_sched_getaffinity, 0, sizeof(mask), mask) <= 0)
        return fail("getaffinity-after-set");
    if ((mask[pinned_cpu / 8] & (1U << (pinned_cpu % 8))) == 0)
        return fail("affinity-current-after-set");
    unsigned char empty[8] = {0};
    errno = 0;
    if (syscall(SYS_sched_setaffinity, 0, sizeof(empty), empty) == 0 || errno != EINVAL)
        return fail("setaffinity-empty");
    return 0;
}

static int nice_and_attr(void)
{
    errno = 0;
    if (syscall(SYS_setpriority, PRIO_PROCESS, 0, 7) < 0)
        return fail("setpriority");
    errno = 0;
    if (syscall(SYS_getpriority, PRIO_PROCESS, 0) != 13 || errno != 0)
        return fail("getpriority");
    struct sched_attr_compat attr;
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    attr.sched_nice = 3;
    if (syscall(SYS_sched_setattr, 0, &attr, 0) < 0)
        return fail("sched-setattr");
    memset(&attr, 0, sizeof(attr));
    attr.size = sizeof(attr);
    if (syscall(SYS_sched_getattr, 0, &attr, sizeof(attr), 0) < 0)
        return fail("sched-getattr");
    if (attr.sched_nice != 3)
        return fail("sched-attr-nice");
    return 0;
}

static int write_all(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY);
    if (fd < 0)
        return fail(path);
    size_t len = strlen(text);
    int ok = write(fd, text, len) == (ssize_t)len;
    close(fd);
    return ok ? 0 : fail(path);
}

static int cgroup_cpuset_affinity(void)
{
    mkdir("/tmp/sched_cg", 0755);
    long mounted = syscall(SYS_mount, "none", "/tmp/sched_cg", "cgroup", 0, "cpuset");
    if (mounted < 0) {
        if (errno == ENOSYS || errno == ENOENT || errno == EINVAL || errno == EPERM)
            return 0;
        return fail("cgroup-mount");
    }

    unsigned cpu = 0;
    if (syscall(SYS_getcpu, &cpu, NULL, NULL) < 0 || cpu >= 32)
        return fail("cgroup-getcpu");
    char cpubuf[16];
    snprintf(cpubuf, sizeof(cpubuf), "%u\n", cpu);
    if (write_all("/tmp/sched_cg/cpuset.cpus", cpubuf) != 0)
        return 1;

    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%d\n", (int)getpid());
    if (write_all("/tmp/sched_cg/tasks", pidbuf) != 0)
        return 1;

    unsigned char mask[8];
    memset(mask, 0, sizeof(mask));
    if (syscall(SYS_sched_getaffinity, 0, sizeof(mask), mask) <= 0)
        return fail("cgroup-getaffinity");
    if ((mask[cpu / 8] & (1U << (cpu % 8))) == 0)
        return fail("cgroup-cpuset-mask");

    syscall(SYS_umount2, "/tmp/sched_cg", 0);
    rmdir("/tmp/sched_cg");
    return 0;
}

int main(void)
{
    printf("SCHED_STRESS: start\n");
    if (priority_bounds() != 0)
        return 1;
    if (policy_param_roundtrip() != 0)
        return 1;
    if (affinity_roundtrip() != 0)
        return 1;
    if (nice_and_attr() != 0)
        return 1;
    if (cgroup_cpuset_affinity() != 0)
        return 1;
    printf("SCHED_STRESS: PASS\n");
    return 0;
}
