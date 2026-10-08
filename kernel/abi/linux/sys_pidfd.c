#include "syscall_impl.h"

#include "ipc/envelope.h"
#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/memfd.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc_internal.h"

typedef struct pidfd_file {
    int pid;
} pidfd_file_t;

/*
 * Linux pidfd semantics: poll() reports readable only after the referenced
 * thread group has exited.  A pidfd with no poll op fell through
 * vfs_poll_file() to POLLNVAL, which made an event loop that watches its
 * children spin at 100% CPU instead of blocking.
 */
static int pidfd_poll(vfile_t *vf, short events)
{
    pidfd_file_t *pf = vf ? (pidfd_file_t *)vf->priv : NULL;
    if (!pf)
        return POLLNVAL;

    task_t *t = proc_find_get(pf->pid);
    int exited = 1;
    if (t) {
        uint64_t flags = spin_lock_irqsave(&tasklist_lock);
        int state = proc_task_state_get(t);
        exited = state == PROC_UNUSED ||
                 (state == PROC_ZOMBIE && proc_tg_group_dead_locked(t));
        spin_unlock_irqrestore(&tasklist_lock, flags);
        proc_put(t);
    }
    if (!exited)
        return 0;

    short revents = POLLHUP;
    if (events & POLLIN)
        revents |= POLLIN;
    return revents;
}

static int pidfd_close(vfile_t *vf)
{
    return anonfd_free_priv_close(vf);
}

static vfile_ops_t g_pidfd_ops = {
    .poll = pidfd_poll,
    .close = pidfd_close,
};

int linux_pidfd_pid(int pidfd)
{
    vfile_t *vf = fdtable_get_current_file_ref(pidfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_pidfd_ops || !vf->priv) {
        vfs_put_file(vf);
        return -EBADF;
    }
    int pid = ((pidfd_file_t *)vf->priv)->pid;
    vfs_put_file(vf);
    return pid;
}

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

    task_t *target = proc_find_get_user(pid);
    if (!target)
        return -ESRCH;
    task_t *self = proc_current();
    int allowed = !self || proc_has_cap(self, CAP_SYS_PTRACE) ||
                  proc_task_may_access(self, target);
    if (!allowed) {
        proc_put(target);
        return -EPERM;
    }

    int fd = linux_pidfd_create(pid, (int)flags);
    proc_put(target);
    return fd;
}

int64_t sys_pidfd_getfd(int pidfd, int targetfd, unsigned flags)
{
    if (flags & ~O_CLOEXEC)
        return -EINVAL;

    vfile_t *vf = fdtable_get_current_file_ref(pidfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_pidfd_ops || !vf->priv) {
        vfs_put_file(vf);
        return -EBADF;
    }
    int pid = ((pidfd_file_t *)vf->priv)->pid;
    vfs_put_file(vf);

    if (targetfd < 0)
        return -EINVAL;

    task_t *self = proc_current();
    task_t *target = proc_find_get_user(pid);
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

    vfile_t *target_file = fdtable_get_file_ref(target, targetfd, NULL);
    if (!target_file) {
        proc_put(target);
        return -EBADF;
    }
    if (!memfd_secret_may_access(target_file, self)) {
        vfs_put_file(target_file);
        proc_put(target);
        return -EACCES;
    }
    /* Install into THIS task's table; the install consumes the lookup
     * reference. */
    int r = fdtable_install_vfile(self, target_file, (int)flags);
    proc_put(target);
    if (r < 0)
        return r;
    /* A7 acquire side: the stolen descriptor is a fresh authority entering
     * this task, so the receiver's envelope decides whether it may be used
     * (docs/research/05 §2.5.1).  Mediation keys on the NEW fd number, which
     * is a current-task gfd exactly like on the SCM_RIGHTS receive path -- the
     * foreign fd number it came from never needs to be meaningful.  A denied
     * fd is closed rather than left installed, so a denial cannot be
     * side-stepped by using the number afterwards. */
    if (env_active(self)) {
        int mr = env_mediate_acquire_gfd(r);
        if (mr) {
            fdtable_close_current(r);
            return mr;
        }
    }
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
    task_t *target = proc_find_get_user(pid);
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
