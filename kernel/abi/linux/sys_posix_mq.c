#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "core/timer.h"
#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "ipc/posix_mq.h"
#include "mm/slab.h"

/*
 * POSIX message queue syscalls.
 *
 * mq_open() returns a real fd whose private data holds the kernel mqd, so
 * close(2)/poll can interact with the queue through the fd table.  The mq
 * core (kernel/ipc/posix_mq.c) owns the queue objects and blocking protocol.
 */

typedef struct mq_fd {
    int mqd;
    int nonblock;
} mq_fd_t;

static int mq_fd_close(vfile_t *vf)
{
    mq_fd_t *mf = vf ? vf->priv : NULL;
    if (mf) {
        (void)posix_mq_close(mf->mqd);
        kfree(mf);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_mq_fd_ops = {
    .close = mq_fd_close,
};

static int mq_fd_from_gfd(int gfd, int *mqd, int *nonblock)
{
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_mq_fd_ops || !vf->priv) {
        vfs_put_file_ref(gfd, vf);
        return -EBADF;
    }
    mq_fd_t *mf = vf->priv;
    *mqd = mf->mqd;
    *nonblock = mf->nonblock;
    vfs_put_file_ref(gfd, vf);
    return 0;
}

int64_t sys_mq_open(const char *name, int oflag, int mode, const void *attr)
{
    if (!name)
        return -EFAULT;
    char kname[MQ_NAME_MAX];
    if (user_strncpy(kname, name, sizeof(kname)) < 0)
        return -EFAULT;

    mq_attr_kern_t kattr;
    const mq_attr_kern_t *kattrp = NULL;
    if (attr) {
        if (copy_from_user(&kattr, attr, sizeof(kattr)) < 0)
            return -EFAULT;
        kattrp = &kattr;
    }

    int mqd = posix_mq_open(kname, oflag, mode, kattrp);
    if (mqd < 0)
        return mqd;

    mq_fd_t *mf = kmalloc(sizeof(*mf));
    vfile_t *vf = vfile_alloc();
    if (!mf || !vf) {
        if (mf) kfree(mf);
        if (vf) vfile_free(vf);
        (void)posix_mq_close(mqd);
        return -ENOMEM;
    }
    mf->mqd = mqd;
    mf->nonblock = (oflag & 0x800) != 0; /* O_NONBLOCK */
    vfile_ref_init(vf, 1);
    vf->ops = &g_mq_fd_ops;
    vf->priv = mf;
    return anonfd_install_vfile(vf, oflag & 0x80000 /* O_CLOEXEC */);
}

int64_t sys_mq_unlink(const char *name)
{
    if (!name)
        return -EFAULT;
    char kname[MQ_NAME_MAX];
    if (user_strncpy(kname, name, sizeof(kname)) < 0)
        return -EFAULT;
    return posix_mq_unlink(kname);
}

int64_t sys_mq_getsetattr(int mqdes, const void *newattr, void *oldattr)
{
    int64_t gfd = fdtable_get_current(mqdes);
    if (gfd < 0)
        return -EBADF;
    int mqd, nb;
    int r = mq_fd_from_gfd((int)gfd, &mqd, &nb);
    if (r < 0)
        return r;

    mq_attr_kern_t knew;
    mq_attr_kern_t *knewp = NULL;
    if (newattr) {
        if (copy_from_user(&knew, newattr, sizeof(knew)) < 0)
            return -EFAULT;
        knewp = &knew;
    }
    mq_attr_kern_t kold;
    r = posix_mq_getsetattr(mqd, knewp, oldattr ? &kold : NULL);
    if (r < 0)
        return r;
    if (oldattr && copy_to_user(oldattr, &kold, sizeof(kold)) < 0)
        return -EFAULT;
    return r;
}

int64_t sys_mq_notify(int mqdes, const void *notification)
{
    int64_t gfd = fdtable_get_current(mqdes);
    if (gfd < 0)
        return -EBADF;
    int mqd, nb;
    int r = mq_fd_from_gfd((int)gfd, &mqd, &nb);
    if (r < 0)
        return r;
    return posix_mq_notify(mqd, notification);
}

struct linux_timespec_mq {
    int64_t tv_sec;
    int64_t tv_nsec;
};

static uint64_t mq_timeout_deadline(const void *timeout)
{
    if (!timeout)
        return 0;
    struct linux_timespec_mq ts;
    if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
        return (uint64_t)-1;
    if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
        return (uint64_t)-2;
    uint64_t ticks = (uint64_t)ts.tv_sec * TICKS_PER_SEC +
                     (uint64_t)ts.tv_nsec * TICKS_PER_SEC / 1000000000ULL;
    if ((ts.tv_sec || ts.tv_nsec) && ticks == 0)
        ticks = 1;
    return timer_get_ticks() + ticks;
}

int64_t sys_mq_timedsend(int mqdes, const char *msg_ptr, size_t msg_len,
                         unsigned msg_prio, const void *abs_timeout)
{
    int64_t gfd = fdtable_get_current(mqdes);
    if (gfd < 0)
        return -EBADF;
    int mqd, nb;
    int r = mq_fd_from_gfd((int)gfd, &mqd, &nb);
    if (r < 0)
        return r;
    uint64_t deadline = mq_timeout_deadline(abs_timeout);
    if (deadline == (uint64_t)-1)
        return -EFAULT;
    if (deadline == (uint64_t)-2)
        return -EINVAL;

    if (msg_len > 0) {
        if (!msg_ptr)
            return -EFAULT;
        char *kbuf = proc_scratch_buffer(msg_len);
        if (!kbuf)
            return -ENOMEM;
        if (copy_from_user(kbuf, msg_ptr, msg_len) < 0)
            return -EFAULT;
        return posix_mq_timedsend(mqd, kbuf, msg_len, msg_prio, deadline);
    }
    return posix_mq_timedsend(mqd, NULL, 0, msg_prio, deadline);
}

int64_t sys_mq_timedreceive(int mqdes, char *msg_ptr, size_t msg_len,
                            unsigned *msg_prio, const void *abs_timeout)
{
    int64_t gfd = fdtable_get_current(mqdes);
    if (gfd < 0)
        return -EBADF;
    int mqd, nb;
    int r = mq_fd_from_gfd((int)gfd, &mqd, &nb);
    if (r < 0)
        return r;
    uint64_t deadline = mq_timeout_deadline(abs_timeout);
    if (deadline == (uint64_t)-1)
        return -EFAULT;
    if (deadline == (uint64_t)-2)
        return -EINVAL;

    if (msg_len > 0 && !msg_ptr)
        return -EFAULT;
    char *kbuf = proc_scratch_buffer(msg_len ? msg_len : 1);
    if (!kbuf)
        return -ENOMEM;
    unsigned prio = 0;
    long got = posix_mq_timedreceive(mqd, kbuf, msg_len, &prio, deadline);
    if (got < 0)
        return got;
    if (msg_len > 0 && got > 0 &&
        copy_to_user(msg_ptr, kbuf, (size_t)got) < 0)
        return -EFAULT;
    if (msg_prio && copy_to_user(msg_prio, &prio, sizeof(prio)) < 0)
        return -EFAULT;
    return got;
}
