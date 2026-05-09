#include "syscall_impl.h"

#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "mm/slab.h"

typedef struct pidfd_file {
    int pid;
} pidfd_file_t;

static int pidfd_close(vfile_t *vf)
{
    return anonfd_free_priv_close(vf);
}

static vfile_ops_t g_pidfd_ops = {
    .close = pidfd_close,
};

int linux_pidfd_create(int pid, int flags)
{
    if (flags & ~O_CLOEXEC)
        return -EINVAL;

    pidfd_file_t *pf = (pidfd_file_t *)kmalloc(sizeof(*pf));
    if (!pf)
        return -ENOMEM;
    pf->pid = pid;

    vfile_t *vf = vfile_alloc();
    if (!vf) {
        kfree(pf);
        return -ENOMEM;
    }
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_pidfd_ops;
    vf->priv = pf;
    return anonfd_install_vfile(vf, flags);
}

int64_t sys_pidfd_send_signal(int pidfd, int sig, void *uinfo, unsigned flags)
{
    if (flags)
        return -EINVAL;
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;

    int gfd = fdtable_get_current(pidfd);
    if (gfd < 0)
        return gfd;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_pidfd_ops || !vf->priv) {
        vfs_put_file_ref(gfd, vf);
        return -EBADF;
    }

    int pid = ((pidfd_file_t *)vf->priv)->pid;
    vfs_put_file_ref(gfd, vf);

    task_t *self = proc_current();
    task_t *target = proc_find(pid);
    if (!target || target->state == PROC_ZOMBIE)
        return -ESRCH;
    if (!proc_has_cap(self, CAP_KILL) &&
        self->cred.uid != target->cred.uid &&
        self->cred.uid != target->cred.suid &&
        self->cred.euid != target->cred.uid &&
        self->cred.euid != target->cred.suid)
        return -EPERM;
    if (sig == 0)
        return 0;

    if (uinfo) {
        uint8_t info[SIGNAL_INFO_SIZE];
        if (copy_from_user(info, uinfo, sizeof(info)) < 0)
            return -EFAULT;
        *(int *)info = sig;
        return signal_send_info(pid, sig, info, sizeof(info));
    }
    return signal_send(pid, sig);
}
