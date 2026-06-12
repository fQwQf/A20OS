#include <errno.h>
#include <signal.h>
#include <stdio.h>
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

int main(void)
{
    printf("PROC_STRESS: start\n");
    if (scenario_fork_wait_yield() != 0)
        return 1;
    if (scenario_exec_wait() != 0)
        return 1;
    if (scenario_sleep_signal() != 0)
        return 1;
    if (scenario_pipe_wake() != 0)
        return 1;
    if (scenario_futex_wake() != 0)
        return 1;
    printf("PROC_STRESS: PASS\n");
    return 0;
}
