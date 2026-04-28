#include "syscall_internal.h"

typedef struct {
    uint64_t counter;
    int semaphore;
    int nonblock;
} eventfd_t;

static int eventfd_read(vfile_t *vf, char *buf, size_t count)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;
    while (efd->counter == 0) {
        if (efd->nonblock) return -EAGAIN;
        proc_yield();
    }
    uint64_t val = efd->semaphore ? 1 : efd->counter;
    efd->counter -= val;
    memcpy(buf, &val, sizeof(val));
    return sizeof(val);
}

static int eventfd_write(vfile_t *vf, const char *buf, size_t count)
{
    eventfd_t *efd = vf ? vf->priv : NULL;
    if (!efd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;
    uint64_t val;
    memcpy(&val, buf, sizeof(val));
    if (val == ~0ULL) return -EINVAL;
    efd->counter += val;
    return sizeof(val);
}

static int anonfd_free_priv_close(vfile_t *vf)
{
    if (vf && vf->priv) {
        kfree(vf->priv);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_eventfd_ops = {
    .read = eventfd_read,
    .write = eventfd_write,
    .close = anonfd_free_priv_close,
};

static int anonfd_install_vfile(vfile_t *vf, int flags)
{
    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        if (vf->ops && vf->ops->close) vf->ops->close(vf);
        kfree(vf);
        return -EMFILE;
    }
    return syscall_alloc_local_fd_with_flags(proc_current(), gfd, flags);
}

int64_t sys_eventfd2(unsigned initval, int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK | 1)) return -EINVAL;
    eventfd_t *efd = kmalloc(sizeof(*efd));
    vfile_t *vf = kmalloc(sizeof(*vf));
    if (!efd || !vf) {
        if (efd) kfree(efd);
        if (vf) kfree(vf);
        return -ENOMEM;
    }
    memset(efd, 0, sizeof(*efd));
    memset(vf, 0, sizeof(*vf));
    efd->counter = initval;
    efd->semaphore = (flags & 1) != 0;
    efd->nonblock = (flags & O_NONBLOCK) != 0;
    vf->flags = O_RDWR | (flags & O_NONBLOCK);
    vf->ref_count = 1;
    vf->ops = &g_eventfd_ops;
    vf->priv = efd;
    return anonfd_install_vfile(vf, flags);
}

typedef struct {
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

static void timerfd_remaining(timerfd_t *tfd, uint64_t out[4])
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
    while (!tfd->armed || timer_get_ticks() < tfd->expire_tick) {
        if (tfd->nonblock) return -EAGAIN;
        proc_yield();
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
    memcpy(buf, &expirations, sizeof(expirations));
    return sizeof(expirations);
}

static vfile_ops_t g_timerfd_ops = {
    .read = timerfd_read,
    .close = anonfd_free_priv_close,
};

int64_t sys_timerfd_create(int clockid, int flags)
{
    if (clockid != 0 && clockid != 1 && clockid != 7) return -EINVAL;
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    timerfd_t *tfd = kmalloc(sizeof(*tfd));
    vfile_t *vf = kmalloc(sizeof(*vf));
    if (!tfd || !vf) {
        if (tfd) kfree(tfd);
        if (vf) kfree(vf);
        return -ENOMEM;
    }
    memset(tfd, 0, sizeof(*tfd));
    memset(vf, 0, sizeof(*vf));
    tfd->nonblock = (flags & O_NONBLOCK) != 0;
    vf->flags = O_RDONLY | (flags & O_NONBLOCK);
    vf->ref_count = 1;
    vf->ops = &g_timerfd_ops;
    vf->priv = tfd;
    return anonfd_install_vfile(vf, flags);
}

int64_t sys_timerfd_settime(int fd, int flags, const void *new_value, void *old_value)
{
    if (!new_value) return -EFAULT;
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file((int)gfd);
    if (!vf || vf->ops != &g_timerfd_ops) return -EINVAL;
    timerfd_t *tfd = vf->priv;
    if (old_value) {
        uint64_t old[4];
        timerfd_remaining(tfd, old);
        if (copy_to_user(old_value, old, sizeof(old)) < 0) return -EFAULT;
    }
    uint64_t ts[4];
    if (copy_from_user(ts, new_value, sizeof(ts)) < 0) return -EFAULT;
    if (ts[1] >= 1000000000ULL || ts[3] >= 1000000000ULL) return -EINVAL;
    tfd->interval_sec = ts[0];
    tfd->interval_nsec = ts[1];
    tfd->value_sec = ts[2];
    tfd->value_nsec = ts[3];
    uint64_t ticks = timerfd_timespec_to_ticks(ts[2], ts[3]);
    tfd->armed = ticks != 0;
    if (tfd->armed) {
        if (flags & 1)
            tfd->expire_tick = ticks;
        else
            tfd->expire_tick = timer_get_ticks() + ticks;
    }
    return 0;
}

int64_t sys_timerfd_gettime(int fd, void *curr_value)
{
    if (!curr_value) return -EFAULT;
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file((int)gfd);
    if (!vf || vf->ops != &g_timerfd_ops) return -EINVAL;
    uint64_t ts[4];
    timerfd_remaining((timerfd_t *)vf->priv, ts);
    return copy_to_user(curr_value, ts, sizeof(ts)) < 0 ? -EFAULT : 0;
}

static int inotify_empty_read(vfile_t *vf, char *buf, size_t count)
{
    (void)vf; (void)buf; (void)count;
    return -EAGAIN;
}

static vfile_ops_t g_inotify_ops = {
    .read = inotify_empty_read,
};

int64_t sys_inotify_init1(int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    vfile_t *vf = kmalloc(sizeof(*vf));
    if (!vf) return -ENOMEM;
    memset(vf, 0, sizeof(*vf));
    vf->flags = O_RDONLY | (flags & O_NONBLOCK);
    vf->ref_count = 1;
    vf->ops = &g_inotify_ops;
    return anonfd_install_vfile(vf, flags);
}
