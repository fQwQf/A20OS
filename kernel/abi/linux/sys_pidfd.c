#include "syscall_impl.h"

#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/memfd.h"
#include "fs/vfs.h"
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

int64_t sys_pidfd_open(int pid, unsigned flags)
{
    if (pid <= 0)
        return -EINVAL;

    task_t *target = proc_find_get(pid);
    if (!target)
        return -ESRCH;
    if (target->state == PROC_ZOMBIE) {
        proc_put(target);
        return -ESRCH;
    }
    proc_put(target);

    task_t *self = proc_current();
    if (self && !proc_has_cap(self, CAP_SYS_PTRACE) &&
        !proc_task_may_access(self, target))
        return -EPERM;

    return linux_pidfd_create(pid, (int)flags);
}

int64_t sys_pidfd_getfd(int pidfd, int targetfd, unsigned flags)
{
    if (flags & ~O_CLOEXEC)
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

    if (targetfd < 0)
        return -EINVAL;

    task_t *self = proc_current();
    task_t *target = proc_find_get(pid);
    if (!target)
        return -ESRCH;
    if (target->state == PROC_ZOMBIE) {
        proc_put(target);
        return -ESRCH;
    }
    if (self && !proc_has_cap(self, CAP_SYS_PTRACE) &&
        !proc_task_may_access(self, target)) {
        proc_put(target);
        return -EPERM;
    }

    int target_gfd = fdtable_get(target, targetfd);
    if (target_gfd < 0) {
        proc_put(target);
        return -EBADF;
    }
    vfile_t *target_file = vfs_get_file_ref(target_gfd);
    if (!target_file) {
        proc_put(target);
        return -EBADF;
    }
    if (!memfd_secret_may_access(target_file, self)) {
        vfs_put_file_ref(target_gfd, target_file);
        proc_put(target);
        return -EACCES;
    }
    int r = fdtable_install_current(target_gfd, (int)flags);
    vfs_put_file_ref(target_gfd, target_file);
    proc_put(target);
    return r;
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
    task_t *target = proc_find_get(pid);
    if (!target)
        return -ESRCH;
    if (target->state == PROC_ZOMBIE) {
        proc_put(target);
        return -ESRCH;
    }
    if (!proc_has_cap(self, CAP_KILL) &&
        self->cred.uid != target->cred.uid &&
        self->cred.uid != target->cred.suid &&
        self->cred.euid != target->cred.uid &&
        self->cred.euid != target->cred.suid) {
        proc_put(target);
        return -EPERM;
    }
    if (sig == 0) {
        proc_put(target);
        return 0;
    }

    if (uinfo) {
        uint8_t info[SIGNAL_INFO_SIZE];
        if (copy_from_user(info, uinfo, sizeof(info)) < 0) {
            proc_put(target);
            return -EFAULT;
        }
        *(int *)info = sig;
        proc_put(target);
        return signal_send_info(pid, sig, info, sizeof(info));
    }
    proc_put(target);
    return signal_send(pid, sig);
}
