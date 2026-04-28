#include "syscall_internal.h"

static int signal_send_permission(task_t *self, task_t *target) {
    if (!self || self->euid == 0) return 0;
    if (self->uid == target->uid || self->uid == target->suid ||
        self->euid == target->uid || self->euid == target->suid)
        return 0;
    return -EPERM;
}

int64_t sys_kill(int pid, int sig) {
    if (pid > 0) {
        task_t *self = proc_current();
        task_t *target = proc_find(pid);
        if (!target) return -ESRCH;
        int perm = signal_send_permission(self, target);
        if (perm < 0) return perm;
        if (sig == 0) return 0;
    }
    return proc_kill(pid, sig);
}

int64_t sys_tgkill(int tgid, int tid, int sig) {
    (void)tgid;
    task_t *self = proc_current();
    task_t *target = proc_find(tid);
    if (!target) return -ESRCH;
    if (target->state == PROC_ZOMBIE) return -ESRCH;
    int perm = signal_send_permission(self, target);
    if (perm < 0) return perm;
    if (sig == 0) return 0;
    return proc_kill(tid, sig);
}

int64_t sys_tkill(int tid, int sig) {
    task_t *self = proc_current();
    task_t *target = proc_find(tid);
    if (!target) return -ESRCH;
    if (target->state == PROC_ZOMBIE) return -ESRCH;
    int perm = signal_send_permission(self, target);
    if (perm < 0) return perm;
    if (sig == 0) return 0;
    return proc_kill(tid, sig);
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

int64_t sys_sigaction(int signum, void *act, void *oldact) {
    return sys_sigaction_impl(signum, (const sigaction_t *)act, (sigaction_t *)oldact);
}

int64_t sys_sigprocmask(int how, void *set, void *oldset) {
    return sys_sigprocmask_impl(how, (const uint64_t *)set, (uint64_t *)oldset);
}

int64_t sys_sigreturn(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (t && t->signals) {
        signal_state_t *ss = (signal_state_t *)t->signals;
        if (t->sig_handling) {
            ss->blocked = t->sig_old_blocked;
            if (ctx)
                *ctx = t->sig_saved_ctx;
            t->sig_handling = 0;
        }
    }
    return 0;
}

int64_t sys_sigsuspend(void *mask) {
    task_t *t = proc_current();
    if (!t || !t->signals || !mask) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t old_blocked = ss->blocked;
    uint64_t new_mask;
    if (copy_from_user(&new_mask, mask, sizeof(new_mask)) < 0) return -EFAULT;

    /* Can't block SIGKILL or SIGSTOP */
    new_mask = signal_mask_from_user(new_mask);
    new_mask &= ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
    if (syscall_sig_diag_count < 128) {
        syscall_sig_diag_count++;
        kdebug("[SIGSYS] sigsuspend pid=%d old=0x%lx new=0x%lx pending=0x%lx\n",
              t->pid, (unsigned long)old_blocked, (unsigned long)new_mask,
              (unsigned long)ss->pending);
    }
    t->sigsuspend_old_blocked = old_blocked;
    t->sigsuspend_active = 1;
    ss->blocked = new_mask;

    uint64_t deliverable = ss->pending & ~ss->blocked;
    if (!deliverable) {
        t->state = PROC_BLOCKED;
        sched();
    }
    return -EINTR;
}

int64_t sys_sigtimedwait(const uint64_t *set, void *info, const void *timeout) {
    task_t *t = proc_current();
    if (!t || !t->signals || !set) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t mask;
    if (copy_from_user(&mask, set, sizeof(mask)) < 0) return -EFAULT;
    mask = signal_mask_from_user(mask);
    uint64_t deliverable = ss->pending & ~ss->blocked & mask;

    if (!deliverable) {
        if (timeout) {
            uint64_t to[2];
            if (copy_from_user(to, timeout, sizeof(to)) < 0) return -EFAULT;
            uint64_t sec = to[0];
            uint64_t nsec = to[1];
            if (sec == 0 && nsec == 0)
                return -EAGAIN;
        }
        t->state = PROC_BLOCKED;
        sched();
        deliverable = ss->pending & ~ss->blocked & mask;
        if (!deliverable)
            return -EAGAIN;
    }

    for (int sig = 1; sig < NSIG; sig++) {
        if (deliverable & (1ULL << sig)) {
            ss->pending &= ~(1ULL << sig);
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
