#include "syscall_impl.h"

#include "fs/inotify.h"
#include "ipc/eventfd.h"
#include "ipc/timerfd.h"

int64_t sys_eventfd2(unsigned initval, int flags)
{
    return eventfd_create(initval, flags);
}

int64_t sys_timerfd_create(int clockid, int flags)
{
    return timerfd_create_file(clockid, flags);
}

int64_t sys_timerfd_settime(int fd, int flags, const void *new_value, void *old_value)
{
    if (!new_value) return -EFAULT;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    uint64_t ts[4];
    if (copy_from_user(ts, new_value, sizeof(ts)) < 0) return -EFAULT;
    uint64_t old[4];
    int r = timerfd_settime_file((int)gfd, flags, ts, old_value ? old : NULL);
    if (r < 0) return r;
    if (old_value && copy_to_user(old_value, old, sizeof(old)) < 0)
        return -EFAULT;
    return r;
}

int64_t sys_timerfd_gettime(int fd, void *curr_value)
{
    if (!curr_value) return -EFAULT;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    uint64_t ts[4];
    int r = timerfd_gettime_file((int)gfd, ts);
    if (r < 0) return r;
    return copy_to_user(curr_value, ts, sizeof(ts)) < 0 ? -EFAULT : 0;
}

int64_t sys_inotify_init1(int flags)
{
    return inotify_create_file(flags);
}

int64_t sys_inotify_init(int flags)
{
    return inotify_create_file(flags);
}

int64_t sys_inotify_add_watch(int fd, const char *pathname, uint32_t mask)
{
    (void)pathname;
    (void)mask;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return -EBADF;
    return 1;
}

int64_t sys_inotify_rm_watch(int fd, int wd)
{
    (void)wd;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return -EBADF;
    return 0;
}
