#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "core/timer.h"
#include "core/timekeeping.h"
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

static int mq_fd_from_gfd(int gfd, int *mqd, int *nonblock, vfile_t **held)
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
    *held = vf;
    return 0;
}

#define MQ_FD_RETURN(gfd, vf, result) do { \
    vfs_put_file_ref((gfd), (vf));          \
    return (result);                        \
} while (0)

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
    vfile_t *held;
    int r = mq_fd_from_gfd((int)gfd, &mqd, &nb, &held);
    if (r < 0)
        return r;

    mq_attr_kern_t knew;
    mq_attr_kern_t *knewp = NULL;
    if (newattr) {
        if (copy_from_user(&knew, newattr, sizeof(knew)) < 0)
            MQ_FD_RETURN((int)gfd, held, -EFAULT);
        knewp = &knew;
    }
    mq_attr_kern_t kold;
    r = posix_mq_getsetattr(mqd, knewp, oldattr ? &kold : NULL);
    if (r < 0)
        MQ_FD_RETURN((int)gfd, held, r);
    if (oldattr && copy_to_user(oldattr, &kold, sizeof(kold)) < 0)
        MQ_FD_RETURN((int)gfd, held, -EFAULT);
    MQ_FD_RETURN((int)gfd, held, r);
}

int64_t sys_mq_notify(int mqdes, const void *notification)
{
    int64_t gfd = fdtable_get_current(mqdes);
    if (gfd < 0)
        return -EBADF;
    int mqd, nb;
    vfile_t *held;
    int r = mq_fd_from_gfd((int)gfd, &mqd, &nb, &held);
    if (r < 0)
        return r;
    r = posix_mq_notify(mqd, notification);
    MQ_FD_RETURN((int)gfd, held, r);
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
    uint64_t now_rt[2];
    timekeeping_get_realtime(now_rt);
    uint64_t now_ticks = timer_get_ticks();
    if ((uint64_t)ts.tv_sec < now_rt[0] ||
        ((uint64_t)ts.tv_sec == now_rt[0] &&
         (uint64_t)ts.tv_nsec <= now_rt[1]))
        return now_ticks;

    uint64_t sec = (uint64_t)ts.tv_sec - now_rt[0];
    int64_t nsec = ts.tv_nsec - (int64_t)now_rt[1];
    if (nsec < 0) {
        sec--;
        nsec += 1000000000LL;
    }
    if (sec > (UINT64_MAX - now_ticks) / TICKS_PER_SEC)
        return UINT64_MAX - 2;
    uint64_t ticks = sec * TICKS_PER_SEC +
                     ((uint64_t)nsec * TICKS_PER_SEC + 999999999ULL) /
                     1000000000ULL;
    if (ticks == 0)
        ticks = 1;
    if (ticks > UINT64_MAX - 2 - now_ticks)
        return UINT64_MAX - 2;
    return now_ticks + ticks;
}

int64_t sys_mq_timedsend(int mqdes, const char *msg_ptr, size_t msg_len,
                         unsigned msg_prio, const void *abs_timeout)
{
    int64_t gfd = fdtable_get_current(mqdes);
    if (gfd < 0)
        return -EBADF;
    int mqd, nb;
    vfile_t *held;
    int r = mq_fd_from_gfd((int)gfd, &mqd, &nb, &held);
    if (r < 0)
        return r;
    uint64_t deadline = mq_timeout_deadline(abs_timeout);
    if (deadline == (uint64_t)-1)
        MQ_FD_RETURN((int)gfd, held, -EFAULT);
    if (deadline == (uint64_t)-2)
        MQ_FD_RETURN((int)gfd, held, -EINVAL);

    if (msg_len > 0) {
        if (!msg_ptr)
            MQ_FD_RETURN((int)gfd, held, -EFAULT);
        char *kbuf = proc_scratch_buffer(msg_len);
        if (!kbuf)
            MQ_FD_RETURN((int)gfd, held, -ENOMEM);
        if (copy_from_user(kbuf, msg_ptr, msg_len) < 0)
            MQ_FD_RETURN((int)gfd, held, -EFAULT);
        r = posix_mq_timedsend(mqd, kbuf, msg_len, msg_prio, deadline);
        MQ_FD_RETURN((int)gfd, held, r);
    }
    r = posix_mq_timedsend(mqd, NULL, 0, msg_prio, deadline);
    MQ_FD_RETURN((int)gfd, held, r);
}

int64_t sys_mq_timedreceive(int mqdes, char *msg_ptr, size_t msg_len,
                            unsigned *msg_prio, const void *abs_timeout)
{
    int64_t gfd = fdtable_get_current(mqdes);
    if (gfd < 0)
        return -EBADF;
    int mqd, nb;
    vfile_t *held;
    int r = mq_fd_from_gfd((int)gfd, &mqd, &nb, &held);
    if (r < 0)
        return r;
    uint64_t deadline = mq_timeout_deadline(abs_timeout);
    if (deadline == (uint64_t)-1)
        MQ_FD_RETURN((int)gfd, held, -EFAULT);
    if (deadline == (uint64_t)-2)
        MQ_FD_RETURN((int)gfd, held, -EINVAL);

    if (msg_len > 0 && !msg_ptr)
        MQ_FD_RETURN((int)gfd, held, -EFAULT);
    char *kbuf = proc_scratch_buffer(msg_len ? msg_len : 1);
    if (!kbuf)
        MQ_FD_RETURN((int)gfd, held, -ENOMEM);
    unsigned prio = 0;
    long got = posix_mq_timedreceive(mqd, kbuf, msg_len, &prio, deadline);
    if (got < 0)
        MQ_FD_RETURN((int)gfd, held, got);
    if (msg_len > 0 && got > 0 &&
        copy_to_user(msg_ptr, kbuf, (size_t)got) < 0)
        MQ_FD_RETURN((int)gfd, held, -EFAULT);
    if (msg_prio && copy_to_user(msg_prio, &prio, sizeof(prio)) < 0)
        MQ_FD_RETURN((int)gfd, held, -EFAULT);
    MQ_FD_RETURN((int)gfd, held, got);
}
