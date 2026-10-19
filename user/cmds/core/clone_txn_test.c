/*
 * Guest coverage for the fork/clone transaction decisions that a plain
 * fork/exec workload never reaches: both CLONE_*_SETTID slots, CLONE_PARENT
 * reparenting, and the out-of-range exit_signal rejection.  Each check prints
 * one marker so a C and a Rust kernel can be compared on the same trace.
 *
 * CLONE_CHILD_CLEARTID is deliberately not exercised here: observing the clear
 * needs a raw clone whose child resumes on a switched stack, and that pattern
 * already faults on the unmodified C kernel (SIGSEGV at pc=0 in the child), so
 * it would compare a pre-existing limitation instead of this transaction.  The
 * recorded slot and the exit-time clear are covered by the host oracle and by
 * the pthread workload in proc_stress, which musl builds on that flag.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define STACK_SIZE (64 * 1024)
#define CLONE_TXN_BAD_SIGNAL 100 /* exit_signal = flags & 0xFF, above _NSIG */
#define CLONE_TXN_SENTINEL 0x5a5a5a5a

static int fail(const char *what)
{
    printf("CLONE_TXN: FAIL %s errno=%d\n", what, errno);
    return 1;
}

static void *stack_alloc(void)
{
    void *stack = mmap(NULL, STACK_SIZE, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return stack == MAP_FAILED ? NULL : stack;
}

static int child_exit_zero(void *arg)
{
    (void)arg;
    return 0;
}

/* CLONE_PARENT_SETTID writes the child id into the parent's slot before the
 * parent returns.  CLONE_CHILD_SETTID writes into the child's own address
 * space, so it is only observable from the parent when the clone shares the
 * address space.  Both variants are checked. */
static int test_settid(void)
{
    void *stack = stack_alloc();
    if (!stack)
        return fail("settid-stack");

    int ptid = 0;
    int ctid = 0;
    pid_t pid = clone(child_exit_zero, (char *)stack + STACK_SIZE,
                      CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | SIGCHLD,
                      NULL, &ptid, (void *)0, &ctid);
    if (pid < 0)
        return fail("settid-clone");
    if (ptid != pid)
        return fail("settid-parent-slot");
    if (ctid != 0)
        return fail("settid-copy-leaked");

    int status = 0;
    if (waitpid(pid, &status, 0) != pid)
        return fail("settid-wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("settid-status");
    printf("CLONE_TXN: settid-copy PASS pid=%d\n", pid);

    ptid = 0;
    ctid = 0;
    pid = clone(child_exit_zero, (char *)stack + STACK_SIZE,
                CLONE_VM | CLONE_PARENT_SETTID | CLONE_CHILD_SETTID | SIGCHLD,
                NULL, &ptid, (void *)0, &ctid);
    if (pid < 0)
        return fail("settid-shared-clone");
    if (ptid != pid)
        return fail("settid-shared-parent-slot");
    if (ctid != pid)
        return fail("settid-shared-child-slot");

    if (waitpid(pid, &status, 0) != pid)
        return fail("settid-shared-wait");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("settid-shared-status");
    printf("CLONE_TXN: settid-shared PASS pid=%d\n", pid);

    munmap(stack, STACK_SIZE);
    return 0;
}

static int child_report_ppid(void *arg)
{
    int fd = *(int *)arg;
    pid_t ppid = getppid();
    ssize_t written = write(fd, &ppid, sizeof(ppid));
    return written == (ssize_t)sizeof(ppid) ? 0 : 1;
}

/* CLONE_PARENT gives the child the caller's parent, so the child reports the
 * grandparent's id, not the caller's.  The child is not ours to reap. */
static int test_clone_parent(void)
{
    int fds[2];
    if (pipe(fds) < 0)
        return fail("parent-pipe");

    void *stack = stack_alloc();
    if (!stack)
        return fail("parent-stack");

    pid_t grandparent = getppid();
    pid_t pid = clone(child_report_ppid, (char *)stack + STACK_SIZE,
                      CLONE_PARENT | SIGCHLD, &fds[1]);
    if (pid < 0)
        return fail("parent-clone");

    pid_t reported = 0;
    ssize_t got = read(fds[0], &reported, sizeof(reported));
    if (got != (ssize_t)sizeof(reported))
        return fail("parent-read");
    if (reported != grandparent)
        return fail("parent-reparent");

    close(fds[0]);
    close(fds[1]);
    munmap(stack, STACK_SIZE);
    printf("CLONE_TXN: clone-parent PASS child=%d ppid=%d\n", pid, reported);
    return 0;
}

/* The exit signal is the low byte of the flags word; a value at or above
 * _NSIG must be rejected before any resource is acquired. */
static int test_bad_signal(void)
{
    errno = 0;
    long ret = syscall(SYS_clone, (long)CLONE_TXN_BAD_SIGNAL, 0, 0, 0, 0);
    if (ret != -1)
        return fail("bad-signal-accepted");
    if (errno != EINVAL)
        return fail("bad-signal-errno");
    printf("CLONE_TXN: bad-signal PASS\n");
    return 0;
}

int main(void)
{
    printf("CLONE_TXN: start\n");
    if (test_settid())
        return 1;
    if (test_clone_parent())
        return 1;
    if (test_bad_signal())
        return 1;
    printf("CLONE_TXN: PASS\n");
    return 0;
}
