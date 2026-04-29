#include "syscall_impl.h"

enum {
    CLOCK_REALTIME_ID = 0,
    CLOCK_MONOTONIC_ID = 1,
    CLOCK_PROCESS_CPUTIME_ID = 2,
    CLOCK_THREAD_CPUTIME_ID = 3,
    CLOCK_MONOTONIC_RAW_ID = 4,
    CLOCK_REALTIME_COARSE_ID = 5,
    CLOCK_MONOTONIC_COARSE_ID = 6,
    CLOCK_BOOTTIME_ID = 7,
    CLOCK_REALTIME_ALARM_ID = 8,
    CLOCK_BOOTTIME_ALARM_ID = 9,
    CLOCK_TAI_ID = 11,
};

static int clock_is_realtime(int clk) {
    switch (clk) {
    case CLOCK_REALTIME_ID:
    case CLOCK_REALTIME_COARSE_ID:
    case CLOCK_REALTIME_ALARM_ID:
    case CLOCK_TAI_ID:
        return 1;
    default:
        return 0;
    }
}

static int clock_is_monotonic(int clk) {
    switch (clk) {
    case CLOCK_MONOTONIC_ID:
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
    case CLOCK_MONOTONIC_RAW_ID:
    case CLOCK_MONOTONIC_COARSE_ID:
    case CLOCK_BOOTTIME_ID:
    case CLOCK_BOOTTIME_ALARM_ID:
        return 1;
    default:
        return 0;
    }
}

int64_t sys_clock_settime(int clk, void *tp) {
    int64_t ts[2];
    if (clk != CLOCK_REALTIME_ID) return -EINVAL;
    if (!tp) return -EFAULT;
    if (copy_from_user(ts, tp, sizeof(ts)) < 0) return -EFAULT;
    if (ts[0] < 0 || ts[1] < 0 || ts[1] >= 1000000000LL) return -EINVAL;
    return timekeeping_set_realtime((uint64_t)ts[0], (uint64_t)ts[1]);
}

int64_t sys_clock_gettime(int clk, void *tp) {
    uint64_t ts[2];
    if (!tp) return -EFAULT;
    /* glibc prefers CLOCK_REALTIME_COARSE for date/time formatting. */
    if (clock_is_realtime(clk)) timekeeping_get_realtime(ts);
    else if (clock_is_monotonic(clk)) timekeeping_get_monotonic(ts);
    else return -EINVAL;
    if (copy_to_user(tp, ts, sizeof(ts)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_clock_getres(int clk, void *tp) {
    if (!clock_is_realtime(clk) && !clock_is_monotonic(clk))
        return -EINVAL;
    if (tp) {
        uint64_t ts[2];
        ts[0] = 0;
        ts[1] = 1000000000UL / TICKS_PER_SEC;
        if (copy_to_user(tp, ts, sizeof(ts)) < 0) return -EFAULT;
    }
    return 0;
}

int64_t sys_nanosleep(void *req, void *rem) {
    (void)rem;
    if (!req) return -EFAULT;
    uint64_t ts[2];
    if (copy_from_user(ts, req, sizeof(ts)) < 0) return -EFAULT;
    uint64_t sec  = ts[0];
    uint64_t nsec = ts[1];
    uint64_t ticks = sec * TICKS_PER_SEC + nsec * TICKS_PER_SEC / 1000000000UL;
    uint64_t until = timer_get_ticks() + ticks;

    task_t *t = proc_current();
    if (t) {
        if (syscall_sleep_diag_count < 128) {
            syscall_sleep_diag_count++;
            kdebug("[SLEEPDBG] enter pid=%d ticks=%lu until=%lu now=%lu\n",
                  t->pid, (unsigned long)ticks, (unsigned long)until,
                  (unsigned long)timer_get_ticks());
        }
        proc_set_wake_time(t, until);
        t->state     = PROC_BLOCKED;
        sched();
        if (syscall_sleep_diag_count < 128) {
            syscall_sleep_diag_count++;
            kdebug("[SLEEPDBG] ret pid=%d now=%lu wake=%lu state=%d\n",
                  t->pid, (unsigned long)timer_get_ticks(),
                  (unsigned long)t->wake_time, t->state);
        }
    } else {
        while (timer_get_ticks() < until) __asm__ volatile("nop");
    }
    return 0;
}

int64_t sys_gettimeofday(void *tv, void *tz) {
    (void)tz;
    if (tv) {
        uint64_t t[2];
        uint64_t ts[2];
        timekeeping_get_realtime(ts);
        t[0] = ts[0];
        t[1] = ts[1] / 1000ULL;
        if (copy_to_user(tv, t, sizeof(t)) < 0) return -EFAULT;
    }
    return 0;
}

int64_t sys_settimeofday(void *tv, void *tz) {
    uint64_t t[2];
    (void)tz;
    if (!tv) return -EINVAL;
    if (copy_from_user(t, tv, sizeof(t)) < 0) return -EFAULT;
    if (t[1] >= 1000000ULL) return -EINVAL;
    return timekeeping_set_realtime(t[0], t[1] * 1000ULL);
}

int64_t sys_times(void *buf) {
    task_t *t = proc_current();
    if (t && buf) {
        uint64_t tm[4];
        memset(tm, 0, sizeof(tm));
        tm[0] = t->total_time;
        if (copy_to_user(buf, tm, sizeof(tm)) < 0) return -EFAULT;
    }
    return (int64_t)(timer_get_ticks());
}

int64_t sys_time(long *tloc) {
    uint64_t ts[2];
    uint64_t t;
    timekeeping_get_realtime(ts);
    t = ts[0];
    if (tloc) {
        long tl = (long)t;
        if (copy_to_user(tloc, &tl, sizeof(long)) < 0) return -EFAULT;
    }
    return (int64_t)t;
}
