#include "syscall_impl.h"

#include "fs/fanotify.h"
#include "fs/fdtable.h"

static const unsigned FANOTIFY_INIT_ALLOWED =
    FAN_CLOEXEC | FAN_NONBLOCK | FAN_CLASS_NOTIF | FAN_REPORT_FID |
    FAN_REPORT_DFID_NAME;

int64_t sys_fanotify_init(unsigned flags, unsigned event_f_flags)
{
    (void)event_f_flags;
    if ((flags & FAN_ALL_CLASS_BITS) != FAN_CLASS_NOTIF)
        return -EINVAL;
    if (flags & ~FANOTIFY_INIT_ALLOWED)
        return -EINVAL;
    return fanotify_create_file((int)flags, (int)event_f_flags);
}

int64_t sys_fanotify_mark(int fanotify_fd, unsigned flags, uint64_t mask,
                          int dfd, const char *path)
{
    if (!path)
        return -EFAULT;

    int64_t gfd = fdtable_get_current(fanotify_fd);
    if (gfd < 0)
        return -EBADF;

    /* path is a user pointer: copy it in before syscall_path_at, which
     * expects a kernel buffer (direct user strlen is an illegal kernel
     * access with SUM cleared). */
    char kpath[MAX_PATH_LEN];
    long pr = user_path_strncpy(kpath, path, sizeof(kpath));
    if (pr < 0)
        return pr;

    char full[MAX_PATH_LEN];
    int r = syscall_path_at(dfd, kpath, full, sizeof(full));
    if (r < 0)
        return r;

    return fanotify_mark((int)gfd, flags, mask, dfd, full);
}
