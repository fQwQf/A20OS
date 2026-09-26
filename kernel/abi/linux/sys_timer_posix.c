#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "proc/proc_internal.h"
#include "proc/posix_timer.h"

/*
 * Linux ABI POSIX-timer surface — wire translation only.
 *
 * Timer objects, ownership checks and expiry delivery live in
 * kernel/proc/timer_posix.c (driven from proc/timer_heap.c).  This file
 * decodes the sigevent/timespec wire structs onto the core calls.
 */

typedef struct {
    unsigned modes;
    long offset;
    long freq;
    long maxerror;
    long esterror;
    int status;
    long constant;
    long precision;
    long tolerance;
    int64_t time_sec;
    int64_t time_usec;
    long tick;
    long ppsfreq;
    long jitter;
    int shift;
    long stabil;
    long jitcnt;
    long calcnt;
    long errcnt;
    long stbcnt;
    int tai;
    int padding[11];
} kernel_timex_t;

#define ADJ_OFFSET             0x0001U
#define ADJ_FREQUENCY          0x0002U
#define ADJ_MAXERROR           0x0004U
#define ADJ_ESTERROR           0x0008U
#define ADJ_STATUS             0x0010U
#define ADJ_TIMECONST          0x0020U
#define ADJ_TAI                0x0080U
#define ADJ_SETOFFSET          0x0100U
#define ADJ_MICRO              0x1000U
#define ADJ_NANO               0x2000U
#define ADJ_TICK               0x4000U
#define ADJ_OFFSET_SINGLESHOT  0x8001U
#define ADJ_OFFSET_SS_READ     0xa001U
#define ADJ_VALID_BASE_MODES   (ADJ_OFFSET | ADJ_FREQUENCY | ADJ_MAXERROR | \
                                ADJ_ESTERROR | ADJ_STATUS | ADJ_TIMECONST | \
                                ADJ_TAI | ADJ_SETOFFSET | ADJ_MICRO | \
                                ADJ_NANO | ADJ_TICK)

static int posix_clock_is_realtime(int clk)
{
    return clk == 0 || clk == 5 || clk == 8 || clk == 11;
}

/* Persistent adjtimex state so repeated reads reflect prior writes. */
static kernel_timex_t g_adjtimex_state;
static int g_adjtimex_inited;

static void adjtimex_state_init(kernel_timex_t *tx)
{
    memset(tx, 0, sizeof(*tx));
    tx->maxerror = 16000000L;
    tx->esterror = 16000000L;
    tx->status   = 64; /* STA_UNSYNC */
    tx->constant = 4;
    tx->tick     = 10000L;
    tx->precision = 1L;
    tx->tolerance = 500000L;
}

static int posix_clock_is_monotonic(int clk)
{
    return clk == 1 || clk == 2 || clk == 3 || clk == 4 ||
           clk == 6 || clk == 7 || clk == 9;
}

static uint64_t timeval_to_ticks(uint64_t sec, uint64_t usec)
{
    return sec * TICKS_PER_SEC + (usec * TICKS_PER_SEC + 999999ULL) / 1000000ULL;
}

/*
 * ITIMER_VIRTUAL/PROF are accounted in scheduler accounting ticks
 * (proc_sched_tick runs at a nominal 100 Hz): utime_ticks/stime_ticks
 * count those ticks, not timer ticks.
 */
static uint64_t timeval_to_sched_ticks(uint64_t sec, uint64_t usec)
{
    return sec * 100ULL + (usec + 9999ULL) / 10000ULL;
}

static void sched_ticks_to_timeval(uint64_t ticks, uint64_t out[2])
{
    out[0] = ticks / 100ULL;
    out[1] = (ticks % 100ULL) * 10000ULL;
}

static void ticks_to_timeval(uint64_t ticks, uint64_t out[2])
{
    out[0] = ticks / TICKS_PER_SEC;
    out[1] = (ticks % TICKS_PER_SEC) * 1000000ULL / TICKS_PER_SEC;
}

static int fill_getitimer(task_t *t, int which, uint64_t out[4])
{
    if (!t || which < 0 || which >= 3) return -EINVAL;
    memset(out, 0, sizeof(uint64_t) * 4);
    if (which != 0) {
        /* ITIMER_VIRTUAL/ITIMER_PROF: cpu-time timers driven by the
         * scheduler tick accounting (kernel/proc/timer_posix.c). */
        uint64_t cpu[2];
        int r = posix_itimer_get(t, which - 1, cpu);
        if (r < 0) return r;
        sched_ticks_to_timeval(cpu[0], out);
        sched_ticks_to_timeval(cpu[1], out + 2);
        return 0;
    }

    ticks_to_timeval(
        __atomic_load_n(&t->itimer_real_interval, __ATOMIC_RELAXED), out);
    uint64_t now = timer_get_ticks();
    uint64_t expire = __atomic_load_n(&t->alarm_expire, __ATOMIC_RELAXED);
    uint64_t rem = (expire > now) ? (expire - now) : 0;
    ticks_to_timeval(rem, out + 2);
    if (rem && out[2] == 0 && out[3] == 0)
        out[3] = 1;
    return 0;
}

int64_t sys_clock_nanosleep(int clk, int flags, const void *req, void *rem)
{
    const int TIMER_ABSTIME = 1;
    if (flags & ~TIMER_ABSTIME) return -EINVAL;
    if (!req) return -EFAULT;

    /* CLOCK_PROCESS/THREAD_CPUTIME_ID sleeps are not modeled; refuse them
     * rather than silently sleeping on the monotonic clock. */
    if (clk == 2 || clk == 3) return -EOPNOTSUPP;
    int realtime = posix_clock_is_realtime(clk);
    int monotonic = posix_clock_is_monotonic(clk);
    if (!realtime && !monotonic) return -EINVAL;

    uint64_t ts[2];
    if (copy_from_user(ts, req, sizeof(ts)) < 0) return -EFAULT;
    if ((int64_t)ts[0] < 0 || (int64_t)ts[1] < 0 ||
        ts[1] >= 1000000000ULL) return -EINVAL;

    uint64_t until;
    if (flags & TIMER_ABSTIME) {
        uint64_t now_ts[2];
        if (realtime) timekeeping_get_realtime(now_ts);
        else timekeeping_get_monotonic(now_ts);

        if (ts[0] < now_ts[0] || (ts[0] == now_ts[0] && ts[1] <= now_ts[1]))
            return 0;

        uint64_t sec = ts[0] - now_ts[0];
        uint64_t nsec;
        if (ts[1] >= now_ts[1]) {
            nsec = ts[1] - now_ts[1];
        } else {
            if (sec == 0) return 0;
            sec--;
            nsec = 1000000000ULL + ts[1] - now_ts[1];
        }
        uint64_t ticks = sec * TICKS_PER_SEC +
                         (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
        until = timer_get_ticks() + ticks;
    } else {
        uint64_t ticks = ts[0] * TICKS_PER_SEC +
                         (ts[1] * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
        if (ticks == 0)
            return 0;
        until = timer_get_ticks() + ticks;
    }

    task_t *t = proc_current();
    if (!t) {
        while (timer_get_ticks() < until) cpu_relax();
        return 0;
    }
    for (;;) {
        proc_wake_reason_t reason =
            proc_park_wait(PROC_WAIT_INTERRUPTIBLE, until);
        if (reason == PROC_WAKE_TIMEOUT_CAPACITY)
            return -EAGAIN;
        if (proc_wake_reason_is_task_interrupt(reason) ||
            signal_task_has_unblocked(t)) {
            /* Linux: clock_nanosleep is never restarted; relative sleeps
             * report the remaining time through rem. */
            if (!(flags & TIMER_ABSTIME) && rem) {
                uint64_t now = timer_get_ticks();
                uint64_t left = until > now ? until - now : 0;
                uint64_t out[2] = {
                    left / TICKS_PER_SEC,
                    (left % TICKS_PER_SEC) * 1000000000ULL / TICKS_PER_SEC,
                };
                if (copy_to_user(rem, out, sizeof(out)) < 0)
                    return -EFAULT;
            }
            return -EINTR;
        }
        if (timer_get_ticks() >= until)
            return 0;
    }
}

int64_t sys_getitimer(int which, void *curr_value)
{
    if (which < 0 || which >= 3) return -EINVAL;
    if (!curr_value) return -EFAULT;
    task_t *cur = proc_current();
    uint64_t out[4];
    int r = fill_getitimer(cur, which, out);
    if (r < 0) return r;
    return copy_to_user(curr_value, out, sizeof(out)) < 0 ? -EFAULT : 0;
}

int64_t sys_setitimer(int which, const void *new_value, void *old_value)
{
    if (which < 0 || which >= 3) return -EINVAL;
    task_t *cur = proc_current();
    if (!cur) return -EINVAL;
    if (old_value) {
        uint64_t old[4];
        int r = fill_getitimer(cur, which, old);
        if (r < 0) return r;
        if (copy_to_user(old_value, old, sizeof(old)) < 0) return -EFAULT;
    }
    if (!new_value) return -EFAULT;
    uint64_t next[4];
    if (copy_from_user(next, new_value, sizeof(next)) < 0) return -EFAULT;
    if ((int64_t)next[0] < 0 || (int64_t)next[1] < 0 || next[1] >= 1000000ULL ||
        (int64_t)next[2] < 0 || (int64_t)next[3] < 0 || next[3] >= 1000000ULL) return -EINVAL;
    if (which != 0) {
        return posix_itimer_set(cur, which - 1,
                                timeval_to_sched_ticks(next[2], next[3]),
                                timeval_to_sched_ticks(next[0], next[1]));
    }
    memcpy(cur->itimer_values[which], next, sizeof(next));
    {
        __atomic_store_n(&cur->itimer_real_interval,
                         timeval_to_ticks(next[0], next[1]),
                         __ATOMIC_RELAXED);
        uint64_t ticks = timeval_to_ticks(next[2], next[3]);
        proc_set_alarm_expire(cur, ticks ? timer_get_ticks() + ticks : 0);
    }
    return 0;
}

int64_t sys_alarm(unsigned seconds)
{
    task_t *cur = proc_current();
    if (!cur) return 0;
    uint64_t now = timer_get_ticks();
    unsigned old = 0;
    uint64_t expire = __atomic_load_n(&cur->alarm_expire, __ATOMIC_RELAXED);
    if (expire > now) {
        uint64_t rem = expire - now;
        old = (unsigned)((rem + TICKS_PER_SEC - 1) / TICKS_PER_SEC);
    }
    __atomic_store_n(&cur->itimer_real_interval, 0, __ATOMIC_RELAXED);
    memset(cur->itimer_values[0], 0, sizeof(cur->itimer_values[0]));
    cur->itimer_values[0][2] = seconds;
    proc_set_alarm_expire(cur, seconds ? now + (uint64_t)seconds * TICKS_PER_SEC : 0);
    return old;
}

/* Linux ABI struct sigevent wire layout (uapi/asm-generic/siginfo.h):
 *   union sigval sigev_value;   // 0..7
 *   int sigev_signo;            // 8
 *   int sigev_notify;           // 12
 *   union { int _tid; ... } ;   // 16
 * SIGEV_SIGNAL == 0, SIGEV_NONE == 1, SIGEV_THREAD == 2, SIGEV_THREAD_ID == 4.
 */
typedef struct {
    uint64_t sigev_value;
    int32_t  sigev_signo;
    int32_t  sigev_notify;
    union {
        int32_t pad[12];
        int32_t tid;
    } un;
} kernel_sigevent_t;

int64_t sys_timer_create(int clockid, void *sevp, int *timerid)
{
    /* CPU-time clocks are not modeled; refuse them explicitly. */
    if (clockid == 2 || clockid == 3) return -EOPNOTSUPP;
    if (!posix_timer_clock_supported(clockid)) return -EINVAL;
    if (!timerid) return -EFAULT;

    int signo = SIGALRM;
    int target_tid = 0;
    int no_notify = 0;
    if (sevp) {
        kernel_sigevent_t sev;
        if (copy_from_user(&sev, sevp, sizeof(sev)) < 0) return -EFAULT;
        if (sev.sigev_notify == 0) {
            /* SIGEV_SIGNAL — deliver sigev_signo process-wide */
            signo = sev.sigev_signo;
        } else if (sev.sigev_notify == 1) {
            /* SIGEV_NONE — no notification */
            signo = 0;
            no_notify = 1;
        } else if (sev.sigev_notify == 4) {
            /* SIGEV_THREAD_ID — deliver sigev_signo to a specific thread */
            signo = sev.sigev_signo;
            target_tid = sev.un.tid;
            if (target_tid <= 0)
                return -EINVAL;
        } else {
            /* SIGEV_THREAD (2) needs a user function+attribute to spawn a
             * thread (a libc-side construct; musl/glibc translate it to
             * SIGEV_THREAD_ID before the syscall).  It and any unknown
             * notify value are refused rather than silently ignored. */
            return -EINVAL;
        }
        if (!no_notify && (signo < 1 || signo >= NSIG))
            return -EINVAL;
    }

    task_t *cur = proc_current();
    int id = posix_timer_create(cur ? cur->pid : 0, signo, target_tid,
                                clockid);
    if (id < 0)
        return id;
    if (copy_to_user(timerid, &id, sizeof(id)) < 0) {
        posix_timer_delete(cur ? cur->pid : 0, id);
        return -EFAULT;
    }
    return 0;
}

int64_t sys_timer_delete(int timerid)
{
    task_t *cur = proc_current();
    int r = posix_timer_delete(cur ? cur->pid : 0, timerid);
    return r < 0 ? r : 0;
}

int64_t sys_timer_gettime(int timerid, void *curr_value)
{
    if (!curr_value) return -EFAULT;
    task_t *cur = proc_current();
    uint64_t out[4];
    int r = posix_timer_get_time(cur ? cur->pid : 0, timerid, out);
    if (r < 0) return r;
    return copy_to_user(curr_value, out, sizeof(out)) < 0 ? -EFAULT : 0;
}

int64_t sys_timer_getoverrun(int timerid)
{
    task_t *cur = proc_current();
    return posix_timer_getoverrun(cur ? cur->pid : 0, timerid);
}

int64_t sys_timer_settime(int timerid, int flags, const void *new_value, void *old_value)
{
    task_t *cur = proc_current();
    int pid = cur ? cur->pid : 0;
    if (old_value) {
        uint64_t old[4];
        int r = posix_timer_get_time(pid, timerid, old);
        if (r < 0) return r;
        if (copy_to_user(old_value, old, sizeof(old)) < 0) return -EFAULT;
    }
    if (!new_value) return -EFAULT;
    uint64_t ts[4];
    if (copy_from_user(ts, new_value, sizeof(ts)) < 0) return -EFAULT;
    if ((int64_t)ts[0] < 0 || (int64_t)ts[1] < 0 || ts[1] >= 1000000000ULL ||
        (int64_t)ts[2] < 0 || (int64_t)ts[3] < 0 || ts[3] >= 1000000000ULL)
        return -EINVAL;
    int r = posix_timer_set_time(pid, timerid, ts, flags);
    return r < 0 ? r : 0;
}

int64_t sys_adjtimex(void *buf)
{
    if (!buf) return -EFAULT;
    kernel_timex_t tx;
    if (copy_from_user(&tx, buf, sizeof(tx)) < 0) return -EFAULT;

    if (!g_adjtimex_inited) {
        adjtimex_state_init(&g_adjtimex_state);
        g_adjtimex_inited = 1;
    }

    uint32_t modes = tx.modes;
    if (modes == 0x8000U) return -EINVAL;
    if (modes != 0 && modes != ADJ_OFFSET_SINGLESHOT && modes != ADJ_OFFSET_SS_READ &&
        (modes & ~ADJ_VALID_BASE_MODES))
        return -EINVAL;
    task_t *cur = proc_current();
    if (modes && modes != ADJ_OFFSET_SS_READ && !proc_has_cap(cur, CAP_SYS_ADMIN))
        return -EPERM;
    if (modes == ADJ_TICK && (tx.tick < 9000 || tx.tick > 11000))
        return -EINVAL;

    if (modes && modes != ADJ_OFFSET_SS_READ && modes != ADJ_OFFSET_SINGLESHOT) {
        if (modes & ADJ_MAXERROR)   g_adjtimex_state.maxerror  = tx.maxerror;
        if (modes & ADJ_ESTERROR)   g_adjtimex_state.esterror  = tx.esterror;
        if (modes & ADJ_STATUS)     g_adjtimex_state.status    = tx.status;
        if (modes & ADJ_TIMECONST)  g_adjtimex_state.constant  = tx.constant;
        if (modes & ADJ_TAI)        g_adjtimex_state.tai       = tx.tai;
        if (modes & ADJ_TICK)       g_adjtimex_state.tick      = tx.tick;
        if (modes & ADJ_FREQUENCY)  g_adjtimex_state.freq      = tx.freq;
        if (modes & ADJ_OFFSET)     g_adjtimex_state.offset    = tx.offset;
        if (modes & ADJ_SETOFFSET) {
            g_adjtimex_state.offset = tx.offset;
            uint64_t ts[2];
            timekeeping_get_realtime(ts);
            int64_t delta_usec = tx.offset;
            if (delta_usec > 0) {
                ts[0] += (uint64_t)delta_usec / 1000000ULL;
                ts[1] += (uint64_t)(delta_usec % 1000000ULL) * 1000ULL;
            } else {
                uint64_t abs_usec = (uint64_t)(-delta_usec);
                uint64_t delta_sec = abs_usec / 1000000ULL;
                uint64_t delta_nsec = (abs_usec % 1000000ULL) * 1000ULL;
                if (ts[1] >= delta_nsec) {
                    ts[1] -= delta_nsec;
                } else if (ts[0] > 0) {
                    ts[0]--;
                    ts[1] = ts[1] + 1000000000ULL - delta_nsec;
                }
                if (ts[0] >= delta_sec)
                    ts[0] -= delta_sec;
                else
                    ts[0] = 0;
            }
            if (ts[1] >= 1000000000ULL) {
                ts[0] += ts[1] / 1000000000ULL;
                ts[1] %= 1000000000ULL;
            }
            timekeeping_set_realtime(ts[0], ts[1]);
            g_adjtimex_state.offset = 0;
        }
        g_adjtimex_state.modes = modes;
    }

    uint64_t now = timer_get_ticks();
    g_adjtimex_state.time_sec  = (int64_t)(now / TICKS_PER_SEC);
    g_adjtimex_state.time_usec = (int64_t)((now % TICKS_PER_SEC) * 1000000ULL / TICKS_PER_SEC);

    if (modes == ADJ_OFFSET_SS_READ || modes == ADJ_OFFSET_SINGLESHOT)
        g_adjtimex_state.offset = 0;

    if (copy_to_user(buf, &g_adjtimex_state, sizeof(g_adjtimex_state)) < 0)
        return -EFAULT;
    /* Return NTP synchronization state (not an error code):
     * TIME_OK(0), TIME_INS(1), TIME_DEL(2), TIME_OOP(3),
     * TIME_WAIT(4), TIME_ERROR(5).
     * STA_UNSYNC(0x40) means clock is not synchronized -> TIME_ERROR(5). */
    if (g_adjtimex_state.status & 0x40U) /* STA_UNSYNC */
        return 5; /* TIME_ERROR */
    return 0; /* TIME_OK */
}

int64_t sys_clock_adjtime(int clk, void *buf)
{
    if (!buf) return -EFAULT;
    kernel_timex_t tx;
    if (copy_from_user(&tx, buf, sizeof(tx)) < 0) return -EFAULT;
    uint32_t modes = tx.modes;
    if (modes != 0 && !posix_clock_is_realtime(clk))
        return -EOPNOTSUPP;
    return sys_adjtimex(buf);
}
