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

typedef struct lifetime_stats {
    unsigned long task_objects;
    unsigned long task_refs;
    unsigned long listed_tasks;
    unsigned long listed_refs;
    unsigned long pid_entries;
    unsigned long wait_entries;
    unsigned long wake_entries;
    unsigned long timeout_entries;
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
    SEEN_LIFETIME_ERRORS = 1U << 8,
    SEEN_ALL = (1U << 9) - 1,
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
    CHECK_FIELD(listed_refs);
    CHECK_FIELD(pid_entries);
    CHECK_FIELD(wait_entries);
    CHECK_FIELD(wake_entries);
    CHECK_FIELD(timeout_entries);
#undef CHECK_FIELD
    if (after->lifetime_errors != 0)
        return fail("post-stress-lifetime-errors");
    return 0;
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
    if (read_stats(&before) != 0)
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

    settle();
    lifetime_stats_t after;
    if (read_stats(&after) != 0)
        return 1;
    if (compare_stats(&before, &after) != 0)
        return 1;

    printf("LIFETIME_STRESS: PASS\n");
    return 0;
}
