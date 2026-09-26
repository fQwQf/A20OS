/*
 * poll_edge — LTP-style edge-semantics tests for the Linux ABI poll area:
 * poll/ppoll/select/pselect6 timeout and validation boundaries, POLLNVAL /
 * POLLHUP reporting rules, and epoll LT/ET/ONESHOT/dup/nesting semantics.
 *
 * Each test returns 0 on success; any deviation prints POLL_EDGE: FAIL.
 */
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef SYS_epoll_pwait2
#define SYS_epoll_pwait2 441
#endif

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static int fail(const char *what, long got)
{
    printf("POLL_EDGE: FAIL %s (got=%ld errno=%d)\n", what, got, errno);
    return -1;
}

/* poll timeout boundaries: 0 = poll-only, short = bounded wait,
 * -1 = wait until an event. */
static int test_poll_timeouts(void)
{
    int p[2];
    if (pipe(p) < 0)
        return fail("pipe", -1);
    struct pollfd pfd = { .fd = p[0], .events = POLLIN };

    uint64_t t0 = now_ms();
    if (poll(&pfd, 1, 0) != 0)
        return fail("poll timeout=0 result", -1);
    if (now_ms() - t0 > 200)
        return fail("poll timeout=0 not immediate", (long)(now_ms() - t0));

    t0 = now_ms();
    if (poll(&pfd, 1, 60) != 0)
        return fail("poll timeout=60 result", -1);
    uint64_t el = now_ms() - t0;
    if (el < 40 || el > 3000)
        return fail("poll timeout=60 elapsed", (long)el);

    /* poll(-1) must block until the event, not return early/late. */
    pid_t child = fork();
    if (child < 0)
        return fail("fork", -1);
    if (child == 0) {
        usleep(60000);
        _exit(write(p[1], "x", 1) == 1 ? 0 : 1);
    }
    t0 = now_ms();
    int r = poll(&pfd, 1, -1);
    el = now_ms() - t0;
    int status = 0;
    waitpid(child, &status, 0);
    close(p[0]);
    close(p[1]);
    if (r != 1 || !(pfd.revents & POLLIN))
        return fail("poll(-1) event", r);
    if (el < 40 || el > 3000)
        return fail("poll(-1) elapsed", (long)el);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("poll(-1) child", status);
    return 0;
}

/* POLLNVAL on invalid fds, fd<1 ignored entries, and HUP/ERR reported even
 * when not requested in .events. */
static int test_poll_revents(void)
{
    int p[2];
    if (pipe(p) < 0)
        return fail("pipe", -1);

    /* Closed fd -> POLLNVAL. */
    int tmp = open("/tmp/poll_edge_nval.tmp", O_CREAT | O_RDWR, 0600);
    if (tmp < 0)
        return fail("open nval", -1);
    close(tmp);
    unlink("/tmp/poll_edge_nval.tmp");
    struct pollfd pfd = { .fd = tmp, .events = POLLIN };
    if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLNVAL))
        return fail("poll POLLNVAL missing", pfd.revents);

    /* fd == -1 entry is ignored. */
    pfd.fd = -1;
    pfd.revents = 0xff;
    if (poll(&pfd, 1, 0) != 0 || pfd.revents != 0)
        return fail("poll fd=-1 not ignored", pfd.revents);

    /* Peer close -> POLLHUP even though only POLLIN was requested. */
    close(p[1]);
    pfd.fd = p[0];
    pfd.events = POLLIN;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLHUP))
        return fail("poll POLLHUP missing", pfd.revents);

    /* HUP/ERR are reported even with events == 0. */
    pfd.events = 0;
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 1 || !(pfd.revents & POLLHUP))
        return fail("poll HUP with events=0", pfd.revents);
    close(p[0]);
    return 0;
}

/* select readiness result must leave only ready fds set; bad timeouts are
 * EINVAL. */
static int test_select_semantics(void)
{
    int p[2], q[2];
    if (pipe(p) < 0 || pipe(q) < 0)
        return fail("pipe", -1);
    if (write(p[1], "x", 1) != 1)
        return fail("pipe write", -1);

    fd_set rfds;
    FD_ZERO(&rfds);
    FD_SET(p[0], &rfds);
    FD_SET(q[0], &rfds);
    struct timeval tv = { .tv_sec = 0, .tv_usec = 0 };
    int nfds = (p[0] > q[0] ? p[0] : q[0]) + 1;
    if (select(nfds, &rfds, NULL, NULL, &tv) != 1)
        return fail("select ready count", -1);
    if (!FD_ISSET(p[0], &rfds))
        return fail("select ready fd cleared", -1);
    if (FD_ISSET(q[0], &rfds))
        return fail("select unready fd left set", -1);

    /* musl select() rejects negative times itself; the kernel side is
     * exercised through raw pselect6 below. */
    struct timeval bad = { .tv_sec = -1, .tv_usec = 0 };
    if (select(0, NULL, NULL, NULL, &bad) != -1 || errno != EINVAL)
        return fail("select negative tv_sec", errno);

    struct timespec nts = { .tv_sec = -1, .tv_nsec = 0 };
    long r = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &nts, NULL);
    if (r != -1 || errno != EINVAL)
        return fail("pselect6 negative tv_sec", r);
    nts.tv_sec = 0;
    nts.tv_nsec = 1000000000L;
    r = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &nts, NULL);
    if (r != -1 || errno != EINVAL)
        return fail("pselect6 tv_nsec=1e9", r);

    /* pselect6 sigmask blob with a wrong size is EINVAL. */
    sigset_t mask;
    sigemptyset(&mask);
    struct { void *ss; size_t ss_len; } blob = { &mask, 4 };
    struct timespec zts = { .tv_sec = 0, .tv_nsec = 0 };
    r = syscall(SYS_pselect6, 0, NULL, NULL, NULL, &zts, &blob);
    if (r != -1 || errno != EINVAL)
        return fail("pselect6 sigsetsize", r);

    r = syscall(SYS_ppoll, NULL, 0, &nts, &mask, 8);
    if (r != -1 || errno != EINVAL)
        return fail("ppoll negative tv_sec", r);
    r = syscall(SYS_ppoll, NULL, 0, &zts, &mask, 4);
    if (r != -1 || errno != EINVAL)
        return fail("ppoll sigsetsize", r);

    close(p[0]); close(p[1]); close(q[0]); close(q[1]);
    return 0;
}

/* ppoll timespec timeout boundaries. */
static int test_ppoll_timeouts(void)
{
    int p[2];
    if (pipe(p) < 0)
        return fail("pipe", -1);
    struct pollfd pfd = { .fd = p[0], .events = POLLIN };
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 50000000 };

    uint64_t t0 = now_ms();
    if (ppoll(&pfd, 1, &ts, NULL) != 0)
        return fail("ppoll 50ms result", -1);
    uint64_t el = now_ms() - t0;
    if (el < 30 || el > 3000)
        return fail("ppoll 50ms elapsed", (long)el);

    ts.tv_nsec = 0;
    t0 = now_ms();
    if (ppoll(&pfd, 1, &ts, NULL) != 0)
        return fail("ppoll 0 result", -1);
    if (now_ms() - t0 > 200)
        return fail("ppoll 0 not immediate", (long)(now_ms() - t0));

    /* nfds == 0 with a timeout is a plain sleep. */
    ts.tv_nsec = 40000000;
    t0 = now_ms();
    if (ppoll(NULL, 0, &ts, NULL) != 0)
        return fail("ppoll nfds=0 result", -1);
    if (now_ms() - t0 < 20)
        return fail("ppoll nfds=0 did not wait", (long)(now_ms() - t0));

    close(p[0]);
    close(p[1]);
    return 0;
}

static int test_epoll_validation(void)
{
    if (epoll_create(0) != -1 || errno != EINVAL)
        return fail("epoll_create(0)", errno);
    if (epoll_create(-5) != -1 || errno != EINVAL)
        return fail("epoll_create(-5)", errno);
    if (epoll_create1(~EPOLL_CLOEXEC) != -1 || errno != EINVAL)
        return fail("epoll_create1 bad flags", errno);

    int ep = epoll_create1(0);
    int p[2];
    if (ep < 0 || pipe(p) < 0)
        return fail("epoll setup", -1);
    struct epoll_event ev = { .events = EPOLLIN };
    struct epoll_event out;

    /* wait with maxevents <= 0 is EINVAL. */
    if (epoll_wait(ep, &out, 0, 0) != -1 || errno != EINVAL)
        return fail("epoll_wait maxevents=0", errno);

    /* ctl on a non-epoll fd is EINVAL; on a bad target fd is EBADF. */
    if (epoll_ctl(p[0], EPOLL_CTL_ADD, p[1], &ev) != -1 || errno != EINVAL)
        return fail("epoll_ctl non-epoll epfd", errno);
    if (epoll_ctl(ep, EPOLL_CTL_ADD, 9999, &ev) != -1 || errno != EBADF)
        return fail("epoll_ctl bad target", errno);

    /* An epoll fd cannot watch itself. */
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ep, &ev) != -1 || errno != EINVAL)
        return fail("epoll_ctl self", errno);

    /* Directories and regular files are not pollable: EPERM. */
    int dfd = open("/tmp", O_RDONLY | O_DIRECTORY);
    if (dfd >= 0) {
        if (epoll_ctl(ep, EPOLL_CTL_ADD, dfd, &ev) != -1 || errno != EPERM)
            return fail("epoll_ctl directory", errno);
        close(dfd);
    }
    int ffd = open("/tmp/poll_edge_reg.tmp", O_CREAT | O_RDWR, 0600);
    if (ffd < 0)
        return fail("open regular", -1);
    if (epoll_ctl(ep, EPOLL_CTL_ADD, ffd, &ev) != -1 || errno != EPERM)
        return fail("epoll_ctl regular file", errno);
    close(ffd);
    unlink("/tmp/poll_edge_reg.tmp");

    /* Duplicate ADD is EEXIST; MOD/DEL of a missing fd is ENOENT. */
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("epoll_ctl ADD", errno);
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != -1 || errno != EEXIST)
        return fail("epoll_ctl re-ADD", errno);
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[1], &ev) != -1 || errno != ENOENT)
        return fail("epoll_ctl MOD missing", errno);
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[1], NULL) != -1 || errno != ENOENT)
        return fail("epoll_ctl DEL missing", errno);
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) != 0)
        return fail("epoll_ctl DEL", errno);
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) != -1 || errno != ENOENT)
        return fail("epoll_ctl re-DEL", errno);

    close(ep);
    close(p[0]);
    close(p[1]);
    return 0;
}

/* Circular epoll nesting is refused with ELOOP; plain nesting works. */
static int test_epoll_nesting(void)
{
    int ep1 = epoll_create1(0);
    int ep2 = epoll_create1(0);
    if (ep1 < 0 || ep2 < 0)
        return fail("epoll_create1", -1);
    struct epoll_event ev = { .events = EPOLLIN };

    if (epoll_ctl(ep1, EPOLL_CTL_ADD, ep2, &ev) != 0)
        return fail("nested epoll ADD", errno);
    if (epoll_ctl(ep2, EPOLL_CTL_ADD, ep1, &ev) != -1 || errno != ELOOP)
        return fail("epoll cycle not ELOOP", errno);

    /* A readable leaf inside ep2 makes ep1 readable. */
    int p[2];
    if (pipe(p) < 0)
        return fail("pipe", -1);
    if (epoll_ctl(ep2, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("epoll leaf ADD", errno);
    if (write(p[1], "x", 1) != 1)
        return fail("pipe write", -1);
    struct epoll_event out;
    if (epoll_wait(ep1, &out, 1, 500) != 1)
        return fail("nested epoll wait", -1);

    close(p[0]); close(p[1]);
    close(ep1); close(ep2);
    return 0;
}

/* A dup'ed epoll fd shares the interest list with the original. */
static int test_epoll_dup(void)
{
    int ep = epoll_create1(0);
    int p[2];
    if (ep < 0 || pipe(p) < 0)
        return fail("setup", -1);
    int dupfd = dup(ep);
    if (dupfd < 0)
        return fail("dup", -1);

    struct epoll_event ev = { .events = EPOLLIN, .data.u32 = 7 };
    /* Register through the dup; the event must arrive on the original. */
    if (epoll_ctl(dupfd, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("epoll_ctl via dup", errno);
    if (write(p[1], "x", 1) != 1)
        return fail("pipe write", -1);
    struct epoll_event out;
    if (epoll_wait(ep, &out, 1, 500) != 1 || out.data.u32 != 7)
        return fail("epoll_wait shared instance", -1);

    /* Closing the dup keeps the instance alive; closing the original fd
     * number last still allows the dup-side registration to work. */
    close(dupfd);
    char c;
    if (read(p[0], &c, 1) != 1)
        return fail("drain pipe", -1);
    if (epoll_ctl(ep, EPOLL_CTL_DEL, p[0], NULL) != 0)
        return fail("epoll DEL after dup close", errno);

    close(ep);
    close(p[0]);
    close(p[1]);
    return 0;
}

/* ET: one event per readiness edge; LT: event repeats while still ready. */
static int test_epoll_et_lt(void)
{
    int p[2];
    if (pipe(p) < 0)
        return fail("pipe", -1);

    /* --- edge-triggered --- */
    int ep = epoll_create1(0);
    if (ep < 0)
        return fail("epoll_create1", -1);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLET };
    struct epoll_event out;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("ET ADD", errno);
    if (write(p[1], "ab", 2) != 2)
        return fail("ET write", -1);
    if (epoll_wait(ep, &out, 1, 500) != 1 || !(out.events & EPOLLIN))
        return fail("ET initial event", -1);
    /* No new data: no redelivery. */
    if (epoll_wait(ep, &out, 1, 0) != 0)
        return fail("ET redelivered without edge", -1);
    /* Partial drain, still no new edge. */
    char c;
    if (read(p[0], &c, 1) != 1)
        return fail("ET read", -1);
    if (epoll_wait(ep, &out, 1, 0) != 0)
        return fail("ET redelivered after partial read", -1);
    /* New data is a new edge. */
    if (write(p[1], "c", 1) != 1)
        return fail("ET rewrite", -1);
    if (epoll_wait(ep, &out, 1, 500) != 1)
        return fail("ET edge after new write", -1);
    close(ep);

    /* --- level-triggered --- */
    int p2[2];
    if (pipe(p2) < 0)
        return fail("pipe2", -1);
    ep = epoll_create1(0);
    if (ep < 0)
        return fail("epoll_create1 lt", -1);
    ev.events = EPOLLIN;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p2[0], &ev) != 0)
        return fail("LT ADD", errno);
    if (write(p2[1], "ab", 2) != 2)
        return fail("LT write", -1);
    if (epoll_wait(ep, &out, 1, 500) != 1)
        return fail("LT initial event", -1);
    if (read(p2[0], &c, 1) != 1)
        return fail("LT read", -1);
    /* One byte remains: LT must report again. */
    if (epoll_wait(ep, &out, 1, 500) != 1)
        return fail("LT repeat while ready", -1);
    if (read(p2[0], &c, 1) != 1)
        return fail("LT drain", -1);
    if (epoll_wait(ep, &out, 1, 0) != 0)
        return fail("LT event after drain", -1);

    close(ep);
    close(p[0]); close(p[1]);
    close(p2[0]); close(p2[1]);
    return 0;
}

/* EPOLLONESHOT disables the interest until re-armed with MOD. */
static int test_epoll_oneshot(void)
{
    int p[2];
    int ep = epoll_create1(0);
    if (ep < 0 || pipe(p) < 0)
        return fail("setup", -1);
    struct epoll_event ev = { .events = EPOLLIN | EPOLLONESHOT };
    struct epoll_event out;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("ONESHOT ADD", errno);
    if (write(p[1], "ab", 2) != 2)
        return fail("ONESHOT write", -1);
    if (epoll_wait(ep, &out, 1, 500) != 1)
        return fail("ONESHOT first event", -1);
    /* Data remains, but the interest is disabled now. */
    if (epoll_wait(ep, &out, 1, 0) != 0)
        return fail("ONESHOT redelivered", -1);
    /* New data must not re-enable it either. */
    if (write(p[1], "c", 1) != 1)
        return fail("ONESHOT rewrite", -1);
    if (epoll_wait(ep, &out, 1, 0) != 0)
        return fail("ONESHOT re-enabled by write", -1);
    /* MOD re-arms; buffered data reports immediately. */
    if (epoll_ctl(ep, EPOLL_CTL_MOD, p[0], &ev) != 0)
        return fail("ONESHOT rearm", errno);
    if (epoll_wait(ep, &out, 1, 500) != 1)
        return fail("ONESHOT after rearm", -1);

    close(ep);
    close(p[0]);
    close(p[1]);
    return 0;
}

/* EPOLLHUP is delivered even when only EPOLLIN was requested; the data
 * union round-trips. */
static int test_epoll_hup_and_data(void)
{
    int p[2];
    int ep = epoll_create1(0);
    if (ep < 0 || pipe(p) < 0)
        return fail("setup", -1);
    struct epoll_event ev = { .events = EPOLLIN,
                              .data.u64 = 0xdeadbeefcafe1234ULL };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("HUP ADD", errno);
    close(p[1]);
    struct epoll_event out;
    if (epoll_wait(ep, &out, 1, 500) != 1)
        return fail("HUP wait", -1);
    if (!(out.events & EPOLLHUP))
        return fail("EPOLLHUP missing", out.events);
    if (out.data.u64 != 0xdeadbeefcafe1234ULL)
        return fail("epoll data roundtrip", (long)out.data.u64);

    close(ep);
    close(p[0]);
    return 0;
}

/* epoll_pwait2 takes a timespec timeout; sub-ms timeouts round up. */
static int test_epoll_pwait2(void)
{
    int p[2];
    int ep = epoll_create1(0);
    if (ep < 0 || pipe(p) < 0)
        return fail("setup", -1);
    struct epoll_event ev = { .events = EPOLLIN };
    if (epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &ev) != 0)
        return fail("pwait2 ADD", errno);
    struct epoll_event out;
    struct timespec ts = { .tv_sec = 0, .tv_nsec = 40000000 };

    uint64_t t0 = now_ms();
    long r = syscall(SYS_epoll_pwait2, ep, &out, 1, &ts, NULL, 0);
    uint64_t el = now_ms() - t0;
    if (r != 0)
        return fail("pwait2 40ms result", r);
    if (el < 20 || el > 3000)
        return fail("pwait2 40ms elapsed", (long)el);

    /* 500us must not be treated as a zero timeout. */
    ts.tv_nsec = 500000;
    t0 = now_ms();
    r = syscall(SYS_epoll_pwait2, ep, &out, 1, &ts, NULL, 0);
    el = now_ms() - t0;
    if (r != 0)
        return fail("pwait2 500us result", r);
    if (el > 1000)
        return fail("pwait2 500us overshoot", (long)el);

    /* Zero timeout is a pure poll. */
    ts.tv_nsec = 0;
    t0 = now_ms();
    r = syscall(SYS_epoll_pwait2, ep, &out, 1, &ts, NULL, 0);
    if (r != 0 || now_ms() - t0 > 200)
        return fail("pwait2 zero timeout", r);

    /* Bad timespec is EINVAL. */
    ts.tv_nsec = 1000000000;
    r = syscall(SYS_epoll_pwait2, ep, &out, 1, &ts, NULL, 0);
    if (r != -1 || errno != EINVAL)
        return fail("pwait2 bad nsec", r);

    /* Ready fd reports immediately regardless of timeout. */
    if (write(p[1], "x", 1) != 1)
        return fail("pwait2 write", -1);
    ts.tv_sec = 5;
    ts.tv_nsec = 0;
    r = syscall(SYS_epoll_pwait2, ep, &out, 1, &ts, NULL, 0);
    if (r != 1 || !(out.events & EPOLLIN))
        return fail("pwait2 ready event", r);

    close(ep);
    close(p[0]);
    close(p[1]);
    return 0;
}

/* eventfd counter semantics: initial value, coalescing reads, semaphore
 * mode decrementing by 1, overflow and empty reads in nonblocking mode. */
static int test_eventfd_edges(void)
{
    uint64_t v;
    int fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0)
        return fail("eventfd create", -1);
    if (read(fd, &v, sizeof(v)) != -1 || errno != EAGAIN)
        return fail("eventfd empty read", errno);
    v = 3;
    if (write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
        return fail("eventfd write", -1);
    v = 0;
    if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v) || v != 3)
        return fail("eventfd coalesced read", (long)v);
    if (read(fd, &v, sizeof(v)) != -1 || errno != EAGAIN)
        return fail("eventfd drained read", errno);
    close(fd);

    /* Initial value is honored. */
    fd = eventfd(5, EFD_NONBLOCK);
    if (fd < 0)
        return fail("eventfd initval", -1);
    v = 0;
    if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v) || v != 5)
        return fail("eventfd initial value", (long)v);
    close(fd);

    /* Semaphore mode decrements by one per read. */
    fd = eventfd(0, EFD_NONBLOCK | EFD_SEMAPHORE);
    if (fd < 0)
        return fail("eventfd semaphore", -1);
    v = 3;
    if (write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
        return fail("eventfd sem write", -1);
    for (int i = 0; i < 3; i++) {
        v = 0;
        if (read(fd, &v, sizeof(v)) != (ssize_t)sizeof(v) || v != 1)
            return fail("eventfd sem read", (long)v);
    }
    if (read(fd, &v, sizeof(v)) != -1 || errno != EAGAIN)
        return fail("eventfd sem drained", errno);
    close(fd);

    /* Writes that would overflow the counter fail with EAGAIN. */
    fd = eventfd(0, EFD_NONBLOCK);
    if (fd < 0)
        return fail("eventfd overflow create", -1);
    v = UINT64_MAX - 1;
    if (write(fd, &v, sizeof(v)) != (ssize_t)sizeof(v))
        return fail("eventfd near-max write", -1);
    v = 1;
    if (write(fd, &v, sizeof(v)) != -1 || errno != EAGAIN)
        return fail("eventfd overflow write", errno);
    close(fd);
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "poll_timeouts", test_poll_timeouts },
        { "poll_revents", test_poll_revents },
        { "select_semantics", test_select_semantics },
        { "ppoll_timeouts", test_ppoll_timeouts },
        { "epoll_validation", test_epoll_validation },
        { "epoll_nesting", test_epoll_nesting },
        { "epoll_dup", test_epoll_dup },
        { "epoll_et_lt", test_epoll_et_lt },
        { "epoll_oneshot", test_epoll_oneshot },
        { "epoll_hup_and_data", test_epoll_hup_and_data },
        { "epoll_pwait2", test_epoll_pwait2 },
        { "eventfd_edges", test_eventfd_edges },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i].fn() < 0) {
            printf("POLL_EDGE: FAIL in %s\n", tests[i].name);
            return 1;
        }
        printf("POLL_EDGE: %s ok\n", tests[i].name);
    }
    printf("POLL_EDGE: PASS\n");
    return 0;
}
