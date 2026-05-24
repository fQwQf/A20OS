#include "ipc/timerfd.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/timer.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"

typedef struct {
    spinlock_t    lock;
    wait_queue_t  waiters;
    uint64_t interval_sec, interval_nsec;
    uint64_t value_sec, value_nsec;
    uint64_t expire_tick;
    int armed;
    int nonblock;
} timerfd_t;

static uint64_t timerfd_timespec_to_ticks(uint64_t sec, uint64_t nsec)
{
    return sec * TICKS_PER_SEC + (nsec * TICKS_PER_SEC + 999999999ULL) / 1000000000ULL;
}

static void timerfd_remaining_locked(timerfd_t *tfd, uint64_t out[4])
{
    memset(out, 0, sizeof(uint64_t) * 4);
    out[0] = tfd->interval_sec;
    out[1] = tfd->interval_nsec;
    if (!tfd->armed) return;
    uint64_t now = timer_get_ticks();
    uint64_t rem = tfd->expire_tick > now ? tfd->expire_tick - now : 0;
    out[2] = rem / TICKS_PER_SEC;
    out[3] = (rem % TICKS_PER_SEC) * 1000000000ULL / TICKS_PER_SEC;
}

static int timerfd_read(vfile_t *vf, char *buf, size_t count)
{
    timerfd_t *tfd = vf ? vf->priv : NULL;
    if (!tfd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;

    spin_lock(&tfd->lock);
    while (!tfd->armed || timer_get_ticks() < tfd->expire_tick) {
        if (tfd->nonblock) {
            spin_unlock(&tfd->lock);
            return -EAGAIN;
        }
        if (tfd->armed) {
            uint64_t remaining = tfd->expire_tick - timer_get_ticks();
            if (remaining > 0)
                proc_set_wake_time(proc_current(), tfd->expire_tick);
        }
        spin_unlock(&tfd->lock);
        wait_queue_sleep(&tfd->waiters);
        spin_lock(&tfd->lock);
    }
    uint64_t expirations = 1;
    uint64_t interval = timerfd_timespec_to_ticks(tfd->interval_sec, tfd->interval_nsec);
    if (interval) {
        uint64_t now = timer_get_ticks();
        if (now > tfd->expire_tick)
            expirations += (now - tfd->expire_tick) / interval;
        tfd->expire_tick += expirations * interval;
    } else {
        tfd->armed = 0;
    }
    spin_unlock(&tfd->lock);
    memcpy(buf, &expirations, sizeof(expirations));
    return sizeof(expirations);
}

static vfile_ops_t g_timerfd_ops = {
    .read = timerfd_read,
    .close = anonfd_free_priv_close,
};

int timerfd_create_file(int clockid, int flags)
{
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
    vf->flags = O_RDONLY | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_timerfd_ops;
    vf->priv = tfd;
    return anonfd_install_vfile(vf, flags);
}

int timerfd_settime_file(int gfd, int flags, const uint64_t new_value[4], uint64_t old_value[4])
{
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
    if (new_value[1] >= 1000000000ULL || new_value[3] >= 1000000000ULL) {
        spin_unlock(&tfd->lock);
        vfs_put_file_ref(gfd, vf);
        return -EINVAL;
    }
    tfd->interval_sec = new_value[0];
    tfd->interval_nsec = new_value[1];
    tfd->value_sec = new_value[2];
    tfd->value_nsec = new_value[3];
    uint64_t ticks = timerfd_timespec_to_ticks(new_value[2], new_value[3]);
    tfd->armed = ticks != 0;
    if (tfd->armed) {
        if (flags & 1)
            tfd->expire_tick = ticks;
        else
            tfd->expire_tick = timer_get_ticks() + ticks;
    }
    spin_unlock(&tfd->lock);
    if (tfd->armed)
        wait_queue_wake_all(&tfd->waiters);
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
