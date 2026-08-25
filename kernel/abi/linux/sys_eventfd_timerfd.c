#include "syscall_impl.h"

#include "fs/inotify.h"
#include "ipc/eventfd.h"
#include "ipc/timerfd.h"
#include "ipc/envelope.h"
#include "ipc/ipc.h"

int64_t sys_eventfd2(unsigned initval, int flags)
{
    int ufd = eventfd_create(initval, flags);
    if (ufd < 0)
        return ufd;
    int gfd = fdtable_get_current(ufd);
    env_kind_register(gfd, A20_OBJ_EVENT_QUEUE);
    if (env_active(proc_current())) {
        uint64_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT;
        int mr = env_mediate_acquire((uint8_t)A20_OBJ_EVENT_QUEUE,
                                     rights, gfd);
        if (mr) {
            fdtable_close_current(ufd);
            return mr;
        }
    }
    return ufd;
}

int64_t sys_timerfd_create(int clockid, int flags)
{
    int ufd = timerfd_create_file(clockid, flags);
    if (ufd < 0)
        return ufd;
    int gfd = fdtable_get_current(ufd);
    env_kind_register(gfd, A20_OBJ_TIMER);
    if (env_active(proc_current())) {
        uint64_t rights = A20_RIGHT_READ | A20_RIGHT_STAT;
        int mr = env_mediate_acquire((uint8_t)A20_OBJ_TIMER, rights, gfd);
        if (mr) {
            fdtable_close_current(ufd);
            return mr;
        }
    }
    return ufd;
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

int64_t sys_inotify_add_watch(int fd, const char *pathname, uint32_t mask)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return -EBADF;
    if (!pathname) return -EFAULT;

    char path[MAX_PATH_LEN];
    long pr0 = user_path_strncpy(path, pathname, sizeof(path));
    if (pr0 < 0)
        return pr0;

    return inotify_add_watch((int)gfd, path, mask);
}

int64_t sys_inotify_rm_watch(int fd, int wd)
{
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return -EBADF;
    return inotify_rm_watch((int)gfd, wd);
}
