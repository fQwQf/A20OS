#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "proc/proc_internal.h"
#include "sleep_abi.h"

static int sleep_clock_is_realtime(int clk) {
    return clk == 0 || clk == 5 || clk == 8 || clk == 11;
}
static int sleep_clock_is_monotonic(int clk) {
    return clk == 1 || clk == 4 || clk == 6 || clk == 7 || clk == 9;
}

int64_t sys_clock_nanosleep_abi(int clk, int flags, const void *req, void *rem,
                                int time32) {
    const int TIMER_ABSTIME = 1;
    if (flags & ~TIMER_ABSTIME)
        return -EINVAL;
    if (!req)
        return -EFAULT;

    /* CLOCK_PROCESS/THREAD_CPUTIME_ID sleeps are not modeled; refuse them
     * rather than silently sleeping on the monotonic clock. */
    if (clk == 2 || clk == 3)
        return -EOPNOTSUPP;
    int realtime = sleep_clock_is_realtime(clk);
    int monotonic = sleep_clock_is_monotonic(clk);
    if (!realtime && !monotonic)
        return -EINVAL;

    uint64_t ts[2];
    int decoded = linux_sleep_read_timespec(req, time32, ts);
    if (decoded < 0)
        return decoded;

    uint64_t until;
    if (flags & TIMER_ABSTIME) {
        uint64_t now_ts[2];
        if (realtime)
            timekeeping_get_realtime(now_ts);
        else
            timekeeping_get_monotonic(now_ts);

        if (ts[0] < now_ts[0] || (ts[0] == now_ts[0] && ts[1] <= now_ts[1]))
            return 0;

        uint64_t sec = ts[0] - now_ts[0];
        uint64_t nsec;
        if (ts[1] >= now_ts[1]) {
            nsec = ts[1] - now_ts[1];
        } else {
            if (sec == 0)
                return 0;
            sec--;
            nsec = 1000000000ULL + ts[1] - now_ts[1];
        }
        until = linux_sleep_deadline(sec, nsec, timer_get_ticks());
    } else {
        if (ts[0] == 0 && ts[1] == 0)
            return 0;
        until = linux_sleep_deadline(ts[0], ts[1], timer_get_ticks());
    }

    task_t *t = proc_current();
    if (!t) {
        while (timer_get_ticks() < until)
            cpu_relax();
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
                int copied = linux_sleep_write_timespec(rem, time32, out);
                if (copied < 0)
                    return copied;
            }
            return -EINTR;
        }
        if (timer_get_ticks() >= until)
            return 0;
    }
}

int64_t sys_clock_nanosleep(int clk, int flags, const void *req, void *rem) {
    return sys_clock_nanosleep_abi(clk, flags, req, rem, 0);
}

int64_t sys_nanosleep_abi(const void *req, void *rem, int time32) {
    return sys_clock_nanosleep_abi(1, 0, req, rem, time32);
}

int64_t sys_nanosleep(void *req, void *rem) {
    return sys_nanosleep_abi(req, rem, 0);
}
