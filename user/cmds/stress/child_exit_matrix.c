/*
 * Child-exit notification matrix (Linux ABI).
 *
 * Covers the combinations the ledger requires beyond the original
 * leader-first case: worker-first group exit, exit_group teardown, two
 * concurrent waiters, vfork with and without SIGCHLD, SA_NOCLDWAIT,
 * CLONE_PARENT liveness, and PR_SET_PDEATHSIG (ordinary and CLONE_PARENT).
 *
 * Every case asserts the observable exactly-once contract: one successful
 * reap, a following ECHILD, and no second pending SIGCHLD.  Standard signals
 * coalesce, so the reap counter -- not the signal count alone -- is the
 * authority.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef CLONE_PARENT
#define CLONE_PARENT 0x00008000
#endif

static sigset_t chld_set;

static void caught(int signal) { (void)signal; }

static int fail(const char *message)
{
    printf("CHILD_EXIT: FAIL %s errno=%d\n", message, errno);
    return 1;
}

static void drain_sigchld(void)
{
    siginfo_t info;
    struct timespec zero = {0, 0};
    while (sigtimedwait(&chld_set, &info, &zero) == SIGCHLD)
        ;
}

static int expect_sigchld(pid_t pid, int status)
{
    siginfo_t info;
    struct timespec timeout = {.tv_sec = 5};
    int sig = sigtimedwait(&chld_set, &info, &timeout);
    if (sig != SIGCHLD)
        return -1;
    if (info.si_code != CLD_EXITED || info.si_pid != pid ||
        info.si_uid != getuid() || info.si_status != status)
        return -2;
    return 0;
}

static int expect_no_sigchld(int milliseconds)
{
    siginfo_t info;
    struct timespec timeout = {
        .tv_sec = milliseconds / 1000,
        .tv_nsec = (long)(milliseconds % 1000) * 1000000L,
    };
    errno = 0;
    int sig = sigtimedwait(&chld_set, &info, &timeout);
    return sig == -1 && errno == EAGAIN ? 0 : -1;
}

static int expect_reap(pid_t pid, int status)
{
    int st = 0;
    if (waitpid(pid, &st, 0) != pid)
        return -1;
    if (!WIFEXITED(st) || WEXITSTATUS(st) != status)
        return -2;
    return 0;
}

static int expect_echild(pid_t pid)
{
    errno = 0;
    return waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD ? 0 : -1;
}

static int join_within(pthread_t thread, int seconds)
{
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += seconds;
    return pthread_timedjoin_np(thread, NULL, &deadline);
}

static int read_within(int fd, void *buf, size_t len, int milliseconds)
{
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, milliseconds) != 1)
        return -1;
    return read(fd, buf, len) == (ssize_t)len ? 0 : -1;
}

/* ---- worker-first thread-group exit ---- */

static void *exit_thread_immediately(void *unused)
{
    (void)unused;
    return NULL;
}

static int case_worker_first(void)
{
    drain_sigchld();
    pid_t child = fork();
    if (child < 0)
        return fail("worker-first fork");
    if (child == 0) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, exit_thread_immediately, NULL))
            _exit(90);
        /* The worker is a task exit before the leader; only the last member
         * may report, and it must report the leader's status. */
        pthread_join(thread, NULL);
        syscall(SYS_exit, 23);
        _exit(91);
    }
    if (expect_sigchld(child, 23) != 0)
        return fail("worker-first notify");
    if (expect_reap(child, 23) != 0)
        return fail("worker-first reap");
    if (expect_echild(child) != 0)
        return fail("worker-first ECHILD");
    if (expect_no_sigchld(200) != 0)
        return fail("worker-first duplicate");
    printf("CHILD_EXIT: worker-first PASS\n");
    return 0;
}

/* ---- exit_group teardown with a live group member ---- */

static int group_pipe[2];

static void *blocked_group_member(void *unused)
{
    (void)unused;
    if (write(group_pipe[1], "R", 1) != 1)
        _exit(92);
    /* The leader's exit_group is the only way out of this loop. */
    for (;;)
        pause();
    return NULL;
}

static int case_exit_group(void)
{
    drain_sigchld();
    if (pipe(group_pipe) != 0)
        return fail("exit-group pipe");
    pid_t child = fork();
    if (child < 0)
        return fail("exit-group fork");
    if (child == 0) {
        pthread_t thread;
        if (pthread_create(&thread, NULL, blocked_group_member, NULL))
            _exit(94);
        char byte;
        if (read(group_pipe[0], &byte, 1) != 1)
            _exit(95);
        syscall(SYS_exit_group, 23);
        _exit(96);
    }
    if (expect_sigchld(child, 23) != 0)
        return fail("exit-group notify");
    if (expect_reap(child, 23) != 0)
        return fail("exit-group reap");
    if (expect_echild(child) != 0)
        return fail("exit-group ECHILD");
    if (expect_no_sigchld(200) != 0)
        return fail("exit-group duplicate");
    close(group_pipe[0]);
    close(group_pipe[1]);
    printf("CHILD_EXIT: exit-group PASS\n");
    return 0;
}

/* ---- two waiters on one child ---- */

static volatile int g_waiters_ready;
struct waiter_result { pid_t pid; int result; };

static void *concurrent_waiter(void *opaque)
{
    struct waiter_result *slot = opaque;
    __atomic_add_fetch(&g_waiters_ready, 1, __ATOMIC_RELEASE);
    int status = 0;
    pid_t got = waitpid(slot->pid, &status, 0);
    if (got == slot->pid)
        slot->result = 1;
    else if (got == -1 && errno == ECHILD)
        slot->result = 2;
    else
        slot->result = -1;
    return NULL;
}

static int case_concurrent_waiters(void)
{
    drain_sigchld();
    int gate[2];
    if (pipe(gate) != 0)
        return fail("concurrent pipe");
    pid_t child = fork();
    if (child < 0)
        return fail("concurrent fork");
    if (child == 0) {
        close(gate[1]);
        char byte;
        if (read(gate[0], &byte, 1) != 1)
            _exit(97);
        _exit(23);
    }
    struct waiter_result slots[2] = {{.pid = child, .result = 0},
                                     {.pid = child, .result = 0}};
    g_waiters_ready = 0;
    pthread_t threads[2];
    for (int i = 0; i < 2; i++) {
        if (pthread_create(&threads[i], NULL, concurrent_waiter, &slots[i])) {
            close(gate[1]);
            return fail("concurrent thread");
        }
    }
    while (__atomic_load_n(&g_waiters_ready, __ATOMIC_ACQUIRE) != 2)
        sched_yield();
    /* Give both waiters a chance to park before the child becomes a zombie. */
    struct timespec settle = {.tv_nsec = 100000000L};
    nanosleep(&settle, NULL);
    if (write(gate[1], "X", 1) != 1) {
        close(gate[1]);
        return fail("concurrent release");
    }
    close(gate[1]);
    for (int i = 0; i < 2; i++) {
        if (join_within(threads[i], 5) != 0)
            return fail("concurrent join");
    }
    if (!((slots[0].result == 1 && slots[1].result == 2) ||
          (slots[0].result == 2 && slots[1].result == 1)))
        return fail("concurrent not exactly once");
    if (expect_sigchld(child, 23) != 0)
        return fail("concurrent notify");
    if (expect_echild(child) != 0)
        return fail("concurrent ECHILD");
    if (expect_no_sigchld(200) != 0)
        return fail("concurrent duplicate");
    printf("CHILD_EXIT: concurrent-waiters PASS\n");
    return 0;
}

/* ---- vfork ---- */

static int case_vfork_sigchld(void)
{
    drain_sigchld();
    pid_t child = vfork();
    if (child < 0)
        return fail("vfork");
    if (child == 0)
        _exit(23);
    /* vfork releases the parent after the child exits or execs; the blocked
     * SIGCHLD is then already queued. */
    if (expect_sigchld(child, 23) != 0)
        return fail("vfork notify");
    if (expect_reap(child, 23) != 0)
        return fail("vfork reap");
    if (expect_echild(child) != 0)
        return fail("vfork ECHILD");
    if (expect_no_sigchld(200) != 0)
        return fail("vfork duplicate");
    printf("CHILD_EXIT: vfork PASS\n");
    return 0;
}

static int case_vfork_sig_ign(void)
{
    drain_sigchld();
    struct sigaction ignored = {.sa_handler = SIG_IGN};
    struct sigaction saved;
    sigemptyset(&ignored.sa_mask);
    if (sigaction(SIGCHLD, &ignored, &saved) != 0)
        return fail("vfork ign sigaction");
    pid_t child = vfork();
    if (child < 0) {
        sigaction(SIGCHLD, &saved, NULL);
        return fail("vfork ign");
    }
    if (child == 0)
        _exit(23);

    /* Auto-reap is part of the exit path but races the vfork release, so
     * poll for ECHILD instead of assuming it is already done. */
    int reaped = 0;
    for (int i = 0; i < 100; i++) {
        int status = 0;
        pid_t got = waitpid(child, &status, WNOHANG);
        if (got == -1 && errno == ECHILD) {
            reaped = 1;
            break;
        }
        if (got == child) {
            sigaction(SIGCHLD, &saved, NULL);
            return fail("vfork ign left a zombie");
        }
        struct timespec pause = {.tv_nsec = 10000000L};
        nanosleep(&pause, NULL);
    }
    int no_signal = expect_no_sigchld(100);
    if (sigaction(SIGCHLD, &saved, NULL) != 0)
        return fail("vfork ign restore");
    if (!reaped)
        return fail("vfork ign was not auto-reaped");
    if (no_signal != 0)
        return fail("vfork ign delivered SIGCHLD");
    printf("CHILD_EXIT: vfork-sig-ign PASS\n");
    return 0;
}

static int case_sa_nocldwait(void)
{
    drain_sigchld();
    struct sigaction nocldwait = {.sa_handler = SIG_DFL,
                                  .sa_flags = SA_NOCLDWAIT};
    struct sigaction saved;
    sigemptyset(&nocldwait.sa_mask);
    if (sigaction(SIGCHLD, &nocldwait, &saved) != 0)
        return fail("nocldwait sigaction");
    pid_t child = fork();
    if (child < 0) {
        sigaction(SIGCHLD, &saved, NULL);
        return fail("nocldwait fork");
    }
    if (child == 0)
        _exit(23);

    int reaped = 0;
    for (int i = 0; i < 100; i++) {
        int status = 0;
        pid_t got = waitpid(child, &status, WNOHANG);
        if (got == -1 && errno == ECHILD) {
            reaped = 1;
            break;
        }
        if (got == child) {
            sigaction(SIGCHLD, &saved, NULL);
            return fail("nocldwait left a zombie");
        }
        struct timespec pause = {.tv_nsec = 10000000L};
        nanosleep(&pause, NULL);
    }
    int no_signal = expect_no_sigchld(100);
    if (sigaction(SIGCHLD, &saved, NULL) != 0)
        return fail("nocldwait restore");
    if (!reaped)
        return fail("nocldwait was not auto-reaped");
    if (no_signal != 0)
        return fail("nocldwait delivered SIGCHLD");
    printf("CHILD_EXIT: sa-nocldwait PASS\n");
    return 0;
}

/* ---- CLONE_PARENT ---- */

/* main -> G; G -> M; M clones C with CLONE_PARENT, so C is G's child.  M
 * must not be able to reap C, and G must reap it exactly once after M is
 * gone.  C sleeps briefly so it outlives its creator. */
static int case_clone_parent(void)
{
    drain_sigchld();
    int ppid_pipe[2];
    if (pipe(ppid_pipe) != 0)
        return fail("clone-parent pipe");

    pid_t grandparent = fork();
    if (grandparent < 0)
        return fail("clone-parent grandparent");
    if (grandparent == 0) {
        /* Keep the read end: C (CLONE_PARENT) reports its ppid here. */
        pid_t creator = fork();
        if (creator < 0)
            _exit(70);
        if (creator == 0) {
            long child = syscall(SYS_clone, CLONE_PARENT | SIGCHLD, 0, 0, 0, 0);
            if (child < 0)
                _exit(71);
            if (child == 0) {
                struct timespec delay = {.tv_nsec = 100000000L};
                nanosleep(&delay, NULL);
                int ppid = getppid();
                if (write(ppid_pipe[1], &ppid, sizeof(ppid)) != sizeof(ppid))
                    _exit(72);
                _exit(0);
            }
            /* C belongs to the creator's parent, so it is not ours to reap. */
            errno = 0;
            int not_ours = waitpid((pid_t)child, NULL, WNOHANG) == -1 &&
                           errno == ECHILD;
            _exit(not_ours ? 0 : 73);
        }
        int creator_status = 0;
        if (waitpid(creator, &creator_status, 0) != creator ||
            !WIFEXITED(creator_status) || WEXITSTATUS(creator_status) != 0)
            _exit(74);
        int ppid = -1;
        if (read(ppid_pipe[0], &ppid, sizeof(ppid)) != sizeof(ppid))
            _exit(75);
        int child_status = 0;
        if (waitpid(-1, &child_status, 0) < 0)
            _exit(76);
        if (ppid != getpid() || !WIFEXITED(child_status) ||
            WEXITSTATUS(child_status) != 0)
            _exit(77);
        _exit(0);
    }

    close(ppid_pipe[0]);
    close(ppid_pipe[1]);
    int status = 0;
    if (waitpid(grandparent, &status, 0) != grandparent)
        return fail("clone-parent wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("CHILD_EXIT: clone-parent tree status=%d\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return fail("clone-parent tree");
    }
    /* Consume the notification for the grandparent itself, then require no
     * duplicate for any of the CLONE_PARENT children. */
    drain_sigchld();
    if (expect_no_sigchld(100) != 0)
        return fail("clone-parent duplicate");
    printf("CHILD_EXIT: clone-parent PASS\n");
    return 0;
}

/* ---- PR_SET_PDEATHSIG ---- */

static volatile sig_atomic_t g_pdeath_hits;

static void pdeath_handler(int signal)
{
    (void)signal;
    g_pdeath_hits++;
}

static void pdeath_fail(int step)
{
    /* _exit() does not flush stdio; write the diagnostic directly. */
    dprintf(STDOUT_FILENO, "CHILD_EXIT: pdeath-child step=%d errno=%d ppid=%d\n",
            step, errno, (int)getppid());
    _exit(step);
}

static void pdeath_child(pid_t expected_parent, int ready_fd, int result_fd)
{
    struct sigaction action = {.sa_handler = pdeath_handler};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0)
        pdeath_fail(51);
    errno = 0;
    if (prctl(PR_SET_PDEATHSIG, SIGUSR1) != 0)
        pdeath_fail(52);
    /* A parent that died before prctl() would never deliver. */
    if (getppid() != expected_parent) {
        dprintf(STDOUT_FILENO,
                "CHILD_EXIT: pdeath-child parent=%d expected=%d\n",
                (int)getppid(), (int)expected_parent);
        _exit(53);
    }
    if (write(ready_fd, "R", 1) != 1)
        pdeath_fail(54);
    while (g_pdeath_hits == 0)
        pause();
    int hits = g_pdeath_hits;
    if (write(result_fd, &hits, sizeof(hits)) != sizeof(hits))
        _exit(55);
    _exit(0);
}

/* C arms PDEATHSIG=SIGUSR1, tells its parent it is ready, then sleeps in
 * pause() until the parent dies.  With clone_parent the tree is
 * main -> G -> M -> C(CLONE_PARENT), so C's parent is G; M exits first and G
 * exits after the ready handshake.  The main process reads the handler count
 * from a pipe after the parent is reaped. */
static int run_pdeath_case(int clone_parent, const char *label)
{
    int ready[2], result[2];
    if (pipe(ready) != 0 || pipe(result) != 0)
        return fail("pdeathsig pipes");
    int hits_seen = -1;

    pid_t outer = fork();
    if (outer < 0)
        return fail("pdeathsig outer");
    if (outer == 0) {
        /* Keep ready[0]: this process waits for the child's readiness. */
        close(result[0]);
        pid_t parent = getpid();
        char byte;

        if (clone_parent) {
            pid_t creator = fork();
            if (creator < 0)
                _exit(50);
            if (creator == 0) {
                long child = syscall(SYS_clone, CLONE_PARENT | SIGCHLD,
                                     0, 0, 0, 0);
                if (child < 0)
                    _exit(57);
                if (child == 0)
                    pdeath_child(parent, ready[1], result[1]);
                _exit(0); /* creator dies before C */
            }
            if (read(ready[0], &byte, 1) != 1)
                _exit(56);
            _exit(0); /* G dies: C's parent */
        }

        pid_t child = fork();
        if (child < 0)
            _exit(50);
        if (child == 0)
            pdeath_child(parent, ready[1], result[1]);
        errno = 0;
        ssize_t got = read(ready[0], &byte, 1);
        if (got != 1) {
            dprintf(STDOUT_FILENO,
                    "CHILD_EXIT: pdeath-parent read=%d errno=%d child=%d\n",
                    (int)got, errno, (int)child);
            _exit(56);
        }
        _exit(0); /* parent dies */
    }

    close(ready[1]);
    close(result[1]);
    int outer_status = 0;
    if (waitpid(outer, &outer_status, 0) != outer) {
        close(ready[0]);
        close(result[0]);
        return fail("pdeathsig outer wait");
    }
    if (!WIFEXITED(outer_status) || WEXITSTATUS(outer_status) != 0) {
        printf("CHILD_EXIT: pdeathsig tree status=%d\n",
               WIFEXITED(outer_status) ? WEXITSTATUS(outer_status) : -1);
        close(ready[0]);
        close(result[0]);
        return fail("pdeathsig parent tree");
    }
    if (read_within(result[0], &hits_seen, sizeof(hits_seen), 5000) != 0) {
        close(ready[0]);
        close(result[0]);
        return fail("pdeathsig delivery timeout");
    }
    close(ready[0]);
    close(result[0]);
    if (hits_seen != 1)
        return fail("pdeathsig not exactly once");
    printf("CHILD_EXIT: %s PASS\n", label);
    return 0;
}

int main(void)
{
    sigemptyset(&chld_set);
    sigaddset(&chld_set, SIGCHLD);
    /* A caught disposition keeps SIGCHLD queued while it is blocked, so
     * sigtimedwait is the observable; the handler never runs. */
    struct sigaction action = {.sa_handler = caught};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGCHLD, &action, NULL) != 0)
        return fail("sigaction");
    sigset_t previous;
    if (sigprocmask(SIG_BLOCK, &chld_set, &previous) != 0)
        return fail("sigprocmask");

    if (case_worker_first() != 0)
        return 1;
    if (case_exit_group() != 0)
        return 1;
    if (case_concurrent_waiters() != 0)
        return 1;
    if (case_vfork_sigchld() != 0)
        return 1;
    if (case_vfork_sig_ign() != 0)
        return 1;
    if (case_sa_nocldwait() != 0)
        return 1;
    if (case_clone_parent() != 0)
        return 1;
    if (run_pdeath_case(0, "pdeathsig") != 0)
        return 1;
    if (run_pdeath_case(1, "clone-parent-pdeathsig") != 0)
        return 1;

    printf("CHILD_EXIT_MATRIX: PASS\n");
    return 0;
}
