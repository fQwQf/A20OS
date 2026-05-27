/*
 * A20OS — Signal Handling
 *
 * Provides POSIX-compatible signal delivery infrastructure.
 * Signals are delivered synchronously at the next trap boundary.
 */

#include "proc/signal.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"

/*
 * Make the page containing @addr executable so the signal trampoline
 * can run.  This is needed because user-allocated stacks (e.g. via
 * malloc in clone02) lack VM_EXEC, and the sigreturn trampoline is
 * placed on the stack.  We upgrade PTE flags by unmapping and
 * remapping with PTE_X added.
 */
static void signal_make_page_exec(uint64_t addr) {
    task_t *t = proc_current();
    if (!t || !t->pgdir) return;
    vaddr_t page = addr & ~(vaddr_t)(PAGE_SIZE - 1);
    paddr_t pa = pt_translate(t->pgdir, page);
    if (!pa) return;
    pt_unmap(t->pgdir, page);
    pt_map(t->pgdir, page, pa, arch_signal_tramp_pte_flags() | PTE_W | PTE_D);
    arch_tlb_flush_page(page);
}

static int signal_core_dump_default(int sig) {
    switch (sig) {
        case SIGQUIT:
        case SIGILL:
        case SIGABRT:
        case SIGBUS:
        case SIGFPE:
        case SIGSEGV:
        case 31: /* SIGSYS */
            return 1;
        default:
            return 0;
    }
}

static int signal_wait_status(int sig) {
    int status = sig & 0x7f;
    if (signal_core_dump_default(sig))
        status |= 0x80;
    return status;
}

static int signal_default_terminate(int sig) {
    switch (sig) {
        case SIGCHLD:
        case SIGURG:
        case SIGWINCH:
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
        case SIGCONT:
            return 0;
        default:
            return 1;
    }
}

static int signal_default_stop(int sig) {
    switch (sig) {
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            return 1;
        default:
            return 0;
    }
}

static int signal_default_ignore(int sig) {
    switch (sig) {
        case SIGCHLD:
        case SIGURG:
        case SIGWINCH:
            return 1;
        default:
            return 0;
    }
}

typedef struct user_rt_sigaction {
    uintptr_t handler;
    uint64_t  flags;
    uint64_t  mask;
} user_rt_sigaction_t;

typedef struct user_sigset {
    uint64_t bits[1];
} user_sigset_t;

static void build_siginfo_code(siginfo_t *si, int sig, task_t *sender, int code)
{
    memset(si, 0, sizeof(*si));
    si->si_signo = sig;
    si->si_code = code;
    if (sender) {
        si->_sifields[0] = 0;
        si->_sifields[1] = sender->pid;
        si->_sifields[2] = (int)sender->cred.uid;
    }
}

static void build_siginfo(siginfo_t *si, int sig, task_t *sender)
{
    build_siginfo_code(si, sig, sender, SI_USER);
}

// 初始化信号状态
void signal_init(signal_state_t *ss) {
    memset(ss, 0, sizeof(*ss));
    refcount_set(&ss->refcount, 1);
}

// 复制信号状态（用于 fork 时继承父进程的信号处理函数）
void signal_copy(const signal_state_t *src, signal_state_t *dst) {
    memcpy(dst, src, sizeof(*dst));
    refcount_set(&dst->refcount, 1);
    dst->pending = 0;  // 子进程不继承未处理的信号
    memset(dst->pending_has_info, 0, sizeof(dst->pending_has_info));
    memset(dst->pending_info, 0, sizeof(dst->pending_info));
}

int signal_send_info(int pid, int signum, const void *info, size_t info_size) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find(pid);
    if (!t) return -ESRCH;
    if (!t->signals) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    sigaction_t *sa = &ss->actions[signum];

    if (sa->sa_handler == SIG_IGN)
        return 0;

    /* For kernel threads (no pgdir), immediately apply default action.
     * For user processes, always queue the signal — it will be delivered
     * at the next trap boundary via signal_deliver_user(), which sets up
     * the proper signal frame on the user stack. */
    int is_user = (t->pgdir != NULL);

    if (!is_user &&
        !(t->sig_blocked & signal_mask_bit(signum)) &&
        sa->sa_handler == SIG_DFL &&
        signal_default_terminate(signum)) {
        proc_force_exit(t, -signal_wait_status(signum));
        return 0;
    }

    if (info && info_size) {
        size_t n = info_size > SIGNAL_INFO_SIZE ? SIGNAL_INFO_SIZE : info_size;
        memcpy(ss->pending_info[signum], info, n);
        if (n < SIGNAL_INFO_SIZE)
            memset(ss->pending_info[signum] + n, 0, SIGNAL_INFO_SIZE - n);
        ss->pending_has_info[signum] = 1;
    } else {
        ss->pending_has_info[signum] = 0;
        memset(ss->pending_info[signum], 0, SIGNAL_INFO_SIZE);
        *(int *)ss->pending_info[signum] = signum;
    }
    ss->pending |= signal_mask_bit(signum);
    if (t->state == PROC_BLOCKED || t->state == PROC_STOPPED) {
        proc_make_ready(t);
    }

    if (!is_user && t == proc_current()) {
        signal_deliver();
    }
    return 0;
}

int signal_send_user(int pid, int signum) {
    siginfo_t si;
    build_siginfo(&si, signum, proc_current());
    return signal_send_info(pid, signum, &si, sizeof(si));
}

int signal_send_thread(int tid, int signum) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find(tid);
    if (!t) return -ESRCH;
    if (!t->signals) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    sigaction_t *sa = &ss->actions[signum];

    if (sa->sa_handler == SIG_IGN)
        return 0;

    int is_user = (t->pgdir != NULL);

    if (!is_user &&
        !(t->sig_blocked & signal_mask_bit(signum)) &&
        sa->sa_handler == SIG_DFL &&
        signal_default_terminate(signum)) {
        proc_force_exit(t, -signal_wait_status(signum));
        return 0;
    }

    t->thread_pending |= signal_mask_bit(signum);
    if (t->state == PROC_BLOCKED || t->state == PROC_STOPPED) {
        proc_make_ready(t);
    }
    return 0;
}

int signal_send_thread_user(int tid, int signum) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find(tid);
    if (!t) return -ESRCH;
    if (!t->signals) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    sigaction_t *sa = &ss->actions[signum];

    if (sa->sa_handler == SIG_IGN)
        return 0;

    int is_user = (t->pgdir != NULL);

    if (!is_user &&
        !(t->sig_blocked & signal_mask_bit(signum)) &&
        sa->sa_handler == SIG_DFL &&
        signal_default_terminate(signum)) {
        proc_force_exit(t, -signal_wait_status(signum));
        return 0;
    }

    siginfo_t si;
    build_siginfo_code(&si, signum, proc_current(), SI_TKILL);
    memset(ss->pending_info[signum], 0, SIGNAL_INFO_SIZE);
    memcpy(ss->pending_info[signum], &si, sizeof(si));
    ss->pending_has_info[signum] = 1;

    t->thread_pending |= signal_mask_bit(signum);
    if (t->state == PROC_BLOCKED || t->state == PROC_STOPPED) {
        proc_make_ready(t);
    }
    return 0;
}

// 向指定进程发送信号
int signal_send(int pid, int signum) {
    return signal_send_info(pid, signum, NULL, 0);
}

int signal_task_has_unblocked(void *task) {
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t deliverable = (ss->pending | t->thread_pending) & ~t->sig_blocked;
    if (!deliverable)
        return 0;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & signal_mask_bit(sig)))
            continue;
        sigaction_t *sa = &ss->actions[sig];
        if (sa->sa_handler == SIG_IGN)
            continue;
        if (sa->sa_handler == SIG_DFL && signal_default_ignore(sig))
            continue;
        return 1;
    }
    return 0;
}

// 传递信号（内核线程使用）
void signal_deliver(void) {
    task_t *t = proc_current();
    if (!t || !t->signals) return;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t deliverable = (ss->pending | t->thread_pending) & ~t->sig_blocked;
    if (!deliverable) return;

    int is_user = t->pgdir != NULL;
    if (is_user)
        return;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;

        sigaction_t *sa = &ss->actions[sig];

        if (sa->sa_handler == SIG_IGN) {
            ss->pending &= ~signal_mask_bit(sig);
            t->thread_pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            continue;
        }

        if (sa->sa_handler == SIG_DFL) {
            ss->pending &= ~signal_mask_bit(sig);
            t->thread_pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            if (signal_default_ignore(sig))
                continue;
            if (signal_default_stop(sig)) {
                t->state = PROC_STOPPED;
                t->exit_code = sig;
                sched();
                continue;
            }
            proc_exit_group(-signal_wait_status(sig));
        }

        if (is_user) {
            continue;
        }

        ss->pending &= ~signal_mask_bit(sig);
        t->thread_pending &= ~signal_mask_bit(sig);
        ss->pending_has_info[sig] = 0;
        void (*handler)(int) = (void (*)(int))(uintptr_t)sa->sa_handler;
        handler(sig);
    }
}

static void build_ucontext(ucontext_t *uc, trap_context_t *ctx,
                           uint64_t old_blocked, sigaltstack_t *altstack)
{
    memset(uc, 0, sizeof(*uc));
    uc->uc_sigmask[0] = signal_mask_to_user(old_blocked);
    uc->uc_sigmask[1] = 0;
    uc->uc_stack.ss_sp = altstack->ss_sp;
    uc->uc_stack.ss_flags = altstack->ss_flags;
    uc->uc_stack.ss_size = altstack->ss_size;
    arch_signal_build_mcontext(&uc->uc_mcontext, ctx);
}

void signal_deliver_user(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (!t || !t->signals || !t->pgdir) return;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t deliverable = (ss->pending | t->thread_pending) & ~t->sig_blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;

        sigaction_t *sa = &ss->actions[sig];

        if (sa->sa_handler == SIG_IGN) {
            ss->pending &= ~signal_mask_bit(sig);
            t->thread_pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            if (t->sigsuspend_active) {
                t->sig_blocked = t->sigsuspend_old_blocked;
                t->sigsuspend_active = 0;
            }
            continue;
        }

        if (sa->sa_handler == SIG_DFL) {
            ss->pending &= ~signal_mask_bit(sig);
            t->thread_pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            if (signal_default_ignore(sig)) {
                if (t->sigsuspend_active) {
                    t->sig_blocked = t->sigsuspend_old_blocked;
                    t->sigsuspend_active = 0;
                }
                continue;
            }
            if (signal_default_stop(sig)) {
                t->sig_blocked = t->sigsuspend_active ?
                              t->sigsuspend_old_blocked : t->sig_blocked;
                t->sigsuspend_active = 0;
                t->state = PROC_STOPPED;
                t->exit_code = sig;
                sched();
                t->state = PROC_RUNNING;
                continue;
            }
            proc_exit_group(-signal_wait_status(sig));
        }

        siginfo_t queued_info;
        int has_queued_info = ss->pending_has_info[sig];
        if (has_queued_info)
            memcpy(&queued_info, ss->pending_info[sig], sizeof(queued_info));

        ss->pending &= ~signal_mask_bit(sig);
        t->thread_pending &= ~signal_mask_bit(sig);
        ss->pending_has_info[sig] = 0;

        if (sa->sa_flags & SA_RESETHAND)
            sa->sa_handler = SIG_DFL;

        t->sig_saved_ctx = *ctx;
        uint64_t old_blocked = t->sigsuspend_active ?
                               t->sigsuspend_old_blocked : t->sig_blocked;
        t->sig_old_blocked = old_blocked;
        t->sigsuspend_active = 0;

        /* Block the signal mask BEFORE setting sig_handling so that a
         * nested signal delivery from a timer interrupt between these
         * two operations cannot re-enter the handler path and corrupt
         * sig_saved_ctx.  Once sig_handling is set, the signal must
         * already be blocked to prevent reentrant delivery. */
        t->sig_blocked |= sa->sa_mask;
        if (!(sa->sa_flags & SA_NODEFER))
            t->sig_blocked |= signal_mask_bit(sig);

        t->sig_handling = sig;

        uint64_t sp = TRAP_CTX_SP(ctx);

        if ((sa->sa_flags & SA_ONSTACK) &&
            t->sigaltstack.ss_flags == 0 &&
            t->sigaltstack.ss_sp != NULL &&
            t->sigaltstack.ss_size >= MINSIGSTKSZ) {
            sp = (uintptr_t)t->sigaltstack.ss_sp + t->sigaltstack.ss_size;
        }

        sig_rt_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.flag = 0x77777777ULL;
        if (has_queued_info)
            frame.info = queued_info;
        else
            build_siginfo_code(&frame.info, sig, NULL, SI_KERNEL);
        build_ucontext(&frame.uc, ctx, old_blocked, &t->sigaltstack);
        arch_signal_build_frame_extra(&frame.arch_extra, ctx);

        sp -= sizeof(sig_rt_frame_t);
        sp &= ~15ULL;

        if (copy_to_user((void *)sp, &frame, sizeof(frame)) < 0)
            proc_exit_group(-signal_wait_status(SIGSEGV));

        uint32_t tramp[2];
        arch_signal_prepare_trampoline(tramp);
        uint64_t tramp_addr = sp + offsetof(sig_rt_frame_t, tramp);
        if (copy_to_user((void *)tramp_addr, tramp, sizeof(tramp)) < 0)
            proc_exit_group(-signal_wait_status(SIGSEGV));

        signal_make_page_exec(tramp_addr);

        TRAP_CTX_SP(ctx) = sp;
        TRAP_CTX_EPC(ctx) = sa->sa_handler;
        TRAP_CTX_ARG0(ctx) = sig;

        if (sa->sa_flags & SA_SIGINFO) {
            TRAP_CTX_ARG1(ctx) = sp + offsetof(sig_rt_frame_t, info);
            TRAP_CTX_ARG2(ctx) = sp + offsetof(sig_rt_frame_t, uc);
        }
        TRAP_CTX_RA(ctx) = tramp_addr;
        return;
    }
}

int64_t sys_rt_sigreturn_impl(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (!t || !t->signals) return -EFAULT;

    uint64_t sp = TRAP_CTX_SP(ctx);
    sig_rt_frame_t frame;
    if (copy_from_user(&frame, (void *)sp, sizeof(frame)) < 0)
        return -EFAULT;

    t->sig_blocked = signal_mask_from_user(frame.uc.uc_sigmask[0]);

    arch_signal_restore_mcontext(ctx, &frame.uc.uc_mcontext);
    arch_signal_restore_frame_extra(ctx, &frame.arch_extra);

    t->sig_handling = 0;
    return 0;
}

// 设置信号处理函数（rt_sigaction 系统调用的实现）
int sys_sigaction_impl(int signum, const void *act, void *oldact, size_t sigsetsize) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    if (signum == SIGKILL || signum == SIGSTOP) return -EINVAL;
    if (sigsetsize != sizeof(user_sigset_t)) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    if (oldact) {
        user_rt_sigaction_t oldk;
        memset(&oldk, 0, sizeof(oldk));
        oldk.handler = ss->actions[signum].sa_handler;
        oldk.flags = (uint64_t)(uint32_t)ss->actions[signum].sa_flags;
        oldk.mask = signal_mask_to_user(ss->actions[signum].sa_mask);
        if (copy_to_user(oldact, &oldk, sizeof(oldk)) < 0)
            return -EFAULT;
    }
    if (act) {
        user_rt_sigaction_t ukact;
        if (copy_from_user(&ukact, act, sizeof(ukact)) < 0)
            return -EFAULT;
        ss->actions[signum].sa_handler = ukact.handler;
        ss->actions[signum].sa_mask = signal_mask_from_user(ukact.mask);
        ss->actions[signum].sa_flags = (int)ukact.flags;
    }
    return 0;
}

// 修改信号掩码（sigprocmask 系统调用的实现）
int sys_sigprocmask_impl(int how, const void *set, void *oldset, size_t sigsetsize) {
    if (sigsetsize != sizeof(user_sigset_t)) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    if (oldset) {
        user_sigset_t oldmask = { .bits = { signal_mask_to_user(t->sig_blocked) } };
        if (copy_to_user(oldset, &oldmask, sizeof(oldmask)) < 0)
            return -EFAULT;
    }
    if (!set) return 0;

    user_sigset_t usermask;
    if (copy_from_user(&usermask, set, sizeof(usermask)) < 0)
        return -EFAULT;
    uint64_t mask = signal_mask_from_user(usermask.bits[0]);
    mask &= ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));

    switch (how) {
        case SIG_BLOCK:   t->sig_blocked |=  mask; break;
        case SIG_UNBLOCK: t->sig_blocked &= ~mask; break;
        case SIG_SETMASK: t->sig_blocked  =  mask; break;
        default: return -EINVAL;
    }
    return 0;
}
