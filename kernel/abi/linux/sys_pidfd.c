#include "syscall_impl.h"

#include "ipc/envelope.h"
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
#include "ipc/envelope_acquire_race_test.h"
#endif
#include "ipc/ipc.h"
#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/memfd.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc_internal.h"
#include "abi/linux/pidfd.h"

typedef struct pidfd_file {
    task_t *task;
    /* Linux keeps reporting the target pid in fdinfo after the task exits,
     * so the number is captured when the pidfd is created. */
    int pid;
} pidfd_file_t;

void a20_pidfd_state_init_rust(void **task, void *value);
void *a20_pidfd_state_task_rust(void *const *task);
bool a20_pidfd_state_drop_rust(
    void **task, void (*release)(void *));
bool a20_pidfd_group_exited_rust(bool task_found, bool state_unused,
                                 bool state_zombie, bool thread_group_dead);
int a20_pidfd_poll_events_rust(bool exited, int events);

/*
 * Linux pidfd semantics: poll() reports readable only after the referenced
 * thread group has exited.  A pidfd with no poll op fell through
 * vfs_poll_file() to POLLNVAL, which made an event loop that watches its
 * children spin at 100% CPU instead of blocking.
 */
static int pidfd_poll(vfile_t *vf, short events)
{
    pidfd_file_t *pf = vf ? (pidfd_file_t *)vf->priv : NULL;
    task_t *target = pf ? pf->task : NULL;
    if (pf)
        target = a20_pidfd_state_task_rust((void *const *)&pf->task);
    if (!target)
        return POLLNVAL;

    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    uint64_t pf_flags = spin_lock_irqsave(&target->park_lock);
    int state = target->state;
    int off_cpu = !proc_task_is_current_any_cpu(target);
    spin_unlock_irqrestore(&target->park_lock, pf_flags);
    int exited = a20_pidfd_group_exited_rust(
        true, state == PROC_UNUSED, state == PROC_ZOMBIE,
        state == PROC_ZOMBIE && off_cpu && proc_tg_group_dead_locked(target));
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return a20_pidfd_poll_events_rust(exited != 0, events);
}

static void pidfd_release_task_ref(void *value);

/* Once the task pin has been stored in pidfd_file_t, release it through the
 * selected state owner on every teardown path, including vfile allocation
 * failure before the pidfd is published. */
static void pidfd_state_release(pidfd_file_t *pf)
{
    if (!pf)
        return;
    (void)a20_pidfd_state_drop_rust((void **)&pf->task,
                                    pidfd_release_task_ref);
}

static int pidfd_close(vfile_t *vf)
{
    pidfd_file_t *pf = vf ? (pidfd_file_t *)vf->priv : NULL;
    if (!pf)
        return 0;
    pidfd_state_release(pf);
    vf->priv = NULL;
    kfree(pf);
    return 0;
}

static void pidfd_release_task_ref(void *value)
{
    proc_put((task_t *)value);
}

static vfile_ops_t g_pidfd_ops = {
    .poll = pidfd_poll,
    .close = pidfd_close,
};

static int pidfd_target_exited(task_t *target)
{
    if (!target)
        return 1;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    int state = proc_task_state_get(target);
    int exited = state == PROC_UNUSED ||
                 (state == PROC_ZOMBIE &&
                  proc_tg_group_dead_locked(target));
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return exited;
}

task_t *linux_pidfd_get_task_ref(int pidfd)
{
    vfile_t *vf = fdtable_get_current_file_ref(pidfd);
    if (!vf)
        return NULL;
    if (vf->ops != &g_pidfd_ops || !vf->priv) {
        vfs_put_file(vf);
        return NULL;
    }
    pidfd_file_t *pf = (pidfd_file_t *)vf->priv;
    task_t *target = pf->task;
    target = a20_pidfd_state_task_rust((void *const *)&pf->task);
    if (target)
        target = proc_get(target);
    vfs_put_file(vf);
    return target;
}

int linux_pidfd_pid(int pidfd)
{
    task_t *target = linux_pidfd_get_task_ref(pidfd);
    if (!target)
        return -EBADF;
    int pid = target->pid;
    proc_put(target);
    return pid;
}

int linux_pidfd_vfile_pid(const vfile_t *vf)
{
    if (!vf || vf->ops != &g_pidfd_ops || !vf->priv)
        return -1;
    pidfd_file_t *pf = (pidfd_file_t *)vf->priv;
    if (pf->pid > 0)
        return pf->pid;
    task_t *target =
        a20_pidfd_state_task_rust((void *const *)&pf->task);
    return target ? target->pid : -1;
}

static int linux_pidfd_create_task(task_t *target, int flags)
{
    /* This function consumes the caller's task reference on every path. */
    if (!target)
        return -EINVAL;
    if (flags & ~O_CLOEXEC) {
        proc_put(target);
        return -EINVAL;
    }

    pidfd_file_t *pf = (pidfd_file_t *)kmalloc(sizeof(*pf));
    if (!pf) {
        proc_put(target);
        return -ENOMEM;
    }
    pf->pid = target->pid;
    a20_pidfd_state_init_rust((void **)&pf->task, target);

    vfile_t *vf = vfile_alloc();
    if (!vf) {
        pidfd_state_release(pf);
        kfree(pf);
        return -ENOMEM;
    }
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_pidfd_ops;
    vf->priv = pf;
    int tag_rc = env_kind_tag_vfile(vf, A20_OBJ_TASK);
    if (tag_rc < 0) {
        vfile_free(vf);
        pidfd_state_release(pf);
        kfree(pf);
        return tag_rc;
    }
    return anonfd_install_vfile(vf, flags);
}

/* Compatibility entry for primary-owned clone3 and SO_PEERPIDFD callers.
 * New Linux pidfd operations should use the stable task-ref accessor. */
int linux_pidfd_create(int pid, int flags)
{
    task_t *target = proc_find_get_user(pid);
    if (!target)
        return -ESRCH;
    return linux_pidfd_create_task(target, flags);
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

    return linux_pidfd_create_task(target, (int)flags);
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
    pidfd_file_t *pf = (pidfd_file_t *)vf->priv;
    task_t *target = pf->task;
    target = a20_pidfd_state_task_rust((void *const *)&pf->task);
    if (target)
        target = proc_get(target);
    vfs_put_file(vf);
    if (!target)
        return -ESRCH;

    if (targetfd < 0) {
        proc_put(target);
        return -EINVAL;
    }

    task_t *self = proc_current();
    if (pidfd_target_exited(target)) {
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
    /* Keep a separate pin so a mediation denial can close only the exact
     * installed vfile even if another thread reuses the returned fd. */
    vfile_get(target_file);
    /* Install into THIS task's table; the install consumes one reference. */
    int r = fdtable_install_vfile(self, target_file, (int)flags);
    proc_put(target);
    if (r < 0) {
        vfs_put_file(target_file);
        vfs_put_file(target_file);
        return r;
    }
    /* A7 acquire side: the stolen descriptor is a fresh authority entering
     * this task, so the receiver's envelope decides whether it may be used
     * (docs/research/05 §2.5.1).  Mediation keys on the NEW fd number, which
     * is a current-task gfd exactly like on the SCM_RIGHTS receive path -- the
     * foreign fd number it came from never needs to be meaningful.  A denied
     * fd is closed rather than left installed, so a denial cannot be
     * side-stepped by using the number afterwards. */
    if (env_active(self)) {
        const uint64_t base_rights = A20_RIGHT_READ | A20_RIGHT_WRITE |
                                     A20_RIGHT_STAT | A20_RIGHT_SEEK;
        int mr = 0;
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
        int injected = a20_envelope_race_test_before_mediation(
            A20_ENVELOPE_RACE_OP_PIDFD, r, target_file);
        if (injected < 0)
            mr = injected;
#endif
        if (!mr)
            mr = env_mediate_acquire_vfile(r, target_file, base_rights);
        if (mr) {
            (void)fdtable_close_current_if_file(r, target_file);
#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
            a20_envelope_race_test_note_pin_release(
                A20_ENVELOPE_RACE_OP_PIDFD, target_file->identity);
#endif
            vfs_put_file(target_file);
            return mr;
        }
    }
    vfs_put_file(target_file);
    return r;
}

int64_t sys_pidfd_send_signal(int pidfd, int sig, void *uinfo, unsigned flags)
{
    if (flags)
        return -EINVAL;
    if (sig < 0 || sig >= NSIG)
        return -EINVAL;

    vfile_t *vf = fdtable_get_current_file_ref(pidfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_pidfd_ops || !vf->priv) {
        vfs_put_file(vf);
        return -EBADF;
    }

    pidfd_file_t *pf = (pidfd_file_t *)vf->priv;
    task_t *target = pf->task;
    target = a20_pidfd_state_task_rust((void *const *)&pf->task);
    if (target)
        target = proc_get(target);
    vfs_put_file(vf);
    if (!target)
        return -ESRCH;

    task_t *self = proc_current();
    if (pidfd_target_exited(target)) {
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
        /* Keep the pidfd task identity pinned through process-directed
         * siginfo delivery; do not resolve the PID a second time. */
        int result = signal_send_task_info(target, sig, info, sizeof(info));
        proc_put(target);
        return result;
    }
    int result = signal_send_task(target, sig);
    proc_put(target);
    return result;
}
