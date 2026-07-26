#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
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

static volatile sig_atomic_t signal_seen;
static int signal_pipe_fd = -1;

static int fail(const char *what)
{
    printf("PROC_STRESS: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static void note_signal(int signum)
{
    (void)signum;
    signal_seen = 1;
    if (signal_pipe_fd >= 0)
        (void)write(signal_pipe_fd, "s", 1);
}

static int install_signal_handler(int signum)
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = note_signal;
    sigemptyset(&action.sa_mask);
    return sigaction(signum, &action, NULL);
}

static int read_byte(int fd, char *value, const char *what)
{
    ssize_t n;
    do {
        n = read(fd, value, 1);
    } while (n < 0 && errno == EINTR);
    if (n != 1)
        return fail(what);
    return 0;
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

static int scenario_signal_stop_exit(void)
{
    int ready[2];
    int handled[2];
    if (pipe(ready) < 0 || pipe(handled) < 0)
        return fail("signal-stop-pipe");

    pid_t pid = fork();
    if (pid < 0)
        return fail("signal-stop-fork");
    if (pid == 0) {
        close(ready[0]);
        close(handled[0]);
        signal_pipe_fd = handled[1];
        if (install_signal_handler(SIGUSR1) < 0)
            _exit(2);
        if (write(ready[1], "r", 1) != 1)
            _exit(3);
        for (;;)
            pause();
    }

    close(ready[1]);
    close(handled[1]);
    char byte = 0;
    if (read_byte(ready[0], &byte, "signal-stop-ready") != 0)
        return 1;
    close(ready[0]);

    if (kill(pid, SIGSTOP) < 0)
        return fail("signal-stop-send-stop");
    int status = 0;
    if (waitpid(pid, &status, WUNTRACED) != pid ||
        !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
        return fail("signal-stop-wait-stopped");

    if (kill(pid, SIGUSR1) < 0)
        return fail("signal-stop-send-ordinary");
    struct pollfd pfd = {
        .fd = handled[0],
        .events = POLLIN,
    };
    int poll_result = poll(&pfd, 1, 30);
    if (poll_result != 0)
        return fail("signal-stop-ordinary-resumed");

    if (kill(pid, SIGCONT) < 0)
        return fail("signal-stop-send-cont");
    status = 0;
    if (waitpid(pid, &status, WCONTINUED) != pid ||
        !WIFCONTINUED(status))
        return fail("signal-stop-wait-continued");
    poll_result = poll(&pfd, 1, 1000);
    if (poll_result != 1 ||
        read_byte(handled[0], &byte, "signal-stop-handler-byte") != 0)
        return fail("signal-stop-pending-delivery");

    if (kill(pid, SIGSTOP) < 0)
        return fail("signal-stop-send-stop-again");
    status = 0;
    if (waitpid(pid, &status, WUNTRACED) != pid ||
        !WIFSTOPPED(status))
        return fail("signal-stop-wait-stopped-again");
    if (kill(pid, SIGKILL) < 0)
        return fail("signal-stop-send-kill");
    status = 0;
    if (waitpid(pid, &status, 0) != pid ||
        !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
        return fail("signal-stop-fatal-exit");

    close(handled[0]);
    return 0;
}

static int scenario_sigsuspend_park(void)
{
    enum { ROUNDS = 24 };
    int ready[2];
    if (pipe(ready) < 0)
        return fail("sigsuspend-pipe");

    pid_t pid = fork();
    if (pid < 0)
        return fail("sigsuspend-fork");
    if (pid == 0) {
        close(ready[0]);
        if (install_signal_handler(SIGUSR1) < 0)
            _exit(2);

        sigset_t blocked;
        sigset_t temporary;
        sigemptyset(&blocked);
        sigaddset(&blocked, SIGUSR1);
        sigemptyset(&temporary);
        if (sigprocmask(SIG_BLOCK, &blocked, NULL) < 0)
            _exit(3);

        for (int round = 0; round < ROUNDS; round++) {
            signal_seen = 0;
            if (write(ready[1], "r", 1) != 1)
                _exit(4);
            errno = 0;
            if (sigsuspend(&temporary) != -1 || errno != EINTR ||
                !signal_seen)
                _exit(5);
        }
        _exit(0);
    }

    close(ready[1]);
    for (int round = 0; round < ROUNDS; round++) {
        char byte = 0;
        if (read_byte(ready[0], &byte, "sigsuspend-ready") != 0)
            return 1;
        if (kill(pid, SIGUSR1) < 0)
            return fail("sigsuspend-send");
    }
    close(ready[0]);
    return wait_exit(pid, 0, "sigsuspend-wait");
}

static int scenario_sigtimedwait_park(void)
{
    enum { ROUNDS = 24 };
    int ready[2];
    if (pipe(ready) < 0)
        return fail("sigtimedwait-pipe");

    pid_t pid = fork();
    if (pid < 0)
        return fail("sigtimedwait-fork");
    if (pid == 0) {
        close(ready[0]);
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR2);
        if (sigprocmask(SIG_BLOCK, &set, NULL) < 0)
            _exit(2);

        struct timespec timeout = {1, 0};
        for (int round = 0; round < ROUNDS; round++) {
            if (write(ready[1], "r", 1) != 1)
                _exit(3);
            siginfo_t info;
            memset(&info, 0, sizeof(info));
            if (sigtimedwait(&set, &info, &timeout) != SIGUSR2 ||
                info.si_signo != SIGUSR2)
                _exit(4);
        }
        _exit(0);
    }

    close(ready[1]);
    for (int round = 0; round < ROUNDS; round++) {
        char byte = 0;
        if (read_byte(ready[0], &byte, "sigtimedwait-ready") != 0)
            return 1;
        if (kill(pid, SIGUSR2) < 0)
            return fail("sigtimedwait-send");
    }
    close(ready[0]);
    return wait_exit(pid, 0, "sigtimedwait-wait");
}

static int scenario_eventfd_signal(int block_signal)
{
    int efd = eventfd(0, 0);
    int ready[2];
    if (efd < 0 || pipe(ready) < 0)
        return fail("eventfd-signal-setup");

    pid_t pid = fork();
    if (pid < 0)
        return fail("eventfd-signal-fork");
    if (pid == 0) {
        close(ready[0]);
        if (install_signal_handler(SIGUSR1) < 0)
            _exit(2);
        if (block_signal) {
            sigset_t set;
            sigemptyset(&set);
            sigaddset(&set, SIGUSR1);
            if (sigprocmask(SIG_BLOCK, &set, NULL) < 0)
                _exit(3);
        }
        if (write(ready[1], "r", 1) != 1)
            _exit(4);

        uint64_t value = 0;
        errno = 0;
        ssize_t n = read(efd, &value, sizeof(value));
        if (block_signal)
            _exit(n == (ssize_t)sizeof(value) && value == 1 &&
                  !signal_seen ? 0 : 5);
        _exit(n < 0 && errno == EINTR && signal_seen ? 0 : 6);
    }

    close(ready[1]);
    char byte = 0;
    if (read_byte(ready[0], &byte, "eventfd-signal-ready") != 0)
        return 1;
    close(ready[0]);
    usleep(20000);
    if (kill(pid, SIGUSR1) < 0)
        return fail("eventfd-signal-send");
    if (block_signal) {
        uint64_t one = 1;
        usleep(20000);
        if (write(efd, &one, sizeof(one)) != (ssize_t)sizeof(one))
            return fail("eventfd-signal-release");
    }
    int result = wait_exit(pid, 0, block_signal ?
                           "eventfd-blocked-wait" :
                           "eventfd-interrupt-wait");
    close(efd);
    return result;
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
    if (scenario_signal_stop_exit() != 0)
        return 1;
    printf("PROC_STRESS: signal-stop-exit PASS\n");
    if (scenario_sigsuspend_park() != 0)
        return 1;
    if (scenario_sigtimedwait_park() != 0)
        return 1;
    if (scenario_eventfd_signal(0) != 0)
        return 1;
    if (scenario_eventfd_signal(1) != 0)
        return 1;
    printf("PROC_STRESS: signal-mask-park PASS\n");
    printf("PROC_STRESS: PASS\n");
    return 0;
}
