#include "ipc/timerfd.h"
#include "ipc/envelope.h"
#include "ipc/ipc.h"

#include "core/poll.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/readiness.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"

typedef struct {
    spinlock_t    lock;
    wait_queue_t  waiters;
    uint64_t interval_sec, interval_nsec;
    uint64_t expire_tick;
    int armed;
    int nonblock;
    int clockid;
    int cancel_on_set;
    uint64_t cancel_gen;
} timerfd_t;

#ifdef CONFIG_EVENT_DESCRIPTOR_STATE_RUST
uint64_t a20_timerfd_timespec_to_ticks_rust(uint64_t sec, uint64_t nsec,
                                             uint64_t ticks_per_sec);
void a20_timerfd_remaining_rust(uint64_t expire_tick, uint64_t now,
                                uint64_t ticks_per_sec, uint64_t *sec_out,
                                uint64_t *nsec_out);
uint64_t a20_timerfd_consume_expirations_rust(uint64_t now,
                                              uint64_t *expire_tick,
                                              uint64_t interval_ticks,
                                              int *armed);
uint64_t a20_timerfd_absolute_deadline_rust(uint64_t now_sec,
                                            uint64_t now_nsec,
                                            uint64_t absolute_sec,
                                            uint64_t absolute_nsec,
                                            uint64_t now_tick,
                                            uint64_t ticks_per_sec);
#endif

static uint64_t timerfd_timespec_to_ticks(uint64_t sec, uint64_t nsec)
{
#ifdef CONFIG_EVENT_DESCRIPTOR_STATE_RUST
    return a20_timerfd_timespec_to_ticks_rust(sec, nsec, TICKS_PER_SEC);
#else
    if (sec > UINT64_MAX / TICKS_PER_SEC ||
        nsec > UINT64_MAX / TICKS_PER_SEC)
        return UINT64_MAX;
    uint64_t whole = sec * TICKS_PER_SEC;
    uint64_t scaled_nsec = nsec * TICKS_PER_SEC;
    uint64_t fraction = scaled_nsec / 1000000000ULL +
                        (scaled_nsec % 1000000000ULL != 0);
    return whole > UINT64_MAX - fraction ? UINT64_MAX : whole + fraction;
#endif
}

/* Linux TFD_TIMER_CANCEL_ON_SET: an armed realtime-abstime timer becomes
 * readable-with-ECANCELED when the realtime clock is set discontinuously. */
static int timerfd_canceled_locked(timerfd_t *tfd)
{
    return tfd->armed && tfd->cancel_on_set &&
           timekeeping_realtime_set_generation() != tfd->cancel_gen;
}

static void timerfd_remaining_locked(timerfd_t *tfd, uint64_t out[4])
{
    memset(out, 0, sizeof(uint64_t) * 4);
    out[0] = tfd->interval_sec;
    out[1] = tfd->interval_nsec;
    if (!tfd->armed) return;
    uint64_t now = timer_get_ticks();
#ifdef CONFIG_EVENT_DESCRIPTOR_STATE_RUST
    a20_timerfd_remaining_rust(tfd->expire_tick, now, TICKS_PER_SEC,
                               &out[2], &out[3]);
#else
    uint64_t rem = tfd->expire_tick > now ? tfd->expire_tick - now : 0;
    out[2] = rem / TICKS_PER_SEC;
    out[3] = (rem % TICKS_PER_SEC) * 1000000000ULL / TICKS_PER_SEC;
#endif
}

static int timerfd_read(vfile_t *vf, char *buf, size_t count)
{
    timerfd_t *tfd = vf ? vf->priv : NULL;
    if (!tfd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;

    spin_lock(&tfd->lock);
    if (timerfd_canceled_locked(tfd)) {
        spin_unlock(&tfd->lock);
        return -ECANCELED;
    }
    while (!tfd->armed || timer_get_ticks() < tfd->expire_tick) {
        if (tfd->nonblock) {
            spin_unlock(&tfd->lock);
            return -EAGAIN;
        }
        uint64_t deadline = tfd->armed ? tfd->expire_tick : 0;
        spin_unlock(&tfd->lock);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        spin_lock(&tfd->lock);
        if (tfd->armed && timer_get_ticks() >= tfd->expire_tick) {
            spin_unlock(&tfd->lock);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            spin_lock(&tfd->lock);
            continue;
        }
        bool linked = wait_queue_link(&tfd->waiters, &entry, token, 0);
        spin_unlock(&tfd->lock);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&tfd->waiters, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
        spin_lock(&tfd->lock);
    }
    uint64_t expirations = 1;
    uint64_t interval = timerfd_timespec_to_ticks(tfd->interval_sec, tfd->interval_nsec);
#ifdef CONFIG_EVENT_DESCRIPTOR_STATE_RUST
    expirations = a20_timerfd_consume_expirations_rust(
        timer_get_ticks(), &tfd->expire_tick, interval, &tfd->armed);
#else
    if (interval) {
        uint64_t now = timer_get_ticks();
        if (now > tfd->expire_tick)
            expirations += (now - tfd->expire_tick) / interval;
        tfd->expire_tick += expirations * interval;
    } else {
        tfd->armed = 0;
    }
#endif
    spin_unlock(&tfd->lock);
    memcpy(buf, &expirations, sizeof(expirations));
    return sizeof(expirations);
}

static int timerfd_poll(vfile_t *vf, short events)
{
    timerfd_t *tfd = vf ? vf->priv : NULL;
    if (!tfd) return POLLNVAL;

    int revents = 0;
    spin_lock(&tfd->lock);
    if ((events & POLLIN) && timerfd_canceled_locked(tfd))
        revents |= POLLIN;
    else if ((events & POLLIN) && tfd->armed &&
             timer_get_ticks() >= tfd->expire_tick)
        revents |= POLLIN;
    spin_unlock(&tfd->lock);
    return revents;
}

static size_t timerfd_poll_sources(vfile_t *vf, short events,
                                   readiness_source_t *sources, size_t max)
{
    timerfd_t *tfd = vf ? vf->priv : NULL;
    if (!tfd || !sources || max == 0 || !(events & POLLIN))
        return 0;
    spin_lock(&tfd->lock);
    uint64_t now = timer_get_ticks();
    sources[0] = (readiness_source_t){
        .queue = &tfd->waiters,
        .key = 0,
        .deadline = tfd->armed && tfd->expire_tick > now ?
                    tfd->expire_tick : 0,
    };
    spin_unlock(&tfd->lock);
    return 1;
}

static vfile_ops_t g_timerfd_ops = {
    .read = timerfd_read,
    .poll = timerfd_poll,
    .poll_sources = timerfd_poll_sources,
    .close = anonfd_free_priv_close,
};

int timerfd_create_file(int clockid, int flags)
{
    /* CLOCK_REALTIME/MONOTONIC/BOOTTIME; the *_ALARM clocks need a
     * suspend/resume wake source this kernel does not have. */
    if (clockid != 0 && clockid != 1 && clockid != 7) return -EINVAL;
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    timerfd_t *tfd = kmalloc(sizeof(*tfd));
    vfile_t *vf = vfile_alloc();
    if (!tfd || !vf) {
        if (tfd) kfree(tfd);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    memset(tfd, 0, sizeof(*tfd));
    spin_init(&tfd->lock);
    wait_queue_init(&tfd->waiters);
    tfd->nonblock = (flags & O_NONBLOCK) != 0;
    tfd->clockid = clockid;
    vf->flags = O_RDONLY | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_timerfd_ops;
    vf->priv = tfd;
    if (env_kind_tag_vfile(vf, A20_OBJ_TIMER) < 0) {
        vfile_free(vf);
        kfree(tfd);
        return -EINVAL;
    }
    return anonfd_install_vfile(vf, flags);
}

int timerfd_settime_file(int gfd, int flags, const uint64_t new_value[4], uint64_t old_value[4])
{
    /* TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET are the only valid flags;
     * CANCEL_ON_SET is only meaningful together with ABSTIME. */
    if (flags & ~3) return -EINVAL;
    if ((flags & 2) && !(flags & 1)) return -EINVAL;
    if ((int64_t)new_value[0] < 0 || (int64_t)new_value[2] < 0) return -EINVAL;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf) return -EINVAL;
    if (vf->ops != &g_timerfd_ops) {
        vfs_put_file_ref(gfd, vf);
        return -EINVAL;
    }
    timerfd_t *tfd = vf->priv;
    spin_lock(&tfd->lock);
    if (old_value)
        timerfd_remaining_locked(tfd, old_value);
    if ((int64_t)new_value[1] < 0 || new_value[1] >= 1000000000ULL ||
        (int64_t)new_value[3] < 0 || new_value[3] >= 1000000000ULL) {
        spin_unlock(&tfd->lock);
        vfs_put_file_ref(gfd, vf);
        return -EINVAL;
    }
    uint64_t interval_ticks = timerfd_timespec_to_ticks(new_value[0], new_value[1]);
    uint64_t ticks = timerfd_timespec_to_ticks(new_value[2], new_value[3]);
    if (interval_ticks == UINT64_MAX || ticks == UINT64_MAX) {
        spin_unlock(&tfd->lock);
        vfs_put_file_ref(gfd, vf);
        return -EINVAL;
    }
    uint64_t new_expire_tick = 0;
    int new_armed = ticks != 0;
    int new_cancel_on_set = 0;
    uint64_t new_cancel_gen = 0;
    if (new_armed) {
        if (flags & 1) {
            /* TFD_TIMER_ABSTIME: value is absolute on the timer's clock. */
            uint64_t now_ts[2];
            if (tfd->clockid == 0)
                timekeeping_get_realtime(now_ts);
            else
                timekeeping_get_monotonic(now_ts);
#ifdef CONFIG_EVENT_DESCRIPTOR_STATE_RUST
            new_expire_tick = a20_timerfd_absolute_deadline_rust(
                now_ts[0], now_ts[1], new_value[2], new_value[3],
                timer_get_ticks(), TICKS_PER_SEC);
            if (new_expire_tick == UINT64_MAX) {
                spin_unlock(&tfd->lock);
                vfs_put_file_ref(gfd, vf);
                return -EINVAL;
            }
#else
            uint64_t delta_ticks = 0;
            if (new_value[2] > now_ts[0] ||
                (new_value[2] == now_ts[0] && new_value[3] > now_ts[1])) {
                uint64_t sec = new_value[2] - now_ts[0];
                uint64_t nsec;
                if (new_value[3] >= now_ts[1]) {
                    nsec = new_value[3] - now_ts[1];
                } else {
                    sec--;
                    nsec = 1000000000ULL + new_value[3] - now_ts[1];
                }
                delta_ticks = timerfd_timespec_to_ticks(sec, nsec);
            }
            /* A past absolute time expires immediately. */
            uint64_t now_tick = timer_get_ticks();
            if (delta_ticks == UINT64_MAX ||
                delta_ticks > UINT64_MAX - now_tick) {
                spin_unlock(&tfd->lock);
                vfs_put_file_ref(gfd, vf);
                return -EINVAL;
            }
            new_expire_tick = now_tick + delta_ticks;
#endif
            if ((flags & 2) && tfd->clockid == 0) {
                new_cancel_on_set = 1;
                new_cancel_gen = timekeeping_realtime_set_generation();
            }
        } else {
            uint64_t now_tick = timer_get_ticks();
            if (ticks > UINT64_MAX - now_tick) {
                spin_unlock(&tfd->lock);
                vfs_put_file_ref(gfd, vf);
                return -EINVAL;
            }
            new_expire_tick = now_tick + ticks;
        }
    }
    tfd->interval_sec = new_value[0];
    tfd->interval_nsec = new_value[1];
    tfd->armed = new_armed;
    tfd->expire_tick = new_expire_tick;
    tfd->cancel_on_set = new_cancel_on_set;
    tfd->cancel_gen = new_cancel_gen;
    spin_unlock(&tfd->lock);
    wait_queue_wake_all(&tfd->waiters, 0, PROC_WAKE_EVENT);
    vfs_put_file_ref(gfd, vf);
    return 0;
}

int timerfd_gettime_file(int gfd, uint64_t curr_value[4])
{
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf) return -EINVAL;
    if (vf->ops != &g_timerfd_ops) {
        vfs_put_file_ref(gfd, vf);
        return -EINVAL;
    }
    timerfd_t *tfd = vf->priv;
    spin_lock(&tfd->lock);
    timerfd_remaining_locked(tfd, curr_value);
    spin_unlock(&tfd->lock);
    vfs_put_file_ref(gfd, vf);
    return 0;
}
