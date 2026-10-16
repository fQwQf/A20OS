#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <pthread.h>
#include <poll.h>

static int fail(const char *what)
{
    printf("SYSCALL_SMOKE: FAIL %s errno=%d\n", what, errno);
    return 1;
}

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif
#ifndef SYS_prctl
#define SYS_prctl 167
#endif
#define PR_A20_TASK_CONSTRUCTION_FAIL 0xA20F
#define TASK_CONSTRUCTION_STAGE_MM 9

static int thread_wait_pipe[2];
static int thread_wait_pid;
static int thread_wait_pidfd;
static int thread_wait_error;
static int zombie_group_release_fd;

#if defined(__x86_64__)
#ifndef SYS_arch_prctl
#define SYS_arch_prctl 158
#endif
#define X86_ARCH_GET_FS 0x1003

typedef struct {
    uint64_t fs_base;
    int failed;
} x86_tls_probe_t;

static uint64_t x86_get_fs_base(void)
{
    uint64_t fs_base = 0;
    if (syscall(SYS_arch_prctl, X86_ARCH_GET_FS, &fs_base) < 0)
        return 0;
    return fs_base;
}

static void *x86_tls_probe_thread(void *arg)
{
    x86_tls_probe_t *probe = arg;
    probe->fs_base = x86_get_fs_base();
    if (!probe->fs_base) {
        probe->failed = 1;
        return NULL;
    }
    for (int i = 0; i < 64; i++) {
        if (sched_yield() < 0 || getpid() <= 0 ||
            x86_get_fs_base() != probe->fs_base) {
            probe->failed = 2;
            break;
        }
    }
    return NULL;
}

static int test_x86_thread_fs_base(void)
{
    enum { THREADS = 4 };
    pthread_t threads[THREADS];
    x86_tls_probe_t probes[THREADS] = {{0}};
    uint64_t main_fs = x86_get_fs_base();
    if (!main_fs)
        return fail("x86-fs-main-get");

    for (int i = 0; i < THREADS; i++) {
        if (pthread_create(&threads[i], NULL, x86_tls_probe_thread,
                           &probes[i]) != 0)
            return fail("x86-fs-thread-create");
    }
    for (int i = 0; i < THREADS; i++) {
        if (pthread_join(threads[i], NULL) != 0)
            return fail("x86-fs-thread-join");
        if (probes[i].failed || !probes[i].fs_base ||
            probes[i].fs_base == main_fs)
            return fail("x86-fs-thread-stability");
        for (int j = 0; j < i; j++) {
            if (probes[i].fs_base == probes[j].fs_base)
                return fail("x86-fs-thread-unique");
        }
    }
    puts("X86_TLS_SMOKE: PASS threads=4 yields=64");
    return 0;
}
#endif

static void *zombie_group_member(void *unused)
{
    (void)unused;
    char byte;
    while (read(zombie_group_release_fd, &byte, 1) < 0 && errno == EINTR)
        ;
    syscall(SYS_exit, 0);
    return NULL;
}

static void zombie_group_child(int ready_fd, int release_fd, int exit_fd)
{
    zombie_group_release_fd = release_fd;
    pthread_t member;
    if (pthread_create(&member, NULL, zombie_group_member, NULL) != 0)
        _exit(70);
    if (write(ready_fd, "r", 1) != 1)
        _exit(71);
    char exit_byte;
    if (read(exit_fd, &exit_byte, 1) != 1)
        _exit(72);
    /* Exit just this thread, leaving a live member behind.  libc _exit uses
     * exit_group and would terminate the scenario instead. */
    syscall(SYS_exit, 23);
    _exit(73);
}

static int proc_pid_state(pid_t pid)
{
    char path[64];
    char line[512];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    FILE *file = fopen(path, "r");
    if (!file)
        return -1;
    if (!fgets(line, sizeof(line), file)) {
        fclose(file);
        return -1;
    }
    fclose(file);
    char *comm_end = strrchr(line, ')');
    return comm_end && comm_end[1] == ' ' ? comm_end[2] : -1;
}

static int test_wait_zombie_leader_with_live_member(void)
{
    int ready_pipe[2], release_pipe[2], exit_pipe[2];
    if (pipe(ready_pipe) < 0 || pipe(release_pipe) < 0 || pipe(exit_pipe) < 0)
        return fail("wait-live-group-pipe");
    pid_t pid = fork();
    if (pid < 0)
        return fail("wait-live-group-fork");
    if (pid == 0) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        close(exit_pipe[1]);
        zombie_group_child(ready_pipe[1], release_pipe[0], exit_pipe[0]);
    }

    close(ready_pipe[1]);
    close(release_pipe[0]);
    close(exit_pipe[0]);
    char byte;
    if (read(ready_pipe[0], &byte, 1) != 1) {
        close(ready_pipe[0]);
        close(release_pipe[1]);
        close(exit_pipe[1]);
        return fail("wait-live-group-ready");
    }
    close(ready_pipe[0]);
    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        close(exit_pipe[1]);
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-pidfd");
    }

    if (write(exit_pipe[1], "x", 1) != 1) {
        close(exit_pipe[1]);
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-exit-leader");
    }
    close(exit_pipe[1]);
    sched_yield();

    /* Wait until procfs confirms the leader is zombie.  The group member
     * remains blocked on release_pipe, so pidfd must still be unreadable. */
    int leader_zombie = 0;
    for (int attempt = 0; attempt < 10000; attempt++) {
        if (proc_pid_state(pid) == 'Z') {
            leader_zombie = 1;
            break;
        }
        sched_yield();
    }
    if (!leader_zombie) {
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-leader-zombie-timeout");
    }

    int zombie_pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (zombie_pidfd < 0) {
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-open-zombie-pidfd");
    }
    struct pollfd pfd = {.fd = pidfd, .events = POLLIN, .revents = 0};
    int polled = poll(&pfd, 1, 0);
    close(zombie_pidfd);
    if (polled != 0 || pfd.revents != 0) {
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        return fail("wait-live-group-pidfd-early-readable");
    }

    siginfo_t info;
    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED | WNOHANG) < 0 ||
        info.si_pid != 0) {
        int saved_errno = errno;
        write(release_pipe[1], "x", 1);
        close(release_pipe[1]);
        close(pidfd);
        waitpid(pid, NULL, 0);
        errno = saved_errno;
        return fail("wait-live-group-nohang");
    }

    if (write(release_pipe[1], "x", 1) != 1) {
        close(release_pipe[1]);
        close(pidfd);
        return fail("wait-live-group-release");
    }
    close(release_pipe[1]);
    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED | WNOWAIT) < 0) {
        close(pidfd);
        return fail("wait-live-group-wnowait");
    }
    if (info.si_pid != pid || info.si_status != 23) {
        close(pidfd);
        return fail("wait-live-group-final-info");
    }
    pfd.revents = 0;
    polled = poll(&pfd, 1, 0);
    if (polled != 1 || !(pfd.revents & POLLIN)) {
        close(pidfd);
        return fail("wait-live-group-pidfd-readable");
    }
    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)pidfd, &info, WEXITED) < 0) {
        close(pidfd);
        return fail("wait-live-group-reap");
    }
    close(pidfd);
    return 0;
}

/*
 * wait4 option/selector matrix.  The C reference engine and the Rust
 * controller must decide these identically:
 *  - pid == 0 and pid < -1 select on the child's process group;
 *  - __WNOTHREAD restricts the scan to the calling thread's own children;
 *  - stop/continue reports are one-shot flags, observable but not consumed
 *    under WNOWAIT;
 *  - a signal delivered while parked must surface as EINTR (no handler
 *    restart), and the reap must still succeed afterwards.
 */
static int test_wait4_pgid_selectors(void)
{
    int ready_pipe[2], a_gate[2], b_gate[2];
    if (pipe(ready_pipe) < 0 || pipe(a_gate) < 0 || pipe(b_gate) < 0)
        return fail("wait4-pgid-pipe");

    /* Child A moves itself into its own process group; child B stays in
     * ours.  This makes pid==0 and pid==-pgid select different children. */
    pid_t a = fork();
    if (a < 0)
        return fail("wait4-pgid-fork-a");
    if (a == 0) {
        close(ready_pipe[0]);
        close(b_gate[0]);
        close(b_gate[1]);
        close(a_gate[1]);
        if (setpgid(0, 0) < 0)
            _exit(60);
        if (write(ready_pipe[1], "a", 1) != 1)
            _exit(61);
        close(ready_pipe[1]);
        char gate_byte;
        if (read(a_gate[0], &gate_byte, 1) != 1)
            _exit(62);
        _exit(55);
    }
    close(ready_pipe[1]);

    pid_t b = fork();
    if (b < 0)
        return fail("wait4-pgid-fork-b");
    if (b == 0) {
        close(ready_pipe[0]);
        close(a_gate[0]);
        close(a_gate[1]);
        close(b_gate[1]);
        char gate_byte;
        if (read(b_gate[0], &gate_byte, 1) != 1)
            _exit(63);
        _exit(66);
    }
    close(a_gate[0]);
    close(b_gate[0]);

    char ready_byte;
    if (read(ready_pipe[0], &ready_byte, 1) != 1) {
        close(ready_pipe[0]);
        return fail("wait4-pgid-a-ready");
    }
    close(ready_pipe[0]);

    /* pid > 0 on a live child: WNOHANG reports nothing yet. */
    int status = 0;
    if (waitpid(b, &status, WNOHANG) != 0)
        return fail("wait4-pgid-live-nohang");

    if (write(b_gate[1], "x", 1) != 1)
        return fail("wait4-pgid-release-b");
    close(b_gate[1]);

    /* pid == 0 selects by our own pgid: B matches, A does not. */
    if (waitpid(0, &status, 0) != b || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 66)
        return fail("wait4-pgid-zero-reap");

    if (write(a_gate[1], "x", 1) != 1)
        return fail("wait4-pgid-release-a");
    close(a_gate[1]);

    /* Only A is left, and it is in another process group: the group
     * selector has no eligible child left, so this is ECHILD, not a
     * live-child zero -- and not an unrelated task. */
    errno = 0;
    if (waitpid(0, &status, WNOHANG) != -1 || errno != ECHILD)
        return fail("wait4-pgid-zero-mismatch");

    errno = 0;
    if (waitpid(getpid(), &status, WNOHANG) != -1 || errno != ECHILD)
        return fail("wait4-pgid-nonchild");

    /* pid == -pgid matches A in its new group. */
    if (waitpid(-a, &status, 0) != a || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 55)
        return fail("wait4-pgid-negative-reap");

    errno = 0;
    if (waitpid(-a, NULL, WNOHANG) != -1 || errno != ECHILD)
        return fail("wait4-pgid-reaped-echild");

    puts("SYSCALL_SMOKE: wait4-pgid-selectors PASS");
    return 0;
}

typedef struct {
    int ready_fd;           /* worker -> main: child pid */
    int go_fd;              /* main -> worker: start reaping */
    int done_fd;            /* worker -> main: reap finished */
    int child_gate_read_fd; /* child reads here to be released */
    int child_gate_write_fd;/* main writes here to release the child */
    int child_pid;
    int reaped_pid;
    int reaped_status;
} wnothread_ctx_t;

static void *wnothread_worker(void *arg)
{
    wnothread_ctx_t *ctx = arg;
    pid_t child = fork();
    if (child < 0) {
        /* Publish the failure so the main thread's read does not hang. */
        pid_t failed = -1;
        (void)write(ctx->ready_fd, &failed, sizeof(failed));
        return NULL;
    }
    if (child == 0) {
        close(ctx->child_gate_write_fd);
        char gate_byte;
        if (read(ctx->child_gate_read_fd, &gate_byte, 1) != 1)
            _exit(64);
        _exit(44);
    }
    close(ctx->child_gate_read_fd);
    ctx->child_pid = (int)child;
    if (write(ctx->ready_fd, &child, sizeof(child)) != (ssize_t)sizeof(child))
        return NULL;
    char go_byte;
    if (read(ctx->go_fd, &go_byte, 1) != 1)
        return NULL;
    /* The child was created by this thread: __WNOTHREAD still matches it,
     * and a still-running child parks this thread until exit. */
    int status = 0;
    pid_t reaped = waitpid(child, &status, __WNOTHREAD);
    ctx->reaped_pid = (int)reaped;
    ctx->reaped_status = status;
    char done_byte = 'd';
    (void)write(ctx->done_fd, &done_byte, 1);
    return NULL;
}

static int test_wait4_wnothread_scope(void)
{
    int ready_pipe[2], go_pipe[2], done_pipe[2], child_gate[2];
    if (pipe(ready_pipe) < 0 || pipe(go_pipe) < 0 || pipe(done_pipe) < 0 ||
        pipe(child_gate) < 0)
        return fail("wait4-wnothread-pipe");

    wnothread_ctx_t ctx = {
        .ready_fd = ready_pipe[1],
        .go_fd = go_pipe[0],
        .done_fd = done_pipe[1],
        .child_gate_read_fd = child_gate[0],
        .child_gate_write_fd = child_gate[1],
        .child_pid = -1,
        .reaped_pid = 0,
        .reaped_status = 0,
    };
    pthread_t helper;
    int err = pthread_create(&helper, NULL, wnothread_worker, &ctx);
    if (err != 0) {
        errno = err;
        return fail("wait4-wnothread-create");
    }

    pid_t child = -1;
    if (read(ready_pipe[0], &child, sizeof(child)) != (ssize_t)sizeof(child))
        return fail("wait4-wnothread-pid");
    if (child <= 0 || child != ctx.child_pid)
        return fail("wait4-wnothread-pid-value");

    /* The child belongs to the helper thread, not to this one. */
    int status = 0;
    errno = 0;
    if (waitpid(child, &status, WNOHANG | __WNOTHREAD) != -1 ||
        errno != ECHILD)
        return fail("wait4-wnothread-main-echild");

    /* Without __WNOTHREAD the whole thread group sees it: alive, so 0. */
    if (waitpid(child, &status, WNOHANG) != 0)
        return fail("wait4-wnothread-group-scope");

    /* Let the helper reap while the child is still in its gate (or right
     * after it exits), then release the child. */
    if (write(go_pipe[1], "g", 1) != 1)
        return fail("wait4-wnothread-go");
    close(go_pipe[1]);
    if (write(child_gate[1], "x", 1) != 1)
        return fail("wait4-wnothread-release");
    close(child_gate[1]);

    if (pthread_join(helper, NULL) != 0)
        return fail("wait4-wnothread-join");
    if (ctx.reaped_pid != child || !WIFEXITED(ctx.reaped_status) ||
        WEXITSTATUS(ctx.reaped_status) != 44)
        return fail("wait4-wnothread-reap");

    errno = 0;
    if (waitpid(child, NULL, WNOHANG) != -1 || errno != ECHILD)
        return fail("wait4-wnothread-reaped-echild");

    puts("SYSCALL_SMOKE: wait4-wnothread PASS");
    return 0;
}

static int test_wait4_stop_continue_matrix(void)
{
    int ready_pipe[2];
    if (pipe(ready_pipe) < 0)
        return fail("wait4-stop-pipe");
    pid_t pid = fork();
    if (pid < 0)
        return fail("wait4-stop-fork");
    if (pid == 0) {
        close(ready_pipe[0]);
        if (write(ready_pipe[1], "r", 1) != 1)
            _exit(65);
        close(ready_pipe[1]);
        for (;;)
            pause();
    }
    close(ready_pipe[1]);
    char ready_byte;
    if (read(ready_pipe[0], &ready_byte, 1) != 1) {
        close(ready_pipe[0]);
        return fail("wait4-stop-ready");
    }
    close(ready_pipe[0]);

    int status = 0;
    if (waitpid(pid, &status, WUNTRACED | WNOHANG) != 0)
        return fail("wait4-stop-running-nohang");

    if (kill(pid, SIGSTOP) < 0)
        return fail("wait4-stop-send");

    /* WNOWAIT observes the stop without consuming the report ... */
    status = 0;
    if (waitpid(pid, &status, WUNTRACED | WNOWAIT) != pid ||
        !WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
        return fail("wait4-stop-wnowait");
    int first_stop = status;

    /* ... a plain WUNTRACED consumes it, and the report is one-shot. */
    status = 0;
    if (waitpid(pid, &status, WUNTRACED) != pid || status != first_stop)
        return fail("wait4-stop-consume");
    if (waitpid(pid, &status, WUNTRACED | WNOHANG) != 0)
        return fail("wait4-stop-one-shot");
    /* Without WUNTRACED a stopped child reports nothing. */
    if (waitpid(pid, &status, WNOHANG) != 0)
        return fail("wait4-stop-not-wuntraced");

    if (kill(pid, SIGCONT) < 0)
        return fail("wait4-stop-continue-send");
    status = 0;
    if (waitpid(pid, &status, WCONTINUED) != pid || !WIFCONTINUED(status))
        return fail("wait4-stop-continued");
    if (waitpid(pid, &status, WCONTINUED | WNOHANG) != 0)
        return fail("wait4-stop-continued-one-shot");

    if (kill(pid, SIGKILL) < 0)
        return fail("wait4-stop-kill");
    status = 0;
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGKILL)
        return fail("wait4-stop-killed-status");

    puts("SYSCALL_SMOKE: wait4-stop-continue PASS");
    return 0;
}

static volatile sig_atomic_t wait4_sigusr1_seen;

static void wait4_sigusr1_handler(int signum)
{
    (void)signum;
    wait4_sigusr1_seen++;
}

static int test_wait4_eintr(void)
{
    struct sigaction action, old_action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = wait4_sigusr1_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; /* no SA_RESTART: the parked wait must surface EINTR */
    if (sigaction(SIGUSR1, &action, &old_action) < 0)
        return fail("wait4-eintr-sigaction");
    wait4_sigusr1_seen = 0;

    pid_t pid = fork();
    if (pid < 0)
        return fail("wait4-eintr-fork");
    if (pid == 0) {
        /* Sleep well past the parent's entry into the wait, interrupt it,
         * then exit so the retry has something to reap. */
        struct timespec delay = { .tv_sec = 0, .tv_nsec = 200000000 };
        nanosleep(&delay, NULL);
        kill(getppid(), SIGUSR1);
        _exit(56);
    }

    int status = 0;
    errno = 0;
    pid_t interrupted = waitpid(pid, &status, 0);
    int saved_errno = errno;
    if (interrupted != -1 || saved_errno != EINTR ||
        wait4_sigusr1_seen != 1)
        return fail("wait4-eintr-interrupted");

    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 56)
        return fail("wait4-eintr-reap");
    if (wait4_sigusr1_seen != 1)
        return fail("wait4-eintr-handler-count");

    if (sigaction(SIGUSR1, &old_action, NULL) < 0)
        return fail("wait4-eintr-restore");
    puts("SYSCALL_SMOKE: wait4-eintr PASS");
    return 0;
}

static int test_wait4_reap_storm(void)
{
    enum { ROUNDS = 16 };

    for (int round = 0; round < ROUNDS; round++) {
        pid_t pid = fork();
        if (pid < 0)
            return fail("wait4-storm-fork");
        if (pid == 0) {
            if (round & 1)
                sched_yield();
            _exit(40 + (round & 7));
        }
        int status = 0;
        if (waitpid(pid, &status, 0) != pid)
            return fail("wait4-storm-wait");
        if (!WIFEXITED(status) || WEXITSTATUS(status) != 40 + (round & 7))
            return fail("wait4-storm-status");
        errno = 0;
        if (waitpid(pid, NULL, WNOHANG) != -1 || errno != ECHILD)
            return fail("wait4-storm-echild");
    }

    printf("SYSCALL_SMOKE: wait4-reap-storm PASS rounds=%d\n", ROUNDS);
    return 0;
}

/* Create a process from a non-leader thread, then let that thread exit.  The
 * main thread must still be able to wait for its thread-group child's pidfd. */
static void *thread_spawn_child(void *unused)
{
    (void)unused;
    pid_t pid = fork();
    if (pid < 0) {
        thread_wait_error = errno ? errno : ECHILD;
        return NULL;
    }
    if (pid == 0) {
        close(thread_wait_pipe[1]);
        char byte;
        if (read(thread_wait_pipe[0], &byte, 1) != 1)
            _exit(62);
        _exit(61);
    }

    int pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (pidfd < 0) {
        thread_wait_error = errno ? errno : ECHILD;
        return NULL;
    }
    thread_wait_pid = (int)pid;
    thread_wait_pidfd = pidfd;
    return NULL;
}

static int test_parent_exit_reap_race(void)
{
    enum { ROUNDS = 32 };

    for (int round = 0; round < ROUNDS; round++) {
        int report_pipe[2], parent_gate[2], child_gate[2];
        if (pipe(report_pipe) < 0 || pipe(parent_gate) < 0 ||
            pipe(child_gate) < 0)
            return fail("parent-reap-race-pipe");

        pid_t parent = fork();
        if (parent < 0)
            return fail("parent-reap-race-fork-parent");
        if (parent == 0) {
            close(report_pipe[0]);
            close(parent_gate[1]);
            close(child_gate[1]);

            pid_t child = fork();
            if (child < 0)
                _exit(80);
            if (child == 0) {
                close(report_pipe[1]);
                close(parent_gate[0]);
                char token;
                if (read(child_gate[0], &token, 1) != 1)
                    _exit(81);
                close(child_gate[0]);
                _exit(73);
            }

            close(child_gate[0]);

            if (write(report_pipe[1], &child, sizeof(child)) !=
                (ssize_t)sizeof(child))
                _exit(82);
            close(report_pipe[1]);
            char token;
            if (read(parent_gate[0], &token, 1) != 1)
                _exit(83);
            close(parent_gate[0]);
            _exit(74);
        }

        close(report_pipe[1]);
        close(parent_gate[0]);
        close(child_gate[0]);
        pid_t child = -1;
        if (read(report_pipe[0], &child, sizeof(child)) !=
            (ssize_t)sizeof(child))
            return fail("parent-reap-race-child-pid");
        close(report_pipe[0]);
        int pidfd = (int)syscall(SYS_pidfd_open, child, 0);

        /* Release the nested child and its parent back-to-back. Alternating
         * order covers both parent-first reparenting and child-first exit. */
        char token = 'x';
        if (round & 1) {
            if (write(parent_gate[1], &token, 1) != 1 ||
                write(child_gate[1], &token, 1) != 1)
                return fail("parent-reap-race-release");
        } else {
            if (write(child_gate[1], &token, 1) != 1 ||
                write(parent_gate[1], &token, 1) != 1)
                return fail("parent-reap-race-release");
        }
        close(parent_gate[1]);
        close(child_gate[1]);

        int status = 0;
        if (waitpid(parent, &status, 0) != parent || !WIFEXITED(status) ||
            WEXITSTATUS(status) != 74)
            return fail("parent-reap-race-parent-exit");
        if (pidfd < 0)
            return fail("parent-reap-race-pidfd");
        struct pollfd pfd = {.fd = pidfd, .events = POLLIN, .revents = 0};
        if (poll(&pfd, 1, 5000) != 1 || !(pfd.revents & POLLIN)) {
            close(pidfd);
            return fail("parent-reap-race-child-exit");
        }
        close(pidfd);
    }

    printf("SYSCALL_SMOKE: parent-exit-reap stress PASS rounds=%d\n", ROUNDS);
    return 0;
}

static int test_thread_group_pidfd_wait(void)
{
    if (pipe(thread_wait_pipe) < 0)
        return fail("thread-wait-pipe");
    thread_wait_pid = -1;
    thread_wait_pidfd = -1;
    thread_wait_error = 0;

    pthread_t helper;
    int err = pthread_create(&helper, NULL, thread_spawn_child, NULL);
    if (err != 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        errno = err;
        return fail("thread-wait-create");
    }
    err = pthread_join(helper, NULL);
    if (err != 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        errno = err;
        return fail("thread-wait-join");
    }
    if (thread_wait_error || thread_wait_pid <= 0 || thread_wait_pidfd < 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        close(thread_wait_pidfd);
        errno = thread_wait_error ? thread_wait_error : ECHILD;
        return fail("thread-wait-spawn");
    }

    siginfo_t info;
    memset(&info, 0xa5, sizeof(info));
    if (waitid(P_PIDFD, (id_t)thread_wait_pidfd, &info,
               WEXITED | WNOHANG) < 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        close(thread_wait_pidfd);
        return fail("thread-wait-live-child");
    }
    if (info.si_pid != 0) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        close(thread_wait_pidfd);
        return fail("thread-wait-nohang-result");
    }

    if (write(thread_wait_pipe[1], "x", 1) != 1) {
        close(thread_wait_pipe[0]);
        close(thread_wait_pipe[1]);
        close(thread_wait_pidfd);
        return fail("thread-wait-release-child");
    }
    close(thread_wait_pipe[0]);
    close(thread_wait_pipe[1]);

    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)thread_wait_pidfd, &info,
               WEXITED | WNOWAIT) < 0)
        return fail("thread-wait-wnowait");
    if (info.si_signo != SIGCHLD || info.si_code != CLD_EXITED ||
        info.si_pid != thread_wait_pid || info.si_status != 61)
        return fail("thread-wait-wnowait-info");

    memset(&info, 0, sizeof(info));
    if (waitid(P_PIDFD, (id_t)thread_wait_pidfd, &info, WEXITED) < 0)
        return fail("thread-wait-reap");
    if (info.si_signo != SIGCHLD || info.si_pid != thread_wait_pid ||
        info.si_status != 61)
        return fail("thread-wait-reap-info");
    close(thread_wait_pidfd);

    errno = 0;
    if (waitpid(thread_wait_pid, NULL, WNOHANG) != -1 || errno != ECHILD)
        return fail("thread-wait-reaped-echild");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1) {
        if (argc != 3 || !argv[1] || !argv[2] ||
            strcmp(argv[2], "sentinel") != 0)
            return fail("exec-argv");
        if (strcmp(argv[1], "vfork-exec") == 0) {
            puts("SYSCALL_SMOKE: vfork-exec-child PASS");
            return 0;
        }
        if (strcmp(argv[1], "exec-argv") != 0)
            return fail("exec-argv");
        return 0;
    }

    printf("SYSCALL_SMOKE: start\n");

    if (getpid() <= 0)
        return fail("getpid");

#if defined(__x86_64__)
    if (test_x86_thread_fs_base())
        return 1;
#endif

    int pfd[2];
    if (pipe(pfd) < 0)
        return fail("pipe");
    const char *msg = "a20-linux-abi";
    char buf[64];
    if (write(pfd[1], msg, strlen(msg)) != (ssize_t)strlen(msg))
        return fail("pipe-write");
    int n = read(pfd[0], buf, sizeof(buf) - 1);
    if (n < 0)
        return fail("pipe-read");
    buf[n] = '\0';
    close(pfd[0]);
    close(pfd[1]);
    if (strcmp(buf, msg) != 0)
        return fail("pipe-compare");

    const char *path = "/tmp/syscall_smoke.txt";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("open");
    if (write(fd, msg, strlen(msg)) != (ssize_t)strlen(msg))
        return fail("write");
    if (lseek(fd, 0, SEEK_SET) < 0)
        return fail("lseek");
    memset(buf, 0, sizeof(buf));
    n = read(fd, buf, sizeof(buf) - 1);
    if (n < 0)
        return fail("read");
    close(fd);
    if (strcmp(buf, msg) != 0)
        return fail("file-compare");

    struct stat st;
    if (stat(path, &st) < 0 || st.st_size != (off_t)strlen(msg))
        return fail("stat");

    /* renameat(2): glibc calls it directly; verify the rename + new path. */
#ifndef SYS_renameat
#define SYS_renameat 38
#endif
    char oldpath[64], newpath[64];
    snprintf(oldpath, sizeof(oldpath), "%s.old", path);
    snprintf(newpath, sizeof(newpath), "%s.new", path);
    int ofd = open(oldpath, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (ofd < 0)
        return fail("renameat-create");
    if (write(ofd, msg, strlen(msg)) != (ssize_t)strlen(msg))
        return fail("renameat-write");
    close(ofd);
    if (syscall(SYS_renameat, AT_FDCWD, oldpath, AT_FDCWD, newpath) < 0)
        return fail("renameat");
    if (stat(newpath, &st) < 0 || st.st_size != (off_t)strlen(msg))
        return fail("renameat-stat");
    if (stat(oldpath, &st) == 0)
        return fail("renameat-old-exists");
    if (unlink(newpath) < 0)
        return fail("renameat-unlink");

    if (unlink(path) < 0)
        return fail("unlink");

    void *mem = mmap(NULL, 4096, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED)
        return fail("mmap");
    strcpy((char *)mem, "mmap-ok");
    if (strcmp((char *)mem, "mmap-ok") != 0)
        return fail("mmap-compare");
    if (munmap(mem, 4096) < 0)
        return fail("munmap");

    errno = 0;
    mem = mmap(NULL, (size_t)-1, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem != MAP_FAILED || errno == 0)
        return fail("mmap-error");

    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
        return fail("clock_gettime");

#ifdef SYSCALL_SMOKE_TASK_CONSTRUCTION_TEST
    if (syscall(SYS_prctl, PR_A20_TASK_CONSTRUCTION_FAIL,
                TASK_CONSTRUCTION_STAGE_MM, 0, 0, 0) < 0)
        return fail("task-construction-arm-failure");
    errno = 0;
    if (fork() != -1 || errno != ENOMEM)
        return fail("task-construction-mm-failure");
    puts("SYSCALL_SMOKE: task-construction-mm-rollback PASS");
#endif

    int pid = fork();
    if (pid < 0)
        return fail("fork");
    if (pid == 0)
        _exit(42);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 42)
        return fail("wait-status");

    /* vfork must keep the parent suspended until the child execs.  The child
     * touches no shared state before exec and uses only async-safe failure
     * exit if exec fails. */
    pid_t vpid = vfork();
    if (vpid < 0)
        return fail("vfork");
    if (vpid == 0) {
        char *vfork_argv[] = {
            "syscall_smoke", "vfork-exec", "sentinel", NULL
        };
        execv("/bin/syscall_smoke", vfork_argv);
        _exit(127);
    }
    status = 0;
    if (waitpid(vpid, &status, 0) != vpid)
        return fail("vfork-waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("vfork-status");
    puts("SYSCALL_SMOKE: vfork-exec-parent PASS");

    /* Exercise the other vfork completion route: the child exits without
     * exec, so the parent is released by task exit rather than exec teardown. */
    vpid = vfork();
    if (vpid < 0)
        return fail("vfork-noexec");
    if (vpid == 0)
        _exit(43);
    status = 0;
    if (waitpid(vpid, &status, 0) != vpid)
        return fail("vfork-noexec-waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 43)
        return fail("vfork-noexec-status");
    puts("SYSCALL_SMOKE: vfork-noexec-parent PASS");

    if (test_parent_exit_reap_race())
        return 1;

    /* P_PIDFD identifies a task, but does not waive wait-child eligibility.
     * This also ensures waitid does not accidentally accept a live unrelated
     * task after resolving its pidfd. */
    int self_pidfd = (int)syscall(SYS_pidfd_open, getpid(), 0);
    if (self_pidfd < 0)
        return fail("waitid-nonchild-pidfd-open");
    siginfo_t nonchild_info;
    memset(&nonchild_info, 0, sizeof(nonchild_info));
    errno = 0;
    int nonchild_wait = waitid(P_PIDFD, (id_t)self_pidfd, &nonchild_info,
                               WEXITED | WNOHANG);
    int nonchild_errno = errno;
    close(self_pidfd);
    if (nonchild_wait != -1 || nonchild_errno != ECHILD) {
        errno = nonchild_errno;
        return fail("waitid-pidfd-nonchild-echild");
    }
    printf("SYSCALL_SMOKE: pidfd-nonchild ECHILD PASS\n");

    /* waitid siginfo_t follows the Linux ABI: on 64-bit targets the
     * siginfo union begins at offset 16 (offset 12 on 32-bit targets).
     * pidfd + WNOWAIT checks both that layout and non-consuming semantics. */
    int waitid_gate[2];
    if (pipe(waitid_gate) < 0)
        return fail("waitid-gate-pipe");
    pid = fork();
    if (pid < 0)
        return fail("waitid-fork");
    if (pid == 0) {
        close(waitid_gate[1]);
        char gate_byte;
        if (read(waitid_gate[0], &gate_byte, 1) != 1)
            _exit(38);
        _exit(37);
    }
    close(waitid_gate[0]);
    int child_pidfd = (int)syscall(SYS_pidfd_open, pid, 0);
    if (child_pidfd < 0) {
        write(waitid_gate[1], "x", 1);
        close(waitid_gate[1]);
        return fail("pidfd-open");
    }
    if (write(waitid_gate[1], "x", 1) != 1) {
        close(waitid_gate[1]);
        close(child_pidfd);
        return fail("waitid-gate-release");
    }
    close(waitid_gate[1]);
    siginfo_t child_info;
    memset(&child_info, 0xa5, sizeof(child_info));
    if (waitid(P_PIDFD, (id_t)child_pidfd, &child_info,
               WEXITED | WNOWAIT) < 0)
        return fail("waitid-wnowait");
    if (child_info.si_signo != SIGCHLD || child_info.si_code != CLD_EXITED ||
        child_info.si_pid != pid || child_info.si_uid != getuid() ||
        child_info.si_status != 37)
        return fail("waitid-siginfo-layout");
    memset(&child_info, 0, sizeof(child_info));
    if (waitid(P_PIDFD, (id_t)child_pidfd, &child_info, WEXITED) < 0)
        return fail("waitid-reap");
    if (child_info.si_signo != SIGCHLD || child_info.si_pid != pid ||
        child_info.si_uid != getuid() || child_info.si_status != 37)
        return fail("waitid-reap-siginfo");
    close(child_pidfd);
    errno = 0;
    if (waitpid(pid, &status, WNOHANG) != -1 || errno != ECHILD)
        return fail("waitid-echild");

    if (test_thread_group_pidfd_wait())
        return 1;

    if (test_wait_zombie_leader_with_live_member())
        return 1;

    if (test_wait4_pgid_selectors())
        return 1;

    if (test_wait4_wnothread_scope())
        return 1;

    if (test_wait4_stop_continue_matrix())
        return 1;

    if (test_wait4_eintr())
        return 1;

    if (test_wait4_reap_storm())
        return 1;

    pid = fork();
    if (pid < 0)
        return fail("exec-fork");
    if (pid == 0) {
        char *exec_argv[] = {
            "syscall_smoke", "exec-argv", "sentinel", NULL
        };
        execv("/bin/syscall_smoke", exec_argv);
        _exit(127);
    }
    status = 0;
    if (waitpid(pid, &status, 0) < 0)
        return fail("exec-waitpid");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("exec-status");

    /*
     * rt_sigaction's kernel-side struct is musl's struct k_sigaction, which
     * carries sa_restorer between the flags and the mask on every architecture
     * whose signal.h defines SA_RESTORER (aarch64, x86_64, riscv32, arm32,
     * ppc64le) and has no such slot on the others (riscv64, loongarch64).
     * Get the offset wrong in the kernel and the mask is read out of the
     * restorer on the way in, and written where the caller never looks on the
     * way out -- so a disposition set with a mask comes back with an empty
     * one, silently.  Round-trip a non-empty mask and a non-default handler.
     */
    {
        struct sigaction sa, back;

        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = (void (*)(int))0;   /* SIG_DFL */
        sigemptyset(&sa.sa_mask);
        sigaddset(&sa.sa_mask, SIGUSR1);
        /* oldact is deliberately NULL: passing it makes the kernel report the
         * disposition that was installed *before* this call, which is still the
         * empty default, so it says nothing about the layout.  Reading the
         * disposition back with a separate query is the check. */
        if (sigaction(SIGUSR2, &sa, NULL) < 0)
            return fail("sigaction-set");

        memset(&back, 0, sizeof(back));
        if (sigaction(SIGUSR2, NULL, &back) < 0)
            return fail("sigaction-get");
        if (!sigismember(&back.sa_mask, SIGUSR1))
            return fail("sigaction-mask-roundtrip");
        if (back.sa_handler != (void (*)(int))0)
            return fail("sigaction-handler-roundtrip");

        /* SIG_IGN, to cover the other reserved handler value. */
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = (void (*)(int))1;   /* SIG_IGN */
        sigemptyset(&sa.sa_mask);
        if (sigaction(SIGUSR2, &sa, NULL) < 0)
            return fail("sigaction-ignore");
        memset(&back, 0, sizeof(back));
        if (sigaction(SIGUSR2, NULL, &back) < 0)
            return fail("sigaction-ignore-get");
        if (back.sa_handler != (void (*)(int))1)
            return fail("sigaction-ignore-roundtrip");
    }

    printf("SYSCALL_SMOKE: PASS\n");
    return 0;
}
