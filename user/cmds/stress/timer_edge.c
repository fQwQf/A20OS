/*
 * timer_edge — LTP-style edge-semantics tests for the Linux ABI timer area:
 * POSIX timer_create/settime/gettime/getoverrun/delete (clockids, SIGEV
 * variants, ABSTIME, disarm, overrun counting), timerfd (ABSTIME,
 * CANCEL_ON_SET, expiration counting, validation), itimer REAL/VIRTUAL/PROF
 * delivery with interval reload, and clock_nanosleep ABSTIME/EINTR+remain.
 *
 * Raw syscalls are used for the POSIX timer surface so libc wrappers cannot
 * hide kernel behavior.  Any deviation prints TIMER_EDGE: FAIL.
 */
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ARM EABI exposes time64 timer syscalls under explicit names.  musl uses a
 * 64-bit time_t on this 32-bit target, so call the matching raw ABI directly. */
#if defined(__arm__) && !defined(__aarch64__)
#ifndef SYS_timer_settime64
#error "ARM EABI headers must expose timer_settime64"
#endif
#ifndef SYS_timer_gettime64
#error "ARM EABI headers must expose timer_gettime64"
#endif
_Static_assert(sizeof(time_t) == sizeof(int64_t),
               "ARM timer64 syscall requires 64-bit time_t layout");
#define A20_SYS_timer_settime SYS_timer_settime64
#define A20_SYS_timer_gettime SYS_timer_gettime64
#else
#define A20_SYS_timer_settime SYS_timer_settime
#define A20_SYS_timer_gettime SYS_timer_gettime
#endif

/* Linux clockids */
#define CLK_REALTIME  0
#define CLK_MONOTONIC 1
#define CLK_PROCESS_CPUTIME 2
#define CLK_THREAD_CPUTIME  3
#define CLK_MONOTONIC_RAW 4
#define CLK_REALTIME_COARSE 5
#define CLK_MONOTONIC_COARSE 6
#define CLK_BOOTTIME  7
#define CLK_TAI       11

#define SIGEV_SIGNAL     0
#define SIGEV_NONE       1
#define SIGEV_THREAD     2
#define SIGEV_THREAD_ID  4

#define TIMER_ABSTIME 1

#define TFD_TIMER_ABSTIME 1
#ifndef TFD_TIMER_CANCEL_ON_SET
#define TFD_TIMER_CANCEL_ON_SET 2
#endif

/* Linux ABI struct sigevent wire layout (uapi/asm-generic/siginfo.h). */
typedef struct {
    uint64_t sigev_value;
    int32_t  sigev_signo;
    int32_t  sigev_notify;
    union {
        int32_t pad[12];
        int32_t tid;
    } un;
} wire_sigevent_t;

static long xtimer_create(int clockid, wire_sigevent_t *sev, int *timerid)
{
    return syscall(SYS_timer_create, clockid, sev, timerid);
}

static long xtimer_settime(int timerid, int flags,
                           const struct itimerspec *newv,
                           struct itimerspec *oldv)
{
    return syscall(A20_SYS_timer_settime, timerid, flags, newv, oldv);
}

static long xtimer_gettime(int timerid, struct itimerspec *cur)
{
    return syscall(A20_SYS_timer_gettime, timerid, cur);
}

static long xtimer_getoverrun(int timerid)
{
    return syscall(SYS_timer_getoverrun, timerid);
}

static long xtimer_delete(int timerid)
{
    return syscall(SYS_timer_delete, timerid);
}

static volatile sig_atomic_t g_sigusr1_count;
static volatile sig_atomic_t g_sigalrm_count;
static volatile sig_atomic_t g_sigvtalrm_count;
static volatile sig_atomic_t g_sigprof_count;

static void on_sigusr1(int sig) { (void)sig; g_sigusr1_count++; }
static void on_sigalrm(int sig) { (void)sig; g_sigalrm_count++; }
static void on_sigvtalrm(int sig) { (void)sig; g_sigvtalrm_count++; }
static void on_sigprof(int sig) { (void)sig; g_sigprof_count++; }

static void install_handler(int signo, void (*fn)(int))
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = fn;
    sa.sa_flags = 0; /* no SA_RESTART: sleeps must report EINTR */
    sigaction(signo, &sa, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static void ms_to_ts(uint64_t ms, struct timespec *ts)
{
    ts->tv_sec = (time_t)(ms / 1000);
    ts->tv_nsec = (long)((ms % 1000) * 1000000);
}

static void ms_to_tv(uint64_t ms, struct timeval *tv)
{
    tv->tv_sec = (time_t)(ms / 1000);
    tv->tv_usec = (long)((ms % 1000) * 1000);
}

static void sleep_ms(uint64_t ms)
{
    struct timespec ts;
    ms_to_ts(ms, &ts);
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR)
        ;
}

static int fail(const char *what, long got)
{
    printf("TIMER_EDGE: FAIL %s (got=%ld errno=%d)\n", what, got, errno);
    return -1;
}

/* timer_create accepts the realtime/monotonic clock families and refuses
 * unknown clockids. */
static int test_timer_create_clocks(void)
{
    static const int ok_clocks[] = {
        CLK_REALTIME, CLK_MONOTONIC, CLK_BOOTTIME, CLK_MONOTONIC_RAW,
        CLK_REALTIME_COARSE, CLK_MONOTONIC_COARSE, CLK_TAI,
    };
    for (size_t i = 0; i < sizeof(ok_clocks) / sizeof(ok_clocks[0]); i++) {
        int tid = -1;
        if (xtimer_create(ok_clocks[i], NULL, &tid) != 0)
            return fail("timer_create supported clock", ok_clocks[i]);
        if (xtimer_delete(tid) != 0)
            return fail("timer_delete", tid);
    }
    int tid;
    if (xtimer_create(42, NULL, &tid) != -1 || errno != EINVAL)
        return fail("timer_create bogus clock", errno);
    return 0;
}

/* SIGEV delivery modes: SIGNAL custom signo, NONE, THREAD_ID; THREAD and
 * unknown notify values are refused; out-of-range signo is EINVAL. */
static int test_timer_create_sigev(void)
{
    install_handler(SIGUSR1, on_sigusr1);

    /* SIGEV_SIGNAL with a custom signo. */
    wire_sigevent_t sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    int tid;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != 0)
        return fail("create SIGEV_SIGNAL", errno);
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    ms_to_ts(30, &its.it_value);
    g_sigusr1_count = 0;
    if (xtimer_settime(tid, 0, &its, NULL) != 0)
        return fail("arm SIGEV_SIGNAL", errno);
    sleep_ms(120);
    if (g_sigusr1_count != 1)
        return fail("SIGEV_SIGNAL delivery", g_sigusr1_count);
    xtimer_delete(tid);

    /* SIGEV_NONE: expiry is tracked, nothing is delivered. */
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_NONE;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != 0)
        return fail("create SIGEV_NONE", errno);
    if (xtimer_settime(tid, 0, &its, NULL) != 0)
        return fail("arm SIGEV_NONE", errno);
    g_sigusr1_count = 0;
    sleep_ms(100);
    struct itimerspec cur;
    if (xtimer_gettime(tid, &cur) != 0)
        return fail("gettime SIGEV_NONE", errno);
    if (cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0)
        return fail("SIGEV_NONE one-shot still armed",
                    (long)cur.it_value.tv_nsec);
    if (g_sigusr1_count != 0)
        return fail("SIGEV_NONE delivered a signal", g_sigusr1_count);
    xtimer_delete(tid);

    /* SIGEV_THREAD_ID targeting the calling thread. */
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGUSR1;
    sev.un.tid = (int32_t)syscall(SYS_gettid);
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != 0)
        return fail("create SIGEV_THREAD_ID", errno);
    if (xtimer_settime(tid, 0, &its, NULL) != 0)
        return fail("arm SIGEV_THREAD_ID", errno);
    g_sigusr1_count = 0;
    sleep_ms(100);
    if (g_sigusr1_count != 1)
        return fail("SIGEV_THREAD_ID delivery", g_sigusr1_count);
    xtimer_delete(tid);

    /* Refused inputs. */
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_signo = SIGUSR1;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != -1 || errno != EINVAL)
        return fail("SIGEV_THREAD not refused", errno);
    sev.sigev_notify = 3;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != -1 || errno != EINVAL)
        return fail("notify=3 not refused", errno);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = 0;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != -1 || errno != EINVAL)
        return fail("signo=0 not refused", errno);
    sev.sigev_signo = 65;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != -1 || errno != EINVAL)
        return fail("signo=65 not refused", errno);
    sev.sigev_notify = SIGEV_THREAD_ID;
    sev.sigev_signo = SIGUSR1;
    sev.un.tid = -1;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != -1 || errno != EINVAL)
        return fail("THREAD_ID bad tid", errno);
    return 0;
}

/* gettime reports remaining time and the interval; disarm reads back zero;
 * malformed settime arguments are EINVAL. */
static int test_timer_settime_gettime(void)
{
    int tid;
    if (xtimer_create(CLK_MONOTONIC, NULL, &tid) != 0)
        return fail("create", errno);

    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    ms_to_ts(300, &its.it_value);
    ms_to_ts(50, &its.it_interval);
    if (xtimer_settime(tid, 0, &its, NULL) != 0)
        return fail("arm periodic", errno);

    struct itimerspec cur;
    if (xtimer_gettime(tid, &cur) != 0)
        return fail("gettime", errno);
    if (cur.it_interval.tv_sec != 0 || cur.it_interval.tv_nsec != 50000000L)
        return fail("interval echo", (long)cur.it_interval.tv_nsec);
    long first_ns = cur.it_value.tv_sec * 1000000000L + cur.it_value.tv_nsec;
    if (first_ns <= 0 || first_ns > 300000000L)
        return fail("remaining out of range", first_ns);
    sleep_ms(100);
    if (xtimer_gettime(tid, &cur) != 0)
        return fail("gettime 2", errno);
    long later_ns = cur.it_value.tv_sec * 1000000000L + cur.it_value.tv_nsec;
    if (later_ns >= first_ns || later_ns <= 0)
        return fail("remaining not decreasing", later_ns);

    /* old_value reports the previous remaining time. */
    struct itimerspec old;
    memset(&its, 0, sizeof(its));
    ms_to_ts(100, &its.it_value);
    if (xtimer_settime(tid, 0, &its, &old) != 0)
        return fail("settime old_value", errno);
    long old_ns = old.it_value.tv_sec * 1000000000L + old.it_value.tv_nsec;
    if (old_ns <= 0 || old_ns > 300000000L)
        return fail("old_value out of range", old_ns);

    /* Disarm via it_value == 0. */
    memset(&its, 0, sizeof(its));
    if (xtimer_settime(tid, 0, &its, NULL) != 0)
        return fail("disarm", errno);
    if (xtimer_gettime(tid, &cur) != 0)
        return fail("gettime disarmed", errno);
    if (cur.it_value.tv_sec != 0 || cur.it_value.tv_nsec != 0)
        return fail("disarmed timer still armed", (long)cur.it_value.tv_nsec);

    /* Malformed values. */
    memset(&its, 0, sizeof(its));
    its.it_value.tv_nsec = 1000000000L;
    if (xtimer_settime(tid, 0, &its, NULL) != -1 || errno != EINVAL)
        return fail("nsec=1e9", errno);
    its.it_value.tv_nsec = -1;
    if (xtimer_settime(tid, 0, &its, NULL) != -1 || errno != EINVAL)
        return fail("nsec=-1", errno);
    its.it_value.tv_nsec = 0;
    its.it_value.tv_sec = -1;
    if (xtimer_settime(tid, 0, &its, NULL) != -1 || errno != EINVAL)
        return fail("sec=-1", errno);
    its.it_value.tv_sec = 0;
    its.it_value.tv_nsec = 1000000L;
    if (xtimer_settime(tid, 2, &its, NULL) != -1 || errno != EINVAL)
        return fail("flags=2", errno);

    /* Bad timer ids. */
    if (xtimer_gettime(9999, &cur) != -1 || errno != EINVAL)
        return fail("gettime bad id", errno);
    if (xtimer_settime(9999, 0, &its, NULL) != -1 || errno != EINVAL)
        return fail("settime bad id", errno);
    if (xtimer_getoverrun(9999) != -1 || errno != EINVAL)
        return fail("getoverrun bad id", errno);
    if (xtimer_delete(9999) != -1 || errno != EINVAL)
        return fail("delete bad id", errno);

    xtimer_delete(tid);
    return 0;
}

/* TIMER_ABSTIME arms against the timer's clock; past times fire at once. */
static int test_timer_abstime(void)
{
    install_handler(SIGUSR1, on_sigusr1);
    wire_sigevent_t sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    int tid;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != 0)
        return fail("create abstime", errno);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value = now;
    its.it_value.tv_nsec += 80000000L;
    if (its.it_value.tv_nsec >= 1000000000L) {
        its.it_value.tv_sec++;
        its.it_value.tv_nsec -= 1000000000L;
    }
    g_sigusr1_count = 0;
    uint64_t t0 = now_ms();
    if (xtimer_settime(tid, TIMER_ABSTIME, &its, NULL) != 0)
        return fail("arm abstime", errno);
    while (g_sigusr1_count == 0 && now_ms() - t0 < 3000)
        ;
    uint64_t el = now_ms() - t0;
    if (g_sigusr1_count != 1)
        return fail("abstime not delivered", g_sigusr1_count);
    if (el < 60)
        return fail("abstime fired early", (long)el);

    /* A past absolute time expires immediately. */
    its.it_value = now; /* ~80ms in the past by now */
    g_sigusr1_count = 0;
    t0 = now_ms();
    if (xtimer_settime(tid, TIMER_ABSTIME, &its, NULL) != 0)
        return fail("arm past abstime", errno);
    while (g_sigusr1_count == 0 && now_ms() - t0 < 3000)
        ;
    if (g_sigusr1_count != 1)
        return fail("past abstime not delivered", g_sigusr1_count);
    if (now_ms() - t0 > 1000)
        return fail("past abstime not immediate", (long)(now_ms() - t0));

    xtimer_delete(tid);
    return 0;
}

/* Overrun counting: expirations that coalesce while the notification is
 * still pending are reported by timer_getoverrun after delivery. */
static int test_timer_overrun(void)
{
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGUSR2);
    if (sigprocmask(SIG_BLOCK, &set, NULL) < 0)
        return fail("block SIGUSR2", errno);

    wire_sigevent_t sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR2;
    int tid;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != 0)
        return fail("create overrun", errno);
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    ms_to_ts(100, &its.it_value);
    ms_to_ts(100, &its.it_interval);
    if (xtimer_settime(tid, 0, &its, NULL) != 0)
        return fail("arm overrun", errno);

    /* ~350ms => expirations at 100/200/300ms; the first is pending, the
     * following two are overruns. */
    sleep_ms(350);
    siginfo_t si;
    if (sigwaitinfo(&set, &si) != SIGUSR2)
        return fail("sigwaitinfo", errno);
    long overrun = xtimer_getoverrun(tid);
    if (overrun < 1)
        return fail("overrun not counted", overrun);
    if (overrun > 6)
        return fail("overrun inflated", overrun);

    xtimer_delete(tid);
    /* Drain anything still pending before unblocking. */
    struct timespec z = { 0, 0 };
    sigtimedwait(&set, &si, &z);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    return 0;
}

/* A deleted timer never delivers. */
static int test_timer_delete_no_delivery(void)
{
    install_handler(SIGUSR1, on_sigusr1);
    wire_sigevent_t sev;
    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGUSR1;
    int tid;
    if (xtimer_create(CLK_MONOTONIC, &sev, &tid) != 0)
        return fail("create delete", errno);
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    ms_to_ts(60, &its.it_value);
    ms_to_ts(60, &its.it_interval);
    if (xtimer_settime(tid, 0, &its, NULL) != 0)
        return fail("arm delete", errno);
    sleep_ms(20);
    if (xtimer_delete(tid) != 0)
        return fail("delete", errno);
    g_sigusr1_count = 0;
    sleep_ms(200);
    if (g_sigusr1_count != 0)
        return fail("signal after delete", g_sigusr1_count);
    return 0;
}

/* timerfd: relative/absolute arming, periodic expiration counting, disarm,
 * and flag validation. */
static int test_timerfd_basics(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    if (fd < 0)
        return fail("timerfd_create", errno);

    /* Relative one-shot. */
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    ms_to_ts(80, &its.it_value);
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    uint64_t t0 = now_ms();
    if (timerfd_settime(fd, 0, &its, NULL) != 0)
        return fail("timerfd arm rel", errno);
    if (poll(&pfd, 1, 2000) != 1 || !(pfd.revents & POLLIN))
        return fail("timerfd rel not readable", -1);
    uint64_t el = now_ms() - t0;
    if (el < 60)
        return fail("timerfd rel fired early", (long)el);
    uint64_t expirations = 0;
    if (read(fd, &expirations, sizeof(expirations)) !=
            (ssize_t)sizeof(expirations) || expirations != 1)
        return fail("timerfd rel count", (long)expirations);

    /* gettime reports remaining. */
    ms_to_ts(200, &its.it_value);
    if (timerfd_settime(fd, 0, &its, NULL) != 0)
        return fail("timerfd rearm", errno);
    struct itimerspec cur;
    if (timerfd_gettime(fd, &cur) != 0)
        return fail("timerfd_gettime", errno);
    long rem_ns = cur.it_value.tv_sec * 1000000000L + cur.it_value.tv_nsec;
    if (rem_ns <= 0 || rem_ns > 200000000L)
        return fail("timerfd remaining", rem_ns);

    /* TFD_TIMER_ABSTIME against CLOCK_MONOTONIC. */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    memset(&its, 0, sizeof(its));
    its.it_value = now;
    its.it_value.tv_nsec += 80000000L;
    if (its.it_value.tv_nsec >= 1000000000L) {
        its.it_value.tv_sec++;
        its.it_value.tv_nsec -= 1000000000L;
    }
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL) != 0)
        return fail("timerfd arm abstime", errno);
    pfd.revents = 0;
    t0 = now_ms();
    if (poll(&pfd, 1, 2000) != 1)
        return fail("timerfd abstime not readable", -1);
    el = now_ms() - t0;
    if (el < 60)
        return fail("timerfd abstime fired early", (long)el);
    if (read(fd, &expirations, sizeof(expirations)) !=
            (ssize_t)sizeof(expirations))
        return fail("timerfd abstime read", errno);

    /* Past absolute time is immediately readable. */
    its.it_value = now; /* in the past */
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME, &its, NULL) != 0)
        return fail("timerfd arm past abstime", errno);
    pfd.revents = 0;
    if (poll(&pfd, 1, 1000) != 1)
        return fail("timerfd past abstime not readable", -1);
    if (read(fd, &expirations, sizeof(expirations)) !=
            (ssize_t)sizeof(expirations))
        return fail("timerfd past abstime read", errno);

    /* Periodic expirations accumulate in the read counter. */
    memset(&its, 0, sizeof(its));
    ms_to_ts(30, &its.it_value);
    ms_to_ts(30, &its.it_interval);
    if (timerfd_settime(fd, 0, &its, NULL) != 0)
        return fail("timerfd arm periodic", errno);
    sleep_ms(140);
    expirations = 0;
    if (read(fd, &expirations, sizeof(expirations)) !=
            (ssize_t)sizeof(expirations))
        return fail("timerfd periodic read", errno);
    if (expirations < 3 || expirations > 12)
        return fail("timerfd periodic count", (long)expirations);

    /* Disarm: not readable, nonblocking read is EAGAIN. */
    memset(&its, 0, sizeof(its));
    if (timerfd_settime(fd, 0, &its, NULL) != 0)
        return fail("timerfd disarm", errno);
    pfd.revents = 0;
    if (poll(&pfd, 1, 0) != 0)
        return fail("timerfd readable after disarm", pfd.revents);
    expirations = 0;
    if (read(fd, &expirations, sizeof(expirations)) != -1 || errno != EAGAIN)
        return fail("timerfd disarmed read", errno);

    /* Flag validation. */
    ms_to_ts(100, &its.it_value);
    if (timerfd_settime(fd, 4, &its, NULL) != -1 || errno != EINVAL)
        return fail("timerfd flags=4", errno);
    if (timerfd_settime(fd, TFD_TIMER_CANCEL_ON_SET, &its, NULL) != -1 ||
        errno != EINVAL)
        return fail("timerfd CANCEL_ON_SET without ABSTIME", errno);
    its.it_value.tv_nsec = 1000000000L;
    if (timerfd_settime(fd, 0, &its, NULL) != -1 || errno != EINVAL)
        return fail("timerfd nsec=1e9", errno);
    if (timerfd_create(42, 0) != -1 || errno != EINVAL)
        return fail("timerfd_create bogus clock", errno);
    if (timerfd_create(CLOCK_MONOTONIC, 0x4000) != -1 || errno != EINVAL)
        return fail("timerfd_create bogus flags", errno);

    close(fd);
    return 0;
}

/* TFD_TIMER_CANCEL_ON_SET: setting the realtime clock discontinuously makes
 * an armed realtime-abstime timerfd read fail with ECANCELED. */
static int test_timerfd_cancel_on_set(void)
{
    int fd = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK);
    if (fd < 0)
        return fail("timerfd_create realtime", errno);

    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    struct itimerspec its;
    memset(&its, 0, sizeof(its));
    its.it_value = rt;
    its.it_value.tv_sec += 60;
    if (timerfd_settime(fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET,
                        &its, NULL) != 0)
        return fail("arm cancel-on-set", errno);

    /* Jump the realtime clock forward. */
    struct timespec jumped = rt;
    jumped.tv_sec += 3600;
    if (clock_settime(CLOCK_REALTIME, &jumped) != 0)
        return fail("clock_settime", errno);

    uint64_t v;
    errno = 0;
    ssize_t n = read(fd, &v, sizeof(v));
    int canceled = n == -1 && errno == ECANCELED;

    /* A canceled timer is poll-readable. */
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int pollable = poll(&pfd, 1, 0) == 1 && (pfd.revents & POLLIN);

    /* Restore the realtime clock. */
    if (clock_settime(CLOCK_REALTIME, &rt) != 0)
        return fail("clock restore", errno);

    /* Re-arming works after a cancel. */
    memset(&its, 0, sizeof(its));
    ms_to_ts(30, &its.it_value);
    if (timerfd_settime(fd, 0, &its, NULL) != 0)
        return fail("rearm after cancel", errno);
    pfd.revents = 0;
    if (poll(&pfd, 1, 2000) != 1)
        return fail("rearm after cancel not readable", -1);

    close(fd);
    if (!canceled)
        return fail("ECANCELED not reported", n);
    if (!pollable)
        return fail("canceled timerfd not pollable", pfd.revents);
    return 0;
}

/* ITIMER_REAL delivers SIGALRM and reloads from it_interval;
 * ITIMER_VIRTUAL/ITIMER_PROF track cpu time and deliver while busy. */
static int test_itimer_real(void)
{
    install_handler(SIGALRM, on_sigalrm);

    struct itimerval it;
    memset(&it, 0, sizeof(it));
    ms_to_tv(60, &it.it_value);
    g_sigalrm_count = 0;
    uint64_t t0 = now_ms();
    if (setitimer(ITIMER_REAL, &it, NULL) != 0)
        return fail("setitimer REAL", errno);
    while (g_sigalrm_count == 0 && now_ms() - t0 < 3000)
        ;
    if (g_sigalrm_count != 1)
        return fail("SIGALRM not delivered", g_sigalrm_count);
    if (now_ms() - t0 < 40)
        return fail("SIGALRM fired early", (long)(now_ms() - t0));

    /* getitimer reports remaining + interval. */
    memset(&it, 0, sizeof(it));
    ms_to_tv(200, &it.it_value);
    ms_to_tv(50, &it.it_interval);
    if (setitimer(ITIMER_REAL, &it, NULL) != 0)
        return fail("setitimer REAL periodic", errno);
    struct itimerval cur;
    if (getitimer(ITIMER_REAL, &cur) != 0)
        return fail("getitimer REAL", errno);
    long rem_us = cur.it_value.tv_sec * 1000000L + cur.it_value.tv_usec;
    if (rem_us <= 0 || rem_us > 200000L)
        return fail("getitimer REAL remaining", rem_us);
    if (cur.it_interval.tv_usec != 50000L)
        return fail("getitimer REAL interval", (long)cur.it_interval.tv_usec);

    /* Interval reload: two deliveries within ~150ms. */
    g_sigalrm_count = 0;
    memset(&it, 0, sizeof(it));
    ms_to_tv(40, &it.it_value);
    ms_to_tv(40, &it.it_interval);
    if (setitimer(ITIMER_REAL, &it, NULL) != 0)
        return fail("setitimer reload", errno);
    t0 = now_ms();
    while (g_sigalrm_count < 2 && now_ms() - t0 < 3000)
        ;
    if (g_sigalrm_count < 2)
        return fail("itimer interval reload", g_sigalrm_count);
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_REAL, &it, NULL); /* disarm */
    return 0;
}

static void burn_cpu(uint64_t wall_ms)
{
    uint64_t start = now_ms();
    volatile uint64_t acc = 0;
    while (now_ms() - start < wall_ms) {
        for (int i = 0; i < 100000; i++)
            acc += (uint64_t)i * 2654435761ULL;
    }
    (void)acc;
}

static int test_itimer_virtual_prof(void)
{
    install_handler(SIGVTALRM, on_sigvtalrm);
    install_handler(SIGPROF, on_sigprof);

    /* ITIMER_VIRTUAL counts user cpu time. */
    struct itimerval it;
    memset(&it, 0, sizeof(it));
    ms_to_tv(40, &it.it_value);
    g_sigvtalrm_count = 0;
    if (setitimer(ITIMER_VIRTUAL, &it, NULL) != 0)
        return fail("setitimer VIRTUAL", errno);

    struct itimerval cur;
    if (getitimer(ITIMER_VIRTUAL, &cur) != 0)
        return fail("getitimer VIRTUAL", errno);
    long rem_us = cur.it_value.tv_sec * 1000000L + cur.it_value.tv_usec;
    if (rem_us <= 0 || rem_us > 40000L)
        return fail("getitimer VIRTUAL remaining", rem_us);

    uint64_t t0 = now_ms();
    while (g_sigvtalrm_count == 0 && now_ms() - t0 < 5000)
        burn_cpu(50);
    if (g_sigvtalrm_count != 1)
        return fail("SIGVTALRM not delivered under load",
                    g_sigvtalrm_count);

    /* Sleeping must not advance the virtual timer: re-arm and sleep well
     * past the timeout; no delivery expected. */
    memset(&it, 0, sizeof(it));
    ms_to_tv(40, &it.it_value);
    g_sigvtalrm_count = 0;
    if (setitimer(ITIMER_VIRTUAL, &it, NULL) != 0)
        return fail("setitimer VIRTUAL 2", errno);
    sleep_ms(200);
    if (g_sigvtalrm_count != 0)
        return fail("SIGVTALRM delivered while sleeping",
                    g_sigvtalrm_count);
    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_VIRTUAL, &it, NULL);

    /* ITIMER_PROF counts user+system cpu time. */
    memset(&it, 0, sizeof(it));
    ms_to_tv(40, &it.it_value);
    g_sigprof_count = 0;
    if (setitimer(ITIMER_PROF, &it, NULL) != 0)
        return fail("setitimer PROF", errno);
    t0 = now_ms();
    while (g_sigprof_count == 0 && now_ms() - t0 < 5000)
        burn_cpu(50);
    if (g_sigprof_count != 1)
        return fail("SIGPROF not delivered under load", g_sigprof_count);

    memset(&it, 0, sizeof(it));
    setitimer(ITIMER_PROF, &it, NULL);
    return 0;
}

/* clock_nanosleep: relative/absolute sleeps, validation, and EINTR+remain
 * on signal interruption (never restarted). */
static int test_clock_nanosleep(void)
{
    struct timespec ts;
    ms_to_ts(60, &ts);
    uint64_t t0 = now_ms();
    int r = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    if (r != 0)
        return fail("clock_nanosleep relative", r);
    if (now_ms() - t0 < 40)
        return fail("clock_nanosleep returned early",
                    (long)(now_ms() - t0));

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    struct timespec abs = now;
    abs.tv_nsec += 60000000L;
    if (abs.tv_nsec >= 1000000000L) {
        abs.tv_sec++;
        abs.tv_nsec -= 1000000000L;
    }
    t0 = now_ms();
    r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL);
    if (r != 0)
        return fail("clock_nanosleep abstime", r);
    if (now_ms() - t0 < 40)
        return fail("clock_nanosleep abstime early",
                    (long)(now_ms() - t0));

    /* Past absolute time returns immediately. */
    abs = now;
    t0 = now_ms();
    r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &abs, NULL);
    if (r != 0 || now_ms() - t0 > 200)
        return fail("clock_nanosleep past abstime", r);

    /* Validation. */
    ms_to_ts(10, &ts);
    r = clock_nanosleep(42, 0, &ts, NULL);
    if (r != EINVAL)
        return fail("clock_nanosleep bogus clock", r);
    ts.tv_nsec = 1000000000L;
    r = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    if (r != EINVAL)
        return fail("clock_nanosleep nsec=1e9", r);
    ts.tv_sec = -1;
    ts.tv_nsec = 0;
    r = clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);
    if (r != EINVAL)
        return fail("clock_nanosleep sec=-1", r);
    ts.tv_sec = 0;
    r = clock_nanosleep(CLOCK_MONOTONIC, 2, &ts, NULL);
    if (r != EINVAL)
        return fail("clock_nanosleep flags=2", r);
    ts.tv_sec = -1;
    r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
    if (r != EINVAL)
        return fail("clock_nanosleep abstime sec=-1", r);

    /* nanosleep rejects negative times too. */
    if (nanosleep(&ts, NULL) != -1 || errno != EINVAL)
        return fail("nanosleep sec=-1", errno);
    return 0;
}

static int test_clock_nanosleep_eintr(void)
{
    install_handler(SIGUSR1, on_sigusr1);

    pid_t child = fork();
    if (child < 0)
        return fail("fork", -1);
    if (child == 0) {
        usleep(50000);
        kill(getppid(), SIGUSR1);
        usleep(50000);
        kill(getppid(), SIGUSR1);
        _exit(0);
    }

    /* Relative sleep interrupted at ~50ms: EINTR with remaining time. */
    struct timespec req = { .tv_sec = 5, .tv_nsec = 0 };
    struct timespec rem = { 0, 0 };
    uint64_t t0 = now_ms();
    int r = clock_nanosleep(CLOCK_MONOTONIC, 0, &req, &rem);
    uint64_t el = now_ms() - t0;
    if (r != EINTR)
        return fail("clock_nanosleep no EINTR", r);
    if (el < 30 || el > 3000)
        return fail("clock_nanosleep EINTR timing", (long)el);
    long rem_ns = rem.tv_sec * 1000000000L + rem.tv_nsec;
    if (rem_ns <= 0 || rem_ns >= 5000000000L)
        return fail("clock_nanosleep remain", rem_ns);

    /* Absolute sleep interrupted: EINTR as well (rem unused). */
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    now.tv_sec += 5;
    r = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &now, NULL);
    if (r != EINTR)
        return fail("clock_nanosleep abstime no EINTR", r);

    int status = 0;
    waitpid(child, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("eintr child", status);
    return 0;
}

int main(void)
{
    struct {
        const char *name;
        int (*fn)(void);
    } tests[] = {
        { "timer_create_clocks", test_timer_create_clocks },
        { "timer_create_sigev", test_timer_create_sigev },
        { "timer_settime_gettime", test_timer_settime_gettime },
        { "timer_abstime", test_timer_abstime },
        { "timer_overrun", test_timer_overrun },
        { "timer_delete_no_delivery", test_timer_delete_no_delivery },
        { "timerfd_basics", test_timerfd_basics },
        { "timerfd_cancel_on_set", test_timerfd_cancel_on_set },
        { "itimer_real", test_itimer_real },
        { "itimer_virtual_prof", test_itimer_virtual_prof },
        { "clock_nanosleep", test_clock_nanosleep },
        { "clock_nanosleep_eintr", test_clock_nanosleep_eintr },
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        if (tests[i].fn() < 0) {
            printf("TIMER_EDGE: FAIL in %s\n", tests[i].name);
            return 1;
        }
        printf("TIMER_EDGE: %s ok\n", tests[i].name);
    }
    printf("TIMER_EDGE: PASS\n");
    return 0;
}
