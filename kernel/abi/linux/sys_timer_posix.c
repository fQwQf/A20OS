#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

typedef struct {
    int used;
    int owner_pid;
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
    (void)clk; (void)flags;
    return sys_nanosleep((void *)req, rem);
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
    (void)sevp;
    if (clockid != 0 && clockid != 1 && clockid != 7) return -EINVAL;
    if (!timerid) return -EFAULT;
    task_t *cur = proc_current();
    for (int i = 0; i < COMPAT_TIMER_MAX; i++) {
        if (!g_posix_timers[i].used) {
            memset(&g_posix_timers[i], 0, sizeof(g_posix_timers[i]));
            g_posix_timers[i].used = 1;
            g_posix_timers[i].owner_pid = cur ? cur->pid : 0;
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
    uint32_t modes = tx.modes;
    if (modes == 0x8000U) return -EINVAL;
    if (modes != ADJ_OFFSET_SINGLESHOT && modes != ADJ_OFFSET_SS_READ &&
        (modes & ~ADJ_VALID_BASE_MODES))
        return -EINVAL;
    task_t *cur = proc_current();
    if (modes && modes != ADJ_OFFSET_SS_READ && !proc_has_cap(cur, CAP_SYS_ADMIN))
        return -EPERM;
    if (modes == ADJ_TICK && (tx.tick < 9000 || tx.tick > 11000))
        return -EINVAL;
    return 0;
}

int64_t sys_clock_adjtime(int clk, void *buf)
{
    (void)clk;
    return sys_adjtimex(buf);
}
