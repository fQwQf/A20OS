#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "proc/proc_internal.h"

typedef struct {
    int used;
    int owner_pid;
    int signo;
    uint64_t interval[2];
    uint64_t value[2];
    uint64_t expire_tick;
} posix_timer_t;

#define COMPAT_TIMER_MAX 32
static posix_timer_t g_posix_timers[COMPAT_TIMER_MAX];

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

static uint64_t posix_timer_timespec_to_ticks(uint64_t sec, uint64_t nsec)
{
    return sec * TICKS_PER_SEC + (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
}

static uint64_t timeval_to_ticks(uint64_t sec, uint64_t usec)
{
    return sec * TICKS_PER_SEC + (usec * TICKS_PER_SEC + 999999ULL) / 1000000ULL;
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
        memcpy(out, t->itimer_values[which], sizeof(uint64_t) * 4);
        return 0;
    }

    ticks_to_timeval(t->itimer_real_interval, out);
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

    uint64_t ts[2];
    if (copy_from_user(ts, req, sizeof(ts)) < 0) return -EFAULT;
    if (ts[1] >= 1000000000ULL) return -EINVAL;

    if (!(flags & TIMER_ABSTIME))
        return sys_nanosleep((void *)req, rem);

    uint64_t now_ts[2];
    if (posix_clock_is_realtime(clk)) timekeeping_get_realtime(now_ts);
    else if (posix_clock_is_monotonic(clk)) timekeeping_get_monotonic(now_ts);
    else return -EINVAL;

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

    (void)rem;
    uint64_t ticks = sec * TICKS_PER_SEC +
                     (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
    uint64_t until = timer_get_ticks() + ticks;

    task_t *t = proc_current();
    if (t) {
        proc_block_until(t, until);
        sched();
        proc_set_wake_time(t, 0);
        if (signal_task_has_unblocked(t))
            return -ERESTARTSYS;
    } else {
        while (timer_get_ticks() < until) __asm__ volatile("nop");
    }
    return 0;
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
    if (next[1] >= 1000000ULL || next[3] >= 1000000ULL) return -EINVAL;
    memcpy(cur->itimer_values[which], next, sizeof(next));
    if (which == 0) {
        cur->itimer_real_interval = timeval_to_ticks(next[0], next[1]);
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
    cur->itimer_real_interval = 0;
    memset(cur->itimer_values[0], 0, sizeof(cur->itimer_values[0]));
    cur->itimer_values[0][2] = seconds;
    proc_set_alarm_expire(cur, seconds ? now + (uint64_t)seconds * TICKS_PER_SEC : 0);
    return old;
}

int64_t sys_timer_create(int clockid, void *sevp, int *timerid)
{
    if (clockid != 0 && clockid != 1 && clockid != 7) return -EINVAL;
    if (!timerid) return -EFAULT;

    int signo = SIGALRM;
    if (sevp) {
        int notify = 0;
        if (copy_from_user(&notify, sevp, sizeof(int)) < 0) return -EFAULT;
        if (notify == 0) {
            /* SIGEV_NONE — no notification */
            signo = 0;
        } else if (notify == 1) {
            /* SIGEV_SIGNAL — read sigev_signo */
            if (copy_from_user(&signo, (char *)sevp + 4, sizeof(int)) < 0) return -EFAULT;
        }
        /* SIGEV_THREAD_ID (4) would need thread-targeted delivery — skip */
    }

    task_t *cur = proc_current();
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        if (!g_posix_timers[i].used) {
            memset(&g_posix_timers[i], 0, sizeof(g_posix_timers[i]));
            g_posix_timers[i].used = 1;
            g_posix_timers[i].owner_pid = cur ? cur->pid : 0;
            g_posix_timers[i].signo = signo;
            if (copy_to_user(timerid, &i, sizeof(i)) < 0) return -EFAULT;
            return 0;
        }
    }
    return -EAGAIN;
}

int64_t sys_timer_delete(int timerid)
{
    if (timerid < 0 || timerid >= COMPAT_TIMER_MAX || !g_posix_timers[timerid].used)
        return -EINVAL;
    task_t *cur = proc_current();
    if (cur && g_posix_timers[timerid].owner_pid != cur->pid)
        return -EINVAL;
    memset(&g_posix_timers[timerid], 0, sizeof(g_posix_timers[timerid]));
    return 0;
}

int64_t sys_timer_gettime(int timerid, void *curr_value)
{
    if (!curr_value) return -EFAULT;
    if (timerid < 0 || timerid >= COMPAT_TIMER_MAX || !g_posix_timers[timerid].used)
        return -EINVAL;
    task_t *cur = proc_current();
    if (cur && g_posix_timers[timerid].owner_pid != cur->pid)
        return -EINVAL;
    uint64_t out[4] = {
        g_posix_timers[timerid].interval[0],
        g_posix_timers[timerid].interval[1],
        g_posix_timers[timerid].value[0],
        g_posix_timers[timerid].value[1],
    };
    return copy_to_user(curr_value, out, sizeof(out)) < 0 ? -EFAULT : 0;
}

int64_t sys_timer_getoverrun(int timerid)
{
    if (timerid < 0 || timerid >= COMPAT_TIMER_MAX || !g_posix_timers[timerid].used)
        return -EINVAL;
    task_t *cur = proc_current();
    if (cur && g_posix_timers[timerid].owner_pid != cur->pid)
        return -EINVAL;
    return 0;
}

int64_t sys_timer_settime(int timerid, int flags, const void *new_value, void *old_value)
{
    (void)flags;
    if (timerid < 0 || timerid >= COMPAT_TIMER_MAX || !g_posix_timers[timerid].used)
        return -EINVAL;
    task_t *cur = proc_current();
    if (cur && g_posix_timers[timerid].owner_pid != cur->pid)
        return -EINVAL;
    if (old_value) {
        int64_t r = sys_timer_gettime(timerid, old_value);
        if (r < 0) return r;
    }
    if (!new_value) return -EFAULT;
    uint64_t ts[4];
    if (copy_from_user(ts, new_value, sizeof(ts)) < 0) return -EFAULT;
    memcpy(g_posix_timers[timerid].interval, ts, sizeof(uint64_t) * 2);
    memcpy(g_posix_timers[timerid].value, ts + 2, sizeof(uint64_t) * 2);
    g_posix_timers[timerid].expire_tick = timer_get_ticks() + posix_timer_timespec_to_ticks(ts[2], ts[3]);
    return 0;
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

extern int signal_send(int pid, int signum);

void posix_timer_tick(void)
{
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        if (!g_posix_timers[i].used || g_posix_timers[i].signo == 0)
            continue;
        if (g_posix_timers[i].expire_tick > 0 && now >= g_posix_timers[i].expire_tick) {
            signal_send(g_posix_timers[i].owner_pid, g_posix_timers[i].signo);
            if (g_posix_timers[i].interval[0] || g_posix_timers[i].interval[1]) {
                uint64_t interval_ticks = posix_timer_timespec_to_ticks(
                    g_posix_timers[i].interval[0], g_posix_timers[i].interval[1]);
                g_posix_timers[i].expire_tick = now + interval_ticks;
            } else {
                g_posix_timers[i].expire_tick = 0;
            }
        }
    }
}
