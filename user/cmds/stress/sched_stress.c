#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include "../linux_sched.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

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

typedef struct sched_diag {
    unsigned long runqueue_migrations;
    unsigned long runqueue_local_picks;
    unsigned long runqueue_empty_picks;
    unsigned long runqueue_lock_acquires;
    unsigned long runqueue_lock_contentions;
    unsigned long runqueue_parallel_pick_peak;
    unsigned long resched_requests;
    unsigned long resched_priority_requests;
    unsigned long resched_ipi_sent;
    unsigned long resched_ipi_acks;
    unsigned long resched_consumed;
    unsigned long scheduler_violations;
    unsigned seen;
} sched_diag_t;

enum {
    SEEN_MIGRATIONS = 1U << 0,
    SEEN_REQUESTS = 1U << 1,
    SEEN_PRIORITY_REQUESTS = 1U << 2,
    SEEN_IPI_SENT = 1U << 3,
    SEEN_IPI_ACKS = 1U << 4,
    SEEN_CONSUMED = 1U << 5,
    SEEN_VIOLATIONS = 1U << 6,
    SEEN_LOCAL_PICKS = 1U << 7,
    SEEN_EMPTY_PICKS = 1U << 8,
    SEEN_LOCK_ACQUIRES = 1U << 9,
    SEEN_LOCK_CONTENTIONS = 1U << 10,
    SEEN_PARALLEL_PICK_PEAK = 1U << 11,
    SEEN_SCHED_DIAG_ALL = (1U << 12) - 1,
};

static int fail(const char *what)
{
    printf("SCHED_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int g_absent;
static int g_skipped;

static int absent(const char *what, int err)
{
    printf("SCHED_STRESS: ABSENT %s (not implemented, errno=%d)\n", what, err);
    g_absent++;
    return 0;
}

static int skip_env(const char *what, int err)
{
    printf("SCHED_STRESS: SKIP %s (not exercised in this run, errno=%d)\n",
           what, err);
    g_skipped++;
    return 0;
}

static int read_sched_diag(sched_diag_t *diag)
{
    memset(diag, 0, sizeof(*diag));
    FILE *fp = fopen("/proc/a20/task_lifetime", "r");
    if (!fp)
        return fail("open-sched-diag");

    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        char key[64];
        unsigned long value;
        if (sscanf(line, "%63[^:]: %lu", key, &value) != 2)
            continue;
        if (strcmp(key, "runqueue_migrations") == 0) {
            diag->runqueue_migrations = value;
            diag->seen |= SEEN_MIGRATIONS;
        } else if (strcmp(key, "runqueue_local_picks") == 0) {
            diag->runqueue_local_picks = value;
            diag->seen |= SEEN_LOCAL_PICKS;
        } else if (strcmp(key, "runqueue_empty_picks") == 0) {
            diag->runqueue_empty_picks = value;
            diag->seen |= SEEN_EMPTY_PICKS;
        } else if (strcmp(key, "runqueue_lock_acquires") == 0) {
            diag->runqueue_lock_acquires = value;
            diag->seen |= SEEN_LOCK_ACQUIRES;
        } else if (strcmp(key, "runqueue_lock_contentions") == 0) {
            diag->runqueue_lock_contentions = value;
            diag->seen |= SEEN_LOCK_CONTENTIONS;
        } else if (strcmp(key, "runqueue_parallel_pick_peak") == 0) {
            diag->runqueue_parallel_pick_peak = value;
            diag->seen |= SEEN_PARALLEL_PICK_PEAK;
        } else if (strcmp(key, "resched_requests") == 0) {
            diag->resched_requests = value;
            diag->seen |= SEEN_REQUESTS;
        } else if (strcmp(key, "resched_priority_requests") == 0) {
            diag->resched_priority_requests = value;
            diag->seen |= SEEN_PRIORITY_REQUESTS;
        } else if (strcmp(key, "resched_ipi_sent") == 0) {
            diag->resched_ipi_sent = value;
            diag->seen |= SEEN_IPI_SENT;
        } else if (strcmp(key, "resched_ipi_acks") == 0) {
            diag->resched_ipi_acks = value;
            diag->seen |= SEEN_IPI_ACKS;
        } else if (strcmp(key, "resched_consumed") == 0) {
            diag->resched_consumed = value;
            diag->seen |= SEEN_CONSUMED;
        } else if (strcmp(key, "scheduler_violations") == 0) {
            diag->scheduler_violations = value;
            diag->seen |= SEEN_VIOLATIONS;
        }
    }
    fclose(fp);
    if (diag->seen != SEEN_SCHED_DIAG_ALL)
        return fail("parse-sched-diag");
    return 0;
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

static int rt_permission_limits(void)
{
    pid_t pid = fork();
    if (pid < 0) return fail("rt-limit-fork");
    if (pid == 0) {
        struct rlimit limit = { .rlim_cur = 0, .rlim_max = 10 };
        struct sched_param param = { .sched_priority = 1 };
        if (setrlimit(RLIMIT_RTPRIO, &limit) || setuid(1000))
            _exit(fail("rt-limit-setup"));
        errno = 0;
        if (a20_linux_sched_setscheduler(0, SCHED_FIFO, &param) != -1 || errno != EPERM)
            _exit(fail("rt-zero-limit-entry"));
        limit.rlim_cur = 10;
        if (setrlimit(RLIMIT_RTPRIO, &limit))
            _exit(fail("rt-soft-raise"));
        struct rlimit observed;
        if (getrlimit(RLIMIT_RTPRIO, &observed) || observed.rlim_cur != 10 ||
            observed.rlim_max != 10)
            _exit(fail("rt-limit-roundtrip"));
        limit.rlim_max = 11;
        errno = 0;
        if (setrlimit(RLIMIT_RTPRIO, &limit) != -1 || errno != EPERM)
            _exit(fail("rt-hard-raise"));
        param.sched_priority = 11;
        errno = 0;
        if (a20_linux_sched_setscheduler(0, SCHED_FIFO, &param) != -1 || errno != EPERM)
            _exit(fail("rt-over-limit"));
        param.sched_priority = 10;
        if (a20_linux_sched_setscheduler(0, SCHED_FIFO, &param))
            _exit(fail("rt-within-limit"));
        param.sched_priority = 0;
        if (a20_linux_sched_setscheduler(0, SCHED_OTHER, &param))
            _exit(fail("rt-limit-restore"));
        _exit(0);
    }
    int status;
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status))
        return fail("rt-permission-child");
    printf("SCHED_STRESS: rt permission/limits ok\n");
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
    unsigned char original[sizeof(mask)];
    memcpy(original, mask, sizeof(original));

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
    if (syscall(SYS_sched_setaffinity, 0, sizeof(original), original) < 0)
        return fail("restore-affinity");
    return 0;
}

static int wait_byte(int fd, char *value, int timeout_ms)
{
    struct pollfd pfd = {
        .fd = fd,
        .events = POLLIN,
    };
    int ready = poll(&pfd, 1, timeout_ms);
    if (ready != 1 || !(pfd.revents & POLLIN))
        return -1;
    return read(fd, value, 1) == 1 ? 0 : -1;
}

static int set_single_cpu_retry(pid_t pid, unsigned cpu)
{
    unsigned char mask[8] = {0};
    if (cpu >= sizeof(mask) * 8)
        return -1;
    mask[cpu / 8] = (unsigned char)(1U << (cpu % 8));
    for (int attempt = 0; attempt < 10000; attempt++) {
        if (syscall(SYS_sched_setaffinity, pid, sizeof(mask), mask) == 0)
            return 0;
        if (errno != EINVAL && errno != ESRCH)
            return -1;
        syscall(SYS_sched_yield);
    }
    return -1;
}

static int wait_child_ok(pid_t pid)
{
    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return -1;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}

static int smp_runqueue_preempt(void)
{
    unsigned char original[8] = {0};
    if (syscall(SYS_sched_getaffinity, 0, sizeof(original), original) <= 0)
        return fail("smp-getaffinity");

    unsigned parent_cpu = 0;
    if (syscall(SYS_getcpu, &parent_cpu, NULL, NULL) < 0 ||
        parent_cpu >= sizeof(original) * 8)
        return fail("smp-getcpu");

    unsigned remote_cpu = (unsigned)-1;
    for (unsigned cpu = 0; cpu < sizeof(original) * 8; cpu++) {
        if (cpu != parent_cpu &&
            (original[cpu / 8] & (1U << (cpu % 8)))) {
            remote_cpu = cpu;
            break;
        }
    }
    if (remote_cpu == (unsigned)-1) {
        printf("SCHED_STRESS: smp-runqueue SKIP cpus=1\n");
        return 0;
    }

    sched_diag_t before;
    if (read_sched_diag(&before) != 0 ||
        before.scheduler_violations != 0)
        return fail("smp-preexisting-diag");
    if (set_single_cpu_retry(0, parent_cpu) != 0)
        return fail("smp-pin-parent");

    int hog_gate[2] = {-1, -1};
    int hog_ready[2] = {-1, -1};
    int target_gate[2] = {-1, -1};
    int target_ready[2] = {-1, -1};
    int target_result[2] = {-1, -1};
    pid_t hog = -1;
    pid_t target = -1;
    int result = 1;
    unsigned observed_cpu = (unsigned)-1;
    const char *failure_stage = "pipe";
    int failure_errno = 0;

    if (pipe(hog_gate) < 0 || pipe(hog_ready) < 0 ||
        pipe(target_gate) < 0 || pipe(target_ready) < 0 ||
        pipe(target_result) < 0) {
        failure_errno = errno;
        goto cleanup;
    }

    failure_stage = "fork-hog";
    hog = fork();
    if (hog < 0) {
        failure_errno = errno;
        goto cleanup;
    }
    if (hog == 0) {
        char byte;
        close(hog_gate[1]);
        close(hog_ready[0]);
        if (read(hog_gate[0], &byte, 1) != 1)
            _exit(2);
        struct sched_param param = {.sched_priority = 99};
        if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &param) < 0)
            _exit(3);
        byte = 'H';
        if (write(hog_ready[1], &byte, 1) != 1)
            _exit(4);
        for (;;)
            __asm__ __volatile__("" ::: "memory");
    }
    close(hog_gate[0]);
    hog_gate[0] = -1;
    close(hog_ready[1]);
    hog_ready[1] = -1;
    failure_stage = "start-hog";
    if (set_single_cpu_retry(hog, remote_cpu) != 0 ||
        write(hog_gate[1], "G", 1) != 1) {
        failure_errno = errno;
        goto cleanup;
    }
    char byte = 0;
    failure_stage = "wait-hog-ready";
    if (wait_byte(hog_ready[0], &byte, 3000) != 0 || byte != 'H') {
        failure_errno = errno;
        goto cleanup;
    }

    failure_stage = "fork-target";
    target = fork();
    if (target < 0) {
        failure_errno = errno;
        goto cleanup;
    }
    if (target == 0) {
        char gate;
        close(target_gate[1]);
        close(target_ready[0]);
        close(target_result[0]);
        struct sched_param param = {.sched_priority = 50};
        if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &param) < 0)
            _exit(5);
        if (write(target_ready[1], "R", 1) != 1)
            _exit(6);
        if (read(target_gate[0], &gate, 1) != 1)
            _exit(7);
        unsigned cpu = 0;
        if (syscall(SYS_getcpu, &cpu, NULL, NULL) < 0 ||
            write(target_result[1], &cpu, sizeof(cpu)) !=
                (ssize_t)sizeof(cpu))
            _exit(8);
        _exit(0);
    }
    close(target_gate[0]);
    target_gate[0] = -1;
    close(target_ready[1]);
    target_ready[1] = -1;
    close(target_result[1]);
    target_result[1] = -1;
    failure_stage = "wait-target-ready";
    if (wait_byte(target_ready[0], &byte, 3000) != 0 || byte != 'R') {
        failure_errno = errno;
        goto cleanup;
    }
    failure_stage = "wake-target-remote";
    if (set_single_cpu_retry(target, remote_cpu) != 0 ||
        write(target_gate[1], "W", 1) != 1) {
        failure_errno = errno;
        goto cleanup;
    }

    usleep(20000);
    failure_stage = "migrate-target-local";
    if (set_single_cpu_retry(target, parent_cpu) != 0) {
        failure_errno = errno;
        goto cleanup;
    }

    struct pollfd result_pfd = {
        .fd = target_result[0],
        .events = POLLIN,
    };
    failure_stage = "wait-target-result";
    if (poll(&result_pfd, 1, 3000) != 1 ||
        !(result_pfd.revents & POLLIN)) {
        failure_errno = errno;
        goto cleanup;
    }
    failure_stage = "read-target-result";
    ssize_t result_bytes =
        read(target_result[0], &observed_cpu, sizeof(observed_cpu));
    if (result_bytes != (ssize_t)sizeof(observed_cpu)) {
        failure_errno = errno;
        goto cleanup;
    }
    failure_stage = "verify-target-cpu";
    if (observed_cpu != parent_cpu)
        goto cleanup;
    failure_stage = "reap-target";
    if (wait_child_ok(target) != 0) {
        failure_errno = errno;
        goto cleanup;
    }
    target = -1;

    failure_stage = "reap-hog";
    if (kill(hog, SIGKILL) < 0 || waitpid(hog, NULL, 0) != hog) {
        failure_errno = errno;
        goto cleanup;
    }
    hog = -1;

    sched_diag_t after;
    memset(&after, 0, sizeof(after));
    failure_stage = "read-diagnostics";
    for (int attempt = 0; attempt < 200; attempt++) {
        syscall(SYS_sched_yield);
        usleep(1000);
        if (read_sched_diag(&after) != 0) {
            failure_errno = errno;
            goto cleanup;
        }
        if (after.resched_ipi_acks > before.resched_ipi_acks)
            break;
    }
    failure_stage = "verify-diagnostics";
    if (after.scheduler_violations != 0 ||
        after.runqueue_migrations <= before.runqueue_migrations ||
        after.resched_requests <= before.resched_requests ||
        after.resched_priority_requests <=
            before.resched_priority_requests ||
        after.resched_ipi_sent <= before.resched_ipi_sent ||
        after.resched_ipi_acks <= before.resched_ipi_acks ||
        after.resched_consumed <= before.resched_consumed)
        goto cleanup;

    printf("SCHED_STRESS: smp-runqueue PASS migrations=%lu requests=%lu "
           "priority=%lu ipi=%lu/%lu consumed=%lu\n",
           after.runqueue_migrations - before.runqueue_migrations,
           after.resched_requests - before.resched_requests,
           after.resched_priority_requests -
               before.resched_priority_requests,
           after.resched_ipi_sent - before.resched_ipi_sent,
           after.resched_ipi_acks - before.resched_ipi_acks,
           after.resched_consumed - before.resched_consumed);
    result = 0;

cleanup:
    if (result != 0)
        printf("SCHED_STRESS: smp-runqueue DETAIL stage=%s errno=%d "
               "parent=%u remote=%u observed=%u\n",
               failure_stage, failure_errno, parent_cpu, remote_cpu,
               observed_cpu);
    if (target > 0) {
        kill(target, SIGKILL);
        waitpid(target, NULL, 0);
    }
    if (hog > 0) {
        kill(hog, SIGKILL);
        waitpid(hog, NULL, 0);
    }
    int *pipes[] = {
        hog_gate, hog_ready, target_gate, target_ready, target_result,
    };
    for (unsigned i = 0; i < sizeof(pipes) / sizeof(pipes[0]); i++) {
        for (int end = 0; end < 2; end++) {
            if (pipes[i][end] >= 0)
                close(pipes[i][end]);
        }
    }
    if (syscall(SYS_sched_setaffinity, 0, sizeof(original), original) < 0)
        return fail("smp-restore-parent");
    if (result != 0)
        return fail("smp-runqueue");
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

static int runqueue_lock_split(void)
{
    sched_diag_t before;
    sched_diag_t after;
    if (read_sched_diag(&before) != 0 ||
        before.scheduler_violations != 0)
        return fail("lock-split-preexisting-diag");

    for (int i = 0; i < 256; i++)
        syscall(SYS_sched_yield);

    if (read_sched_diag(&after) != 0)
        return fail("lock-split-read-diag");
    if (after.scheduler_violations != 0 ||
        after.runqueue_local_picks <= before.runqueue_local_picks ||
        after.runqueue_lock_acquires <= before.runqueue_lock_acquires ||
        after.runqueue_lock_contentions > after.runqueue_lock_acquires ||
        after.runqueue_parallel_pick_peak == 0)
        return fail("lock-split-diagnostics");

    printf("SCHED_STRESS: lock-split PASS picks=%lu empty=%lu "
           "locks=%lu contended=%lu parallel_peak=%lu\n",
           after.runqueue_local_picks - before.runqueue_local_picks,
           after.runqueue_empty_picks - before.runqueue_empty_picks,
           after.runqueue_lock_acquires - before.runqueue_lock_acquires,
           after.runqueue_lock_contentions -
               before.runqueue_lock_contentions,
           after.runqueue_parallel_pick_peak);
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
        int err = errno;
        /* ENOSYS is mount(2) itself missing -> cgroupfs is unproven, fail.
         * ENOENT: the mount point is not there in this run (a tmpfs fact, and
         * the same errno vfs_mount uses for an unresolvable target).
         * EINVAL: vfs_mount only answers -EINVAL for an unknown fstype, i.e.
         *   the cgroup branch was removed from vfs_mount() -- absent.
         * EPERM: privilege, environment.
         * Split rationale: the capability is "does cgroup cpuset affinity
         * constrain a task", and nothing short of a working mount can prove it,
         * so every one of these errnos costs the gate real coverage.  EINVAL and
         * ENOSYS are a missing implementation and go red; ENOENT/EPERM describe
         * this invocation and are counted as reported skips instead. */
        if (err == ENOSYS || err == EINVAL)
            return absent("cgroup-mount", err);
        if (err == ENOENT || err == EPERM)
            return skip_env("cgroup-mount", err);
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

typedef struct rr_slice_shared {
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned ready;
    unsigned paused;
    int go;
    int resume;
    uint64_t start_ns[2];
    uint64_t end_ns[2];
    unsigned long work[2];
} rr_slice_shared_t;

typedef struct rr_slice_arg {
    rr_slice_shared_t *shared;
    unsigned index;
} rr_slice_arg_t;

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return 0;
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void *rr_slice_worker(void *opaque)
{
    rr_slice_arg_t *arg = opaque;
    rr_slice_shared_t *shared = arg->shared;
    unsigned index = arg->index;
    pthread_mutex_lock(&shared->lock);
    shared->ready++;
    pthread_cond_broadcast(&shared->cond);
    while (!shared->go)
        pthread_cond_wait(&shared->cond, &shared->lock);
    pthread_mutex_unlock(&shared->lock);

    /* Switch policy from the running task itself. This exercises the policy
     * transition timestamp/quantum initialization instead of setting RR only
     * while the task is blocked on the start barrier. */
    struct sched_param param = {.sched_priority = 40};
    int rc = pthread_setschedparam(pthread_self(), SCHED_RR, &param);
    if (rc != 0) {
        errno = rc;
        return (void *)(uintptr_t)1;
    }

    /* Consume part of the first quantum, then block. The blocked interval must
     * not consume the remaining slice; after wake both peers still make work. */
    uint64_t partial_start = monotonic_ns();
    uint64_t partial_now = partial_start;
    unsigned long partial_work = 0;
    while (partial_now - partial_start < 30000000ULL) {
        partial_work++;
        if ((partial_work & 0x3fffUL) == 0)
            partial_now = monotonic_ns();
    }
    pthread_mutex_lock(&shared->lock);
    shared->paused++;
    pthread_cond_broadcast(&shared->cond);
    while (!shared->resume)
        pthread_cond_wait(&shared->cond, &shared->lock);
    pthread_mutex_unlock(&shared->lock);

    uint64_t start = monotonic_ns();
    shared->start_ns[index] = start;
    uint64_t now = start;
    unsigned long work = 0;
    while (now - start < 700000000ULL) {
        work++;
        if ((work & 0x3fffUL) == 0)
            now = monotonic_ns();
    }
    shared->work[index] = work;
    shared->end_ns[index] = monotonic_ns();
    return NULL;
}

/* Two equal-priority CPU-bound RR tasks pinned to one CPU must overlap in
 * wall-clock execution. The previous pick-time rotation had no tick-driven
 * reschedule and repeatedly selected the same peer; this test is designed to
 * fail that behavior without weakening the work or affinity requirements. */
static int rt_rr_quantum_progress(void)
{
    unsigned char original[8] = {0};
    unsigned cpu = 0;
    if (syscall(SYS_sched_getaffinity, 0, sizeof(original), original) <= 0 ||
        syscall(SYS_getcpu, &cpu, NULL, NULL) < 0 ||
        set_single_cpu_retry(0, cpu) != 0)
        return fail("rr-affinity");

    rr_slice_shared_t shared = {0};
    rr_slice_arg_t args[2] = {{&shared, 0}, {&shared, 1}};
    pthread_t threads[2];
    int created = 0;
    if (pthread_mutex_init(&shared.lock, NULL) != 0 ||
        pthread_cond_init(&shared.cond, NULL) != 0)
        goto failed;
    for (; created < 2; created++) {
        int rc = pthread_create(&threads[created], NULL, rr_slice_worker,
                                &args[created]);
        if (rc != 0) {
            errno = rc;
            goto release;
        }
    }
    pthread_mutex_lock(&shared.lock);
    while (shared.ready != 2)
        pthread_cond_wait(&shared.cond, &shared.lock);
    pthread_mutex_unlock(&shared.lock);

    pthread_mutex_lock(&shared.lock);
    shared.go = 1;
    pthread_cond_broadcast(&shared.cond);
    while (shared.paused != 2)
        pthread_cond_wait(&shared.cond, &shared.lock);
    pthread_mutex_unlock(&shared.lock);
    /* Deliberately sleep while both RR tasks are blocked with partial slices. */
    usleep(200000);
    pthread_mutex_lock(&shared.lock);
    shared.resume = 1;
    pthread_cond_broadcast(&shared.cond);
    pthread_mutex_unlock(&shared.lock);
    for (unsigned i = 0; i < 2; i++) {
        if (pthread_join(threads[i], NULL) != 0)
            goto failed_destroy;
    }
    {
        uint64_t overlap_start = shared.start_ns[0] > shared.start_ns[1]
                               ? shared.start_ns[0] : shared.start_ns[1];
        uint64_t overlap_end = shared.end_ns[0] < shared.end_ns[1]
                             ? shared.end_ns[0] : shared.end_ns[1];
        uint64_t overlap = overlap_end > overlap_start
                         ? overlap_end - overlap_start : 0;
        pthread_cond_destroy(&shared.cond);
        pthread_mutex_destroy(&shared.lock);
        if (syscall(SYS_sched_setaffinity, 0, sizeof(original), original) < 0)
            return fail("rr-restore-affinity");
        if (!shared.work[0] || !shared.work[1] || overlap < 100000000ULL) {
            printf("SCHED_STRESS: FAIL rr-quantum work=%lu,%lu overlap_ns=%llu\n",
                   shared.work[0], shared.work[1],
                   (unsigned long long)overlap);
            return 1;
        }
        printf("SCHED_STRESS: rt-rr-quantum PASS overlap_ns=%llu work=%lu,%lu blocked_partial_slice=1 self_policy_switch=1\n",
               (unsigned long long)overlap, shared.work[0], shared.work[1]);
        return 0;
    }

release:
    pthread_mutex_lock(&shared.lock);
    shared.go = 1;
    pthread_cond_broadcast(&shared.cond);
    pthread_mutex_unlock(&shared.lock);
    for (int i = 0; i < created; i++)
        (void)pthread_join(threads[i], NULL);
failed_destroy:
    pthread_cond_destroy(&shared.cond);
    pthread_mutex_destroy(&shared.lock);
failed:
    (void)syscall(SYS_sched_setaffinity, 0, sizeof(original), original);
    return fail("rr-setup");
}

static int rt_bandwidth_progress(void)
{
    unsigned char original[8] = {0};
    unsigned cpu = 0;
    if (syscall(SYS_sched_getaffinity, 0, sizeof(original), original) <= 0 ||
        syscall(SYS_getcpu, &cpu, NULL, NULL) < 0 ||
        set_single_cpu_retry(0, cpu) != 0)
        return fail("rt-budget-affinity");
    unsigned long *progress = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                                   MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (progress == MAP_FAILED)
        return fail("rt-budget-map");
    int ready[2];
    if (pipe(ready) < 0)
        return fail("rt-budget-pipe");
    pid_t child = fork();
    if (child < 0)
        return fail("rt-budget-fork");
    if (!child) {
        close(ready[0]);
        struct sched_param param = {.sched_priority = 99};
        if (syscall(SYS_sched_setscheduler, 0, SCHED_FIFO, &param) < 0 ||
            write(ready[1], "R", 1) != 1)
            _exit(2);
        for (;;)
            __atomic_fetch_add(progress, 1, __ATOMIC_RELAXED);
    }
    close(ready[1]);
    char byte = 0;
    int result = wait_byte(ready[0], &byte, 3000);
    close(ready[0]);
    unsigned long last = __atomic_load_n(progress, __ATOMIC_RELAXED);
    for (int i = 0; result == 0 && i < 2; i++) {
        usleep(550000);
        usleep(550000);
        unsigned long next = __atomic_load_n(progress, __ATOMIC_RELAXED);
        if (next == last)
            result = -1;
        last = next;
    }
    /* Reaching here proves normal work on the SAME CPU progresses beside a
     * non-yielding FIFO task; increasing counters prove budget replenishment. */
    if (kill(child, SIGKILL) < 0 || waitpid(child, NULL, 0) != child)
        result = -1;
    munmap(progress, 4096);
    if (syscall(SYS_sched_setaffinity, 0, sizeof(original), original) < 0)
        result = -1;
    if (result || byte != 'R' || !last)
        return fail("rt-budget-progress");
    printf("SCHED_STRESS: rt-bandwidth-progress PASS\n");
    return 0;
}

int main(void)
{
    printf("SCHED_STRESS: start\n");
    if (priority_bounds() != 0)
        return 1;
    if (policy_param_roundtrip() != 0)
        return 1;
    if (rt_permission_limits() != 0)
        return 1;
    if (rt_rr_quantum_progress() != 0)
        return 1;
    if (affinity_roundtrip() != 0)
        return 1;
    if (rt_bandwidth_progress() != 0)
        return 1;
    if (smp_runqueue_preempt() != 0)
        return 1;
    if (runqueue_lock_split() != 0)
        return 1;
    if (nice_and_attr() != 0)
        return 1;
    if (cgroup_cpuset_affinity() != 0)
        return 1;
    printf("SCHED_STRESS: coverage absent=%d skipped=%d\n", g_absent, g_skipped);
    if (g_absent) {
        printf("SCHED_STRESS: FAIL absent-capability count=%d "
               "(capability not implemented; no PASS marker emitted)\n",
               g_absent);
        return 1;
    }
    printf("SCHED_STRESS: PASS\n");
    return 0;
}
