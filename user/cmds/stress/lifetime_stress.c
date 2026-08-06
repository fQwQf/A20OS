#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_futex
#define SYS_futex 98
#endif

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif

#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

#define RACE_ROUNDS 8
#define RACE_WORKERS 8
#define TIMEOUT_CAPACITY_TEST_MAX 128
#define TIMEOUT_CAPACITY_SLEEP_SEC 30

typedef struct lifetime_stats {
    unsigned long task_objects;
    unsigned long task_refs;
    unsigned long listed_tasks;
    unsigned long listed_refs;
    unsigned long pid_entries;
    unsigned long wait_entries;
    unsigned long wake_entries;
    unsigned long timeout_entries;
    unsigned long timeout_capacity;
    unsigned long timeout_full_failures;
    unsigned long timeout_duplicate_rejections;
    unsigned long timeout_stale_expirations;
    unsigned long timeout_heap_violations;
    unsigned long lifetime_errors;
    unsigned seen;
} lifetime_stats_t;

enum {
    SEEN_TASK_OBJECTS = 1U << 0,
    SEEN_TASK_REFS = 1U << 1,
    SEEN_LISTED_TASKS = 1U << 2,
    SEEN_LISTED_REFS = 1U << 3,
    SEEN_PID_ENTRIES = 1U << 4,
    SEEN_WAIT_ENTRIES = 1U << 5,
    SEEN_WAKE_ENTRIES = 1U << 6,
    SEEN_TIMEOUT_ENTRIES = 1U << 7,
    SEEN_TIMEOUT_CAPACITY = 1U << 8,
    SEEN_TIMEOUT_FULL_FAILURES = 1U << 9,
    SEEN_TIMEOUT_DUPLICATE_REJECTIONS = 1U << 10,
    SEEN_TIMEOUT_STALE_EXPIRATIONS = 1U << 11,
    SEEN_TIMEOUT_HEAP_VIOLATIONS = 1U << 12,
    SEEN_LIFETIME_ERRORS = 1U << 13,
    SEEN_ALL = (1U << 14) - 1,
};

static int fail(const char *what)
{
    printf("LIFETIME_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static void settle(void)
{
    for (int i = 0; i < 32; i++)
        syscall(SYS_sched_yield);
    usleep(20000);
}

static int read_stats(lifetime_stats_t *stats)
{
    memset(stats, 0, sizeof(*stats));
    FILE *fp = fopen("/proc/a20/task_lifetime", "r");
    if (!fp)
        return fail("open-task-lifetime");

    char line[128];
    while (fgets(line, sizeof(line), fp)) {
        char key[64];
        unsigned long value;
        if (sscanf(line, "%63[^:]: %lu", key, &value) != 2)
            continue;
        if (strcmp(key, "task_objects") == 0) {
            stats->task_objects = value;
            stats->seen |= SEEN_TASK_OBJECTS;
        } else if (strcmp(key, "task_refs") == 0) {
            stats->task_refs = value;
            stats->seen |= SEEN_TASK_REFS;
        } else if (strcmp(key, "listed_tasks") == 0) {
            stats->listed_tasks = value;
            stats->seen |= SEEN_LISTED_TASKS;
        } else if (strcmp(key, "listed_refs") == 0) {
            stats->listed_refs = value;
            stats->seen |= SEEN_LISTED_REFS;
        } else if (strcmp(key, "pid_entries") == 0) {
            stats->pid_entries = value;
            stats->seen |= SEEN_PID_ENTRIES;
        } else if (strcmp(key, "wait_entries") == 0) {
            stats->wait_entries = value;
            stats->seen |= SEEN_WAIT_ENTRIES;
        } else if (strcmp(key, "wake_entries") == 0) {
            stats->wake_entries = value;
            stats->seen |= SEEN_WAKE_ENTRIES;
        } else if (strcmp(key, "timeout_entries") == 0) {
            stats->timeout_entries = value;
            stats->seen |= SEEN_TIMEOUT_ENTRIES;
        } else if (strcmp(key, "timeout_capacity") == 0) {
            stats->timeout_capacity = value;
            stats->seen |= SEEN_TIMEOUT_CAPACITY;
        } else if (strcmp(key, "timeout_full_failures") == 0) {
            stats->timeout_full_failures = value;
            stats->seen |= SEEN_TIMEOUT_FULL_FAILURES;
        } else if (strcmp(key, "timeout_duplicate_rejections") == 0) {
            stats->timeout_duplicate_rejections = value;
            stats->seen |= SEEN_TIMEOUT_DUPLICATE_REJECTIONS;
        } else if (strcmp(key, "timeout_stale_expirations") == 0) {
            stats->timeout_stale_expirations = value;
            stats->seen |= SEEN_TIMEOUT_STALE_EXPIRATIONS;
        } else if (strcmp(key, "timeout_heap_violations") == 0) {
            stats->timeout_heap_violations = value;
            stats->seen |= SEEN_TIMEOUT_HEAP_VIOLATIONS;
        } else if (strcmp(key, "lifetime_errors") == 0) {
            stats->lifetime_errors = value;
            stats->seen |= SEEN_LIFETIME_ERRORS;
        }
    }
    fclose(fp);
    if (stats->seen != SEEN_ALL)
        return fail("parse-task-lifetime");
    if (stats->lifetime_errors != 0)
        return fail("preexisting-lifetime-errors");
    return 0;
}

static int compare_stats(const lifetime_stats_t *before,
                         const lifetime_stats_t *after)
{
#define CHECK_FIELD(field)                                                     \
    do {                                                                       \
        if (before->field != after->field) {                                   \
            printf("LIFETIME_STRESS: FAIL baseline " #field                   \
                   " before=%lu after=%lu\n", before->field, after->field);    \
            return 1;                                                          \
        }                                                                      \
    } while (0)
    CHECK_FIELD(task_objects);
    CHECK_FIELD(task_refs);
    CHECK_FIELD(listed_tasks);
    CHECK_FIELD(pid_entries);
    CHECK_FIELD(wait_entries);
    CHECK_FIELD(wake_entries);
    CHECK_FIELD(timeout_entries);
    CHECK_FIELD(timeout_capacity);
    CHECK_FIELD(timeout_duplicate_rejections);
    CHECK_FIELD(timeout_stale_expirations);
    CHECK_FIELD(timeout_heap_violations);
#undef CHECK_FIELD
    if (after->lifetime_errors != 0)
        return fail("post-stress-lifetime-errors");
    return 0;
}

static int ownership_stats_equal(const lifetime_stats_t *a,
                                 const lifetime_stats_t *b)
{
#define SAME_FIELD(field) do { if (a->field != b->field) return 0; } while (0)
    SAME_FIELD(task_objects);
    SAME_FIELD(task_refs);
    SAME_FIELD(listed_tasks);
    SAME_FIELD(pid_entries);
    SAME_FIELD(wait_entries);
    SAME_FIELD(wake_entries);
    SAME_FIELD(timeout_entries);
    SAME_FIELD(timeout_capacity);
    SAME_FIELD(timeout_duplicate_rejections);
    SAME_FIELD(timeout_stale_expirations);
    SAME_FIELD(timeout_heap_violations);
#undef SAME_FIELD
    return 1;
}

/*
 * SMP scheduler references move between listed user tasks and unlisted idle
 * tasks as runqueue ownership is transferred to dispatch/current CPU slots.
 * Therefore listed_refs is a useful point-in-time diagnostic, but not a stable
 * cross-time baseline.  task_refs is the authoritative global ownership count;
 * listed_tasks and pid_entries prove that all published children were reaped.
 * Require two consecutive equal samples for those stable fields, then wait for
 * cleanup to return to that baseline.  Monotonic error counters are still
 * rejected by read_stats immediately.
 */
static int read_stable_stats(lifetime_stats_t *stats)
{
    lifetime_stats_t previous;
    if (read_stats(&previous) != 0)
        return 1;
    for (int attempt = 0; attempt < 64; attempt++) {
        settle();
        lifetime_stats_t current;
        if (read_stats(&current) != 0)
            return 1;
        if (ownership_stats_equal(&previous, &current)) {
            *stats = current;
            return 0;
        }
        previous = current;
    }
    printf("LIFETIME_STRESS: FAIL unstable initial baseline\n");
    return 1;
}

static int wait_for_baseline(const lifetime_stats_t *baseline,
                             lifetime_stats_t *last)
{
    for (int attempt = 0; attempt < 64; attempt++) {
        settle();
        if (read_stats(last) != 0)
            return 1;
        if (ownership_stats_equal(baseline, last))
            return 0;
    }
    return compare_stats(baseline, last);
}

static int wait_any_exit(pid_t pid, const char *what)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail(what);
    if (!WIFEXITED(status) && !WIFSIGNALED(status))
        return fail(what);
    return 0;
}

static int run_existing_stress(const char *name)
{
    pid_t pid = fork();
    if (pid < 0)
        return fail("fork-existing-stress");
    if (pid == 0) {
        char path[96];
        snprintf(path, sizeof(path), "/bin/%s", name);
        char *argv[] = {(char *)name, NULL};
        char *envp[] = {"PATH=/bin", NULL};
        execve(path, argv, envp);
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("wait-existing-stress");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("LIFETIME_STRESS: FAIL child=%s status=%d\n", name, status);
        return 1;
    }
    return 0;
}

static int race_fork_exit_wait(void)
{
    for (int round = 0; round < RACE_ROUNDS; round++) {
        pid_t pids[RACE_WORKERS];
        for (int i = 0; i < RACE_WORKERS; i++) {
            pids[i] = fork();
            if (pids[i] < 0)
                return fail("fork-exit-wait");
            if (pids[i] == 0) {
                for (int n = 0; n < (i & 3); n++)
                    syscall(SYS_sched_yield);
                _exit((round + i) & 31);
            }
        }
        for (int i = RACE_WORKERS - 1; i >= 0; i--) {
            int status = 0;
            if (waitpid(pids[i], &status, 0) < 0 ||
                !WIFEXITED(status) ||
                WEXITSTATUS(status) != ((round + i) & 31))
                return fail("wait4-exit-race");
        }
    }
    return 0;
}

static int race_signal_exit(void)
{
    for (int round = 0; round < RACE_ROUNDS; round++) {
        pid_t pids[RACE_WORKERS];
        for (int i = 0; i < RACE_WORKERS; i++) {
            pids[i] = fork();
            if (pids[i] < 0)
                return fail("fork-signal-exit");
            if (pids[i] == 0) {
                usleep((unsigned)(500 + i * 250));
                _exit(0);
            }
        }
        syscall(SYS_sched_yield);
        for (int i = 0; i < RACE_WORKERS; i++) {
            if (kill(pids[i], SIGTERM) < 0 && errno != ESRCH)
                return fail("kill-signal-exit");
        }
        for (int i = 0; i < RACE_WORKERS; i++) {
            if (wait_any_exit(pids[i], "wait-signal-exit") != 0)
                return 1;
        }
    }
    return 0;
}

static int race_timeout_exit(void)
{
    for (int round = 0; round < RACE_ROUNDS; round++) {
        pid_t pids[RACE_WORKERS];
        for (int i = 0; i < RACE_WORKERS; i++) {
            pids[i] = fork();
            if (pids[i] < 0)
                return fail("fork-timeout-exit");
            if (pids[i] == 0) {
                struct timespec ts = {0, 1000000 + i * 250000};
                (void)nanosleep(&ts, NULL);
                _exit(0);
            }
        }
        usleep(1000);
        for (int i = 1; i < RACE_WORKERS; i += 2) {
            if (kill(pids[i], SIGTERM) < 0 && errno != ESRCH)
                return fail("kill-timeout-exit");
        }
        for (int i = 0; i < RACE_WORKERS; i++) {
            if (wait_any_exit(pids[i], "wait-timeout-exit") != 0)
                return 1;
        }
    }
    return 0;
}

static int race_futex_exit(void)
{
    int *words = mmap(NULL, RACE_WORKERS * sizeof(*words),
                      PROT_READ | PROT_WRITE,
                      MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (words == MAP_FAILED)
        return fail("mmap-futex-exit");

    for (int round = 0; round < RACE_ROUNDS; round++) {
        memset(words, 0, RACE_WORKERS * sizeof(*words));
        pid_t pids[RACE_WORKERS];
        for (int i = 0; i < RACE_WORKERS; i++) {
            pids[i] = fork();
            if (pids[i] < 0)
                return fail("fork-futex-exit");
            if (pids[i] == 0) {
                struct timespec timeout = {0, 3000000 + i * 100000};
                (void)syscall(SYS_futex, &words[i], FUTEX_WAIT, 0,
                              &timeout, NULL, 0);
                _exit(0);
            }
        }
        usleep(1000);
        for (int i = 0; i < RACE_WORKERS; i++) {
            if ((i & 1) == 0) {
                __atomic_store_n(&words[i], 1, __ATOMIC_SEQ_CST);
                if (syscall(SYS_futex, &words[i], FUTEX_WAKE, 1,
                            NULL, NULL, 0) < 0)
                    return fail("wake-futex-exit");
            } else if (kill(pids[i], SIGTERM) < 0 && errno != ESRCH) {
                return fail("kill-futex-exit");
            }
        }
        for (int i = 0; i < RACE_WORKERS; i++) {
            if (wait_any_exit(pids[i], "wait-futex-exit") != 0)
                return 1;
        }
    }

    munmap(words, RACE_WORKERS * sizeof(*words));
    return 0;
}

static int wait_timeout_entries(unsigned long expected)
{
    for (int attempt = 0; attempt < 8192; attempt++) {
        lifetime_stats_t stats;
        if (read_stats(&stats) != 0)
            return 1;
        if (stats.timeout_entries == expected)
            return 0;
        syscall(SYS_sched_yield);
    }
    printf("LIFETIME_STRESS: FAIL timeout-entries expected=%lu\n",
           expected);
    return 1;
}

static int read_exact(int fd, void *buf, size_t len)
{
    size_t done = 0;
    while (done < len) {
        ssize_t n = read(fd, (char *)buf + done, len - done);
        if (n <= 0)
            return -1;
        done += (size_t)n;
    }
    return 0;
}

static void stop_timeout_sleepers(pid_t *pids, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (pids[i] > 0)
            (void)kill(pids[i], SIGKILL);
    }
    for (size_t i = 0; i < count; i++) {
        if (pids[i] > 0)
            (void)wait_any_exit(pids[i], "wait-timeout-capacity-child");
    }
}

static int fill_timeout_heap(unsigned long target_entries,
                             unsigned long baseline_entries,
                             pid_t **pids_out, size_t *count_out)
{
    if (target_entries < baseline_entries)
        return fail("timeout-capacity-target");

    size_t count = (size_t)(target_entries - baseline_entries);
    pid_t *pids = calloc(count ? count : 1, sizeof(*pids));
    int ready[2];
    if (!pids || pipe(ready) < 0) {
        free(pids);
        return fail("timeout-capacity-setup");
    }

    size_t started = 0;
    for (; started < count; started++) {
        pids[started] = fork();
        if (pids[started] < 0)
            break;
        if (pids[started] == 0) {
            close(ready[0]);
            char byte = 'R';
            if (write(ready[1], &byte, 1) != 1)
                _exit(20);
            struct timespec sleep_for = {
                .tv_sec = TIMEOUT_CAPACITY_SLEEP_SEC,
                .tv_nsec = 0,
            };
            (void)nanosleep(&sleep_for, NULL);
            _exit(21);
        }
    }
    close(ready[1]);
    if (started != count) {
        close(ready[0]);
        stop_timeout_sleepers(pids, started);
        free(pids);
        return fail("fork-timeout-capacity");
    }

    char byte;
    for (size_t i = 0; i < count; i++) {
        if (read_exact(ready[0], &byte, 1) < 0) {
            close(ready[0]);
            stop_timeout_sleepers(pids, count);
            free(pids);
            return fail("timeout-capacity-ready");
        }
    }
    close(ready[0]);
    if (wait_timeout_entries(target_entries) != 0) {
        stop_timeout_sleepers(pids, count);
        free(pids);
        return 1;
    }

    *pids_out = pids;
    *count_out = count;
    return 0;
}

static int timeout_capacity_boundaries(const lifetime_stats_t *baseline)
{
    unsigned long capacity = baseline->timeout_capacity;
    unsigned long base = baseline->timeout_entries;
    if (capacity > TIMEOUT_CAPACITY_TEST_MAX) {
        printf("LIFETIME_STRESS: timeout-capacity SKIP capacity=%lu\n",
               capacity);
        return 0;
    }
    if (capacity < 2 || base >= capacity - 1)
        return fail("timeout-capacity-baseline");

    const unsigned long targets[] = {capacity - 1, capacity};
    const char *labels[] = {"capacity-1", "capacity"};
    for (size_t i = 0; i < 2; i++) {
        pid_t *pids = NULL;
        size_t count = 0;
        if (fill_timeout_heap(targets[i], base, &pids, &count) != 0)
            return 1;
        printf("LIFETIME_STRESS: timeout-%s PASS entries=%lu\n",
               labels[i], targets[i]);
        stop_timeout_sleepers(pids, count);
        free(pids);
        if (wait_timeout_entries(base) != 0)
            return 1;
    }

    pid_t *pids = NULL;
    size_t count = 0;
    if (fill_timeout_heap(capacity, base, &pids, &count) != 0)
        return 1;

    int result_pipe[2];
    if (pipe(result_pipe) < 0) {
        stop_timeout_sleepers(pids, count);
        free(pids);
        return fail("timeout-capacity-result-pipe");
    }
    pid_t overflow = fork();
    if (overflow < 0) {
        close(result_pipe[0]);
        close(result_pipe[1]);
        stop_timeout_sleepers(pids, count);
        free(pids);
        return fail("fork-timeout-capacity-overflow");
    }
    if (overflow == 0) {
        close(result_pipe[0]);
        struct timespec sleep_for = {
            .tv_sec = TIMEOUT_CAPACITY_SLEEP_SEC,
            .tv_nsec = 0,
        };
        errno = 0;
        int rc = nanosleep(&sleep_for, NULL);
        int result = rc < 0 ? errno : 0;
        (void)write(result_pipe[1], &result, sizeof(result));
        _exit(0);
    }
    close(result_pipe[1]);
    int result = 0;
    int read_result = read_exact(result_pipe[0], &result, sizeof(result));
    close(result_pipe[0]);
    if (wait_any_exit(overflow, "wait-timeout-capacity-overflow") != 0)
        read_result = -1;

    lifetime_stats_t full;
    int stats_result = read_stats(&full);
    if (read_result < 0 || stats_result != 0 || result != EAGAIN ||
        full.timeout_entries != capacity ||
        full.timeout_full_failures !=
            baseline->timeout_full_failures + 1) {
        printf("LIFETIME_STRESS: FAIL timeout-capacity+1 errno=%d "
               "entries=%lu failures=%lu expected_failures=%lu\n",
               result, full.timeout_entries, full.timeout_full_failures,
               baseline->timeout_full_failures + 1);
        stop_timeout_sleepers(pids, count);
        free(pids);
        return 1;
    }
    printf("LIFETIME_STRESS: timeout-capacity+1 PASS entries=%lu "
           "errno=%d\n", capacity, result);
    stop_timeout_sleepers(pids, count);
    free(pids);
    if (wait_timeout_entries(base) != 0)
        return 1;

    printf("LIFETIME_STRESS: timeout-capacity PASS capacity=%lu\n",
           capacity);
    return 0;
}

int main(void)
{
    static const char *existing[] = {
        "sched_stress",
        "futex_stress",
        "proc_stress",
        "io_event_test",
        "vfs_stress",
        "socket_stress",
    };

    printf("LIFETIME_STRESS: start\n");
    settle();
    lifetime_stats_t before;
    if (read_stable_stats(&before) != 0)
        return 1;

    for (size_t i = 0; i < sizeof(existing) / sizeof(existing[0]); i++) {
        printf("LIFETIME_STRESS: run %s\n", existing[i]);
        if (run_existing_stress(existing[i]) != 0)
            return 1;
    }
    if (race_fork_exit_wait() != 0)
        return 1;
    if (race_signal_exit() != 0)
        return 1;
    if (race_timeout_exit() != 0)
        return 1;
    if (race_futex_exit() != 0)
        return 1;
    if (timeout_capacity_boundaries(&before) != 0)
        return 1;

    lifetime_stats_t after;
    if (wait_for_baseline(&before, &after) != 0)
        return 1;

    printf("LIFETIME_STRESS: PASS\n");
    return 0;
}
