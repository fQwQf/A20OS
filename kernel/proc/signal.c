/*
 * A20OS — Signal Handling
 *
 * Provides POSIX-compatible signal delivery infrastructure.
 * Signals are delivered synchronously at the next trap boundary.
 */

#include "proc/signal.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"

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
    uint32_t  mask[2];
    uintptr_t unused;
} user_rt_sigaction_t;

typedef struct user_sigset {
    uint64_t bits[1];
} user_sigset_t;

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
        !(ss->blocked & signal_mask_bit(signum)) &&
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
    if (t->state == PROC_BLOCKED) {
        proc_make_ready(t);
    }

    if (t == proc_current()) {
        signal_deliver();
    }
    return 0;
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
        !(ss->blocked & signal_mask_bit(signum)) &&
        sa->sa_handler == SIG_DFL &&
        signal_default_terminate(signum)) {
        proc_force_exit(t, -signal_wait_status(signum));
        return 0;
    }

    t->thread_pending |= signal_mask_bit(signum);
    if (t->state == PROC_BLOCKED) {
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
    uint64_t deliverable = (ss->pending | t->thread_pending) & ~ss->blocked;
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
    uint64_t deliverable = (ss->pending | t->thread_pending) & ~ss->blocked;
    if (!deliverable) return;

    int is_user = t->pgdir != NULL;

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
                t->state = PROC_BLOCKED;
                sched();
                continue;
            }
            proc_exit(-signal_wait_status(sig));
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

static void build_siginfo(siginfo_t *si, int sig, task_t *sender)
{
    memset(si, 0, sizeof(*si));
    si->si_signo = sig;
    si->si_code = SI_USER;
    if (sender) {
        si->_sifields[0] = 0;
        si->_sifields[1] = sender->pid;
        si->_sifields[2] = (int)sender->cred.uid;
    }
}

static void build_ucontext(ucontext_t *uc, trap_context_t *ctx,
                           uint64_t old_blocked, sigaltstack_t *altstack)
{
    memset(uc, 0, sizeof(*uc));
    uc->uc_sigmask = signal_mask_to_user(old_blocked);
    uc->uc_stack.ss_sp = altstack->ss_sp;
    uc->uc_stack.ss_flags = altstack->ss_flags;
    uc->uc_stack.ss_size = altstack->ss_size;
    sigcontext_t *sc = &uc->uc_mcontext;
#ifdef __riscv
    for (int i = 0; i < 32; i++) sc->sc_regs[i] = ctx->x[i];
    sc->sc_pc = ctx->sepc;
#elif defined(__loongarch__)
    for (int i = 0; i < 32; i++) sc->sc_regs[i] = ctx->regs[i];
    sc->sc_pc = ctx->era;
#endif
}

void signal_deliver_user(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (!t || !t->signals || !t->pgdir) return;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t deliverable = (ss->pending | t->thread_pending) & ~ss->blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;

        sigaction_t *sa = &ss->actions[sig];

        if (sa->sa_handler == SIG_IGN) {
            ss->pending &= ~signal_mask_bit(sig);
            t->thread_pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            if (t->sigsuspend_active) {
                ss->blocked = t->sigsuspend_old_blocked;
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
                    ss->blocked = t->sigsuspend_old_blocked;
                    t->sigsuspend_active = 0;
                }
                continue;
            }
            if (signal_default_stop(sig)) {
                ss->blocked = t->sigsuspend_active ?
                              t->sigsuspend_old_blocked : ss->blocked;
                t->sigsuspend_active = 0;
                t->state = PROC_BLOCKED;
                sched();
                t->state = PROC_RUNNING;
                continue;
            }
            proc_exit(-signal_wait_status(sig));
        }

        ss->pending &= ~signal_mask_bit(sig);
        t->thread_pending &= ~signal_mask_bit(sig);
        ss->pending_has_info[sig] = 0;

        if (sa->sa_flags & SA_RESETHAND)
            sa->sa_handler = SIG_DFL;

        t->sig_saved_ctx = *ctx;
        t->sig_handling = sig;
        uint64_t old_blocked = t->sigsuspend_active ?
                               t->sigsuspend_old_blocked : ss->blocked;
        t->sig_old_blocked = old_blocked;
        t->sigsuspend_active = 0;

        ss->blocked |= sa->sa_mask;
        if (!(sa->sa_flags & SA_NODEFER))
            ss->blocked |= signal_mask_bit(sig);

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
        build_siginfo(&frame.info, sig, t);
        build_ucontext(&frame.uc, ctx, old_blocked, &t->sigaltstack);

        sp -= sizeof(sig_rt_frame_t);
        sp &= ~15ULL;

        if (copy_to_user((void *)sp, &frame, sizeof(frame)) < 0)
            proc_exit(-signal_wait_status(SIGSEGV));

        uint64_t tramp_addr = sp + offsetof(sig_rt_frame_t, uc) +
                              sizeof(ucontext_t) + sizeof(siginfo_t);
        uint32_t tramp[2];
        arch_signal_prepare_trampoline(tramp);
        if (copy_to_user((void *)tramp_addr, tramp, sizeof(tramp)) < 0)
            proc_exit(-signal_wait_status(SIGSEGV));

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

    signal_state_t *ss = (signal_state_t *)t->signals;
    ss->blocked = signal_mask_from_user(frame.uc.uc_sigmask);

    sigcontext_t *sc = &frame.uc.uc_mcontext;
#ifdef __riscv
    for (int i = 0; i < 32; i++) ctx->x[i] = sc->sc_regs[i];
    ctx->sepc = sc->sc_pc;
#elif defined(__loongarch__)
    for (int i = 0; i < 32; i++) ctx->regs[i] = sc->sc_regs[i];
    ctx->era = sc->sc_pc;
#endif

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
        uint64_t old_user_mask = signal_mask_to_user(ss->actions[signum].sa_mask);
        oldk.mask[0] = (uint32_t)(old_user_mask & 0xffffffffULL);
        oldk.mask[1] = (uint32_t)(old_user_mask >> 32);
        if (copy_to_user(oldact, &oldk, sizeof(oldk)) < 0)
            return -EFAULT;
    }
    if (act) {
        user_rt_sigaction_t ukact;
        if (copy_from_user(&ukact, act, sizeof(ukact)) < 0)
            return -EFAULT;
        ss->actions[signum].sa_handler = ukact.handler;
        ss->actions[signum].sa_mask = signal_mask_from_user(
            ((uint64_t)ukact.mask[1] << 32) | (uint64_t)ukact.mask[0]);
        ss->actions[signum].sa_flags = (int)ukact.flags;
    }
    return 0;
}

// 修改信号掩码（sigprocmask 系统调用的实现）
int sys_sigprocmask_impl(int how, const void *set, void *oldset, size_t sigsetsize) {
    if (sigsetsize != sizeof(user_sigset_t)) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    if (oldset) {
        user_sigset_t oldmask = { .bits = { signal_mask_to_user(ss->blocked) } };
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
        case SIG_BLOCK:   ss->blocked |=  mask; break;
        case SIG_UNBLOCK: ss->blocked &= ~mask; break;
        case SIG_SETMASK: ss->blocked  =  mask; break;
        default: return -EINVAL;
    }
    return 0;
}
