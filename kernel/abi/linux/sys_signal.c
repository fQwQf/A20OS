#include "syscall_impl.h"
#include "proc/proc_internal.h"

static int signal_send_permission(task_t *self, task_t *target) {
    if (proc_has_cap(self, CAP_KILL)) return 0;
    if (self->cred.uid == target->cred.uid || self->cred.uid == target->cred.suid ||
        self->cred.euid == target->cred.uid || self->cred.euid == target->cred.suid)
        return 0;
    return -EPERM;
}

int64_t sys_kill(int pid, int sig) {
    if (sig < 0 || sig >= NSIG) return -EINVAL;
    if (sig == 0) {
        if (pid > 0)
            return proc_find(pid) ? 0 : -ESRCH;
        return 0;
    }
    if (pid > 0) {
        task_t *self = proc_current();
        task_t *target = proc_find(pid);
        if (!target) return -ESRCH;
        int perm = signal_send_permission(self, target);
        if (perm < 0) return perm;
        return proc_kill(pid, sig);
    }
    if (pid == 0) {
        task_t *self = proc_current();
        if (!self) return -ESRCH;
        int r = proc_kill_pgid(self->pgid, sig, 0);
        return r < 0 ? r : 0;
    }
    if (pid < -1) {
        int r = proc_kill_pgid(-pid, sig, 0);
        return r < 0 ? r : 0;
    }

    int count = 0;
    int max_pid = proc_pid_max();
    for (int p = 1; p <= max_pid; p++) {
        task_t *target = proc_find(p);
        if (!target || target->pid <= 1 || !target->pgdir)
            continue;
        int perm = signal_send_permission(proc_current(), target);
        if (perm < 0)
            continue;
        if (proc_kill(target->pid, sig) == 0)
            count++;
    }
    return count ? 0 : -ESRCH;
}

int64_t sys_tgkill(int tgid, int tid, int sig) {
    task_t *self = proc_current();
    task_t *target = proc_find(tid);
    if (!target) return -ESRCH;
    if (target->state == PROC_ZOMBIE) return -ESRCH;
    int target_tgid = target->tgid > 0 ? target->tgid : target->pid;
    if (target_tgid != tgid) return -ESRCH;
    int perm = signal_send_permission(self, target);
    if (perm < 0) return perm;
    if (sig == 0) return 0;
    return signal_send_thread_user(tid, sig);
}

int64_t sys_tkill(int tid, int sig) {
    task_t *self = proc_current();
    task_t *target = proc_find(tid);
    if (!target) return -ESRCH;
    if (target->state == PROC_ZOMBIE) return -ESRCH;
    int perm = signal_send_permission(self, target);
    if (perm < 0) return perm;
    if (sig == 0) return 0;
    return signal_send_thread_user(tid, sig);
}

int64_t sys_rt_sigqueueinfo(int tgid, int sig, void *uinfo) {
    if (sig <= 0 || sig >= NSIG) return -EINVAL;
    if (!uinfo) return -EFAULT;
    task_t *self = proc_current();
    task_t *target = proc_find(tgid);
    if (!target) return -ESRCH;
    int perm = signal_send_permission(self, target);
    if (perm < 0) return perm;
    uint8_t info[SIGNAL_INFO_SIZE];
    if (copy_from_user(info, uinfo, sizeof(info)) < 0) return -EFAULT;
    *(int *)info = sig;
    return signal_send_info(tgid, sig, info, sizeof(info));
}

int64_t sys_sigaction(int signum, void *act, void *oldact, size_t sigsetsize) {
    return sys_sigaction_impl(signum, act, oldact, sigsetsize);
}

int64_t sys_sigprocmask(int how, void *set, void *oldset, size_t sigsetsize) {
    return sys_sigprocmask_impl(how, set, oldset, sigsetsize);
}

int64_t sys_sigreturn(trap_context_t *ctx) {
    /*
     * Linux syscall 139 is rt_sigreturn on RISC-V, LoongArch and AArch64.
     * Restoring from task->sig_saved_ctx is not sufficient: it is only one
     * slot per task, so nested or adjacent signal delivery can corrupt the
     * context that user space expects rt_sigreturn to restore from its stack
     * frame.  The rt frame is the ABI source of truth.
     */
    return sys_rt_sigreturn_impl(ctx);
}

int64_t sys_sigsuspend(void *mask, size_t sigsetsize) {
    task_t *t = proc_current();
    if (!t || !t->signals || !mask) return -EINVAL;
    if (sigsetsize != sizeof(uint64_t)) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t old_blocked = t->sig_blocked;
    uint64_t new_mask;
    if (copy_from_user(&new_mask, mask, sizeof(new_mask)) < 0) return -EFAULT;

    /* Can't block SIGKILL or SIGSTOP */
    new_mask = signal_mask_from_user(new_mask);
    new_mask &= ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
    t->sigsuspend_old_blocked = old_blocked;
    t->sigsuspend_active = 1;
    t->sig_blocked = new_mask;

    uint64_t deliverable = (ss->pending | t->thread_pending) & ~t->sig_blocked;
    if (!deliverable) {
        proc_block_until(t, 0);
        sched();
    }
    return -EINTR;
}

int64_t sys_sigaltstack(void *ss, void *old_ss) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;

    /* Return current alt-stack info if requested */
    if (old_ss) {
        stack_t cur;
        memset(&cur, 0, sizeof(cur));
        cur.ss_sp    = t->sigaltstack.ss_sp;
        cur.ss_size  = t->sigaltstack.ss_size;
        cur.ss_flags = t->sigaltstack.ss_flags;
        if (t->sigaltstack.ss_flags & SS_DISABLE)
            cur.ss_flags = SS_DISABLE;
        else
            cur.ss_flags = 0;
        if (copy_to_user(old_ss, &cur, sizeof(cur)) < 0)
            return -EFAULT;
    }

    /* Set new alt-stack if requested */
    if (ss) {
        stack_t new_ss;
        if (copy_from_user(&new_ss, ss, sizeof(new_ss)) < 0)
            return -EFAULT;
        if (new_ss.ss_flags & ~SS_DISABLE)
            return -EINVAL;
        if (new_ss.ss_flags & SS_DISABLE) {
            t->sigaltstack.ss_sp    = NULL;
            t->sigaltstack.ss_size  = 0;
            t->sigaltstack.ss_flags = SS_DISABLE;
        } else {
            if (new_ss.ss_size < MINSIGSTKSZ)
                return -ENOMEM;
            t->sigaltstack.ss_sp    = new_ss.ss_sp;
            t->sigaltstack.ss_size  = new_ss.ss_size;
            t->sigaltstack.ss_flags = 0;
        }
    }

    return 0;
}

int64_t sys_sigtimedwait(const uint64_t *set, void *info, const void *timeout, size_t sigsetsize) {
    task_t *t = proc_current();
    if (!t || !t->signals || !set) return -EINVAL;
    if (sigsetsize != sizeof(uint64_t)) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t mask;
    if (copy_from_user(&mask, set, sizeof(mask)) < 0) return -EFAULT;
    mask = signal_mask_from_user(mask);
    uint64_t deliverable = (ss->pending | t->thread_pending) & mask;

    if (!deliverable) {
        uint64_t until = 0;
        if (timeout) {
            uint64_t to[2];
            if (copy_from_user(to, timeout, sizeof(to)) < 0) return -EFAULT;
            uint64_t sec = to[0];
            uint64_t nsec = to[1];
            if (nsec >= 1000000000ULL) return -EINVAL;
            if (sec == 0 && nsec == 0)
                return -EAGAIN;
            uint64_t ticks = sec * TICKS_PER_SEC +
                             nsec * TICKS_PER_SEC / 1000000000ULL;
            until = timer_get_ticks() + (ticks ? ticks : 1);
        }
        proc_block_until(t, until);
        sched();
        if (until)
            proc_set_wake_time(t, 0);
        deliverable = (ss->pending | t->thread_pending) & mask;
        if (!deliverable)
            return -EAGAIN;
    }

    for (int sig = 1; sig < NSIG; sig++) {
        if (deliverable & (1ULL << sig)) {
            ss->pending &= ~signal_mask_bit(sig);
            t->thread_pending &= ~signal_mask_bit(sig);
            if (info) {
                uint8_t infobuf[128];
                memset(infobuf, 0, 128);
                if (ss->pending_has_info[sig])
                    memcpy(infobuf, ss->pending_info[sig], sizeof(infobuf));
                else
                    *(int *)infobuf = sig;
                copy_to_user(info, infobuf, 128);
            }
            ss->pending_has_info[sig] = 0;
            return sig;
        }
    }
    return -EAGAIN;
}

int64_t sys_rt_sigpending(void *set, size_t sigsetsize) {
    if (!set) return -EFAULT;
    if (sigsetsize != sizeof(uint64_t)) return -EINVAL;
    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t pending = (ss->pending | t->thread_pending) & t->sig_blocked;
    uint64_t user_pending = signal_mask_to_user(pending);
    if (copy_to_user(set, &user_pending, sizeof(user_pending)) < 0) return -EFAULT;
    return 0;
}
