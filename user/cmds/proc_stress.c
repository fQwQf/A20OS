#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#ifndef SYS_futex
#define SYS_futex 98
#endif

#ifndef FUTEX_WAIT
#define FUTEX_WAIT 0
#endif

#ifndef FUTEX_WAKE
#define FUTEX_WAKE 1
#endif

static int fail(const char *what)
{
    printf("PROC_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static int wait_exit(pid_t pid, int code, const char *what)
{
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail(what);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != code)
        return fail(what);
    return 0;
}

static int scenario_fork_wait_yield(void)
{
    for (int i = 0; i < 8; i++) {
        pid_t pid = fork();
        if (pid < 0)
            return fail("fork-yield");
        if (pid == 0) {
            for (int j = 0; j < 16; j++)
                syscall(SYS_sched_yield);
            _exit(10 + i);
        }
        if (wait_exit(pid, 10 + i, "wait-yield") != 0)
            return 1;
    }
    return 0;
}

static int scenario_exec_wait(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return fail("fork-exec");
    if (pid == 0) {
        char *argv[] = {"true", NULL};
        char *envp[] = {"PATH=/bin", NULL};
        execve("/bin/true", argv, envp);
        _exit(127);
    }
    return wait_exit(pid, 0, "wait-exec");
}

static int verify_large_exec_args(int argc, char **argv)
{
    if (argc != 98)
        return 1;
    for (int i = 0; i < 96; i++) {
        char expected[96];
        int prefix = snprintf(expected, sizeof(expected), "arg-%03d-", i);
        memset(expected + prefix, 'A' + (i % 26),
               sizeof(expected) - (size_t)prefix - 1);
        expected[sizeof(expected) - 1] = '\0';
        if (strcmp(argv[i + 2], expected) != 0)
            return 1;
    }
    return 0;
}

static int scenario_exec_large_args(void)
{
    static char arg_storage[96][96];
    char *argv[99];
    argv[0] = "proc_stress";
    argv[1] = "--verify-exec-args";
    for (int i = 0; i < 96; i++) {
        int prefix = snprintf(arg_storage[i], sizeof(arg_storage[i]),
                              "arg-%03d-", i);
        memset(arg_storage[i] + prefix, 'A' + (i % 26),
               sizeof(arg_storage[i]) - (size_t)prefix - 1);
        arg_storage[i][sizeof(arg_storage[i]) - 1] = '\0';
        argv[i + 2] = arg_storage[i];
    }
    argv[98] = NULL;

    pid_t pid = fork();
    if (pid < 0)
        return fail("fork-exec-large-args");
    if (pid == 0) {
        char *envp[] = {"PATH=/bin", NULL};
        execve("/bin/proc_stress", argv, envp);
        _exit(127);
    }
    return wait_exit(pid, 0, "wait-exec-large-args");
}

static int scenario_spawn_missing(void)
{
    pid_t pid = -1;
    char *argv[] = {"a20os-definitely-missing-executable", NULL};
    int ret = posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ);

    if (ret == ENOENT)
        return 0;
    if (ret == 0) {
        int status = 0;
        (void)waitpid(pid, &status, 0);
        errno = 0;
    } else {
        errno = ret;
    }
    return fail("spawn-missing-enoent");
}

static int scenario_sleep_signal(void)
{
    pid_t pid = fork();
    if (pid < 0)
        return fail("fork-signal");
    if (pid == 0) {
        for (;;)
            pause();
    }

    struct timespec ts = {0, 10000000};
    nanosleep(&ts, NULL);
    if (kill(pid, SIGTERM) < 0)
        return fail("kill");
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("wait-signal");
    if (!WIFSIGNALED(status) && !(WIFEXITED(status) && WEXITSTATUS(status) != 0))
        return fail("signal-status");
    return 0;
}

static int scenario_pipe_wake(void)
{
    int fds[2];
    if (pipe(fds) < 0)
        return fail("pipe");
    pid_t pid = fork();
    if (pid < 0)
        return fail("fork-pipe");
    if (pid == 0) {
        close(fds[1]);
        char c = 0;
        int n = read(fds[0], &c, 1);
        close(fds[0]);
        _exit(n == 1 && c == 'x' ? 0 : 1);
    }
    close(fds[0]);
    struct timespec ts = {0, 10000000};
    nanosleep(&ts, NULL);
    if (write(fds[1], "x", 1) != 1)
        return fail("pipe-write");
    close(fds[1]);
    return wait_exit(pid, 0, "wait-pipe");
}

static int scenario_futex_wake(void)
{
    int *word = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (word == MAP_FAILED)
        return fail("futex-mmap");
    *word = 0;

    pid_t pid = fork();
    if (pid < 0)
        return fail("fork-futex");
    if (pid == 0) {
        while (__atomic_load_n(word, __ATOMIC_SEQ_CST) == 0) {
            long r = syscall(SYS_futex, word, FUTEX_WAIT, 0, NULL, NULL, 0);
            if (r < 0 && errno != EAGAIN && errno != EINTR)
                _exit(2);
        }
        _exit(0);
    }

    struct timespec ts = {0, 10000000};
    nanosleep(&ts, NULL);
    __atomic_store_n(word, 1, __ATOMIC_SEQ_CST);
    if (syscall(SYS_futex, word, FUTEX_WAKE, 1, NULL, NULL, 0) < 0)
        return fail("futex-wake");
    int r = wait_exit(pid, 0, "wait-futex");
    munmap(word, 4096);
    return r;
}

static int scenario_vfork_auto_reap(void)
{
    struct sigaction old_action;
    struct sigaction ignore_action;
    memset(&ignore_action, 0, sizeof(ignore_action));
    ignore_action.sa_handler = SIG_IGN;
    sigemptyset(&ignore_action.sa_mask);
    if (sigaction(SIGCHLD, &ignore_action, &old_action) < 0)
        return fail("vfork-ignore-sigchld");

    for (int i = 0; i < 128; i++) {
        pid_t pid = vfork();
        if (pid < 0) {
            (void)sigaction(SIGCHLD, &old_action, NULL);
            return fail("vfork-auto-reap");
        }
        if (pid == 0)
            _exit(0);
    }

    if (sigaction(SIGCHLD, &old_action, NULL) < 0)
        return fail("vfork-restore-sigchld");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--verify-exec-args") == 0)
        return verify_large_exec_args(argc, argv);

    printf("PROC_STRESS: start\n");
    if (scenario_fork_wait_yield() != 0)
        return 1;
    if (scenario_exec_wait() != 0)
        return 1;
    if (scenario_exec_large_args() != 0)
        return 1;
    if (scenario_spawn_missing() != 0)
        return 1;
    if (scenario_sleep_signal() != 0)
        return 1;
    if (scenario_pipe_wake() != 0)
        return 1;
    if (scenario_futex_wake() != 0)
        return 1;
    if (scenario_vfork_auto_reap() != 0)
        return 1;
    printf("PROC_STRESS: vfork-auto-reap PASS\n");
    printf("PROC_STRESS: PASS\n");
    return 0;
}
