/*
 * A20OS — Signal Handling
 *
 * Provides POSIX-compatible signal delivery infrastructure.
 * Signals are delivered synchronously at the next trap boundary.
 */

#include "signal.h"
#include "proc.h"
#include "mm.h"
#include "string.h"
#include "stdio.h"
#include "klog.h"

// 初始化信号状态
void signal_init(signal_state_t *ss) {
    memset(ss, 0, sizeof(*ss));
}

// 复制信号状态（用于 fork 时继承父进程的信号处理函数）
void signal_copy(const signal_state_t *src, signal_state_t *dst) {
    memcpy(dst, src, sizeof(*dst));
    dst->pending = 0;  // 子进程不继承未处理的信号
}

// 向指定进程发送信号
int signal_send(int pid, int signum) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find(pid);
    if (!t) return -ESRCH;
    if (!t->signals) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    ss->pending |= (1ULL << signum);

    if (t->state == PROC_BLOCKED) {
        t->state = PROC_READY;
    }

    if (t == proc_current()) {
        signal_deliver();
    }
    return 0;
}

// 传递信号（内核线程使用）
void signal_deliver(void) {
    task_t *t = proc_current();
    if (!t || !t->signals) return;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t deliverable = ss->pending & ~ss->blocked;
    if (!deliverable) return;

    int is_user = t->pgdir != NULL;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;

        sigaction_t *sa = &ss->actions[sig];

        if (sa->sa_handler == SIG_IGN) {
            ss->pending &= ~(1ULL << sig);
            continue;
        }

        if (sa->sa_handler == SIG_DFL) {
            ss->pending &= ~(1ULL << sig);
            switch (sig) {
                case SIGCHLD:  continue;
                case SIGPIPE:  proc_exit(128 + sig);
                case SIGKILL:
                case SIGTERM:
                case SIGINT:
                case SIGQUIT:
                case SIGSEGV:
                case SIGILL:
                case SIGABRT:
                    proc_exit(128 + sig);
                default:
                    proc_exit(128 + sig);
            }
        }

        if (is_user) {
            continue;
        }

        ss->pending &= ~(1ULL << sig);
        void (*handler)(int) = (void (*)(int))(uintptr_t)sa->sa_handler;
        handler(sig);
    }
}

// 传递信号到用户空间（用户进程使用）
void signal_deliver_user(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (!t || !t->signals || !t->pgdir) return;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t deliverable = ss->pending & ~ss->blocked;
    if (!deliverable) return;

    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;

        sigaction_t *sa = &ss->actions[sig];

        if (sa->sa_handler == SIG_IGN) {
            ss->pending &= ~(1ULL << sig);
            continue;
        }

        if (sa->sa_handler == SIG_DFL) {
            ss->pending &= ~(1ULL << sig);
            switch (sig) {
                case SIGCHLD: continue;
                default: proc_exit(128 + sig);
            }
        }

        ss->pending &= ~(1ULL << sig);

        t->sig_saved_ctx = *ctx;
        t->sig_handling = sig;
        t->sig_old_blocked = ss->blocked;

        ss->blocked |= sa->sa_mask;
        ss->blocked |= (1ULL << sig);

        uint64_t sp = TRAP_CTX_SP(ctx);
        sp -= 16;
        sp &= ~15ULL;
#ifdef CONFIG_RISCV64
        uint32_t tramp[2] = { 0x08b00893, 0x00000073 };
#elif defined(CONFIG_LOONGARCH64)
        uint32_t tramp[2] = { 0x0380c093, 0x00200000 };
#else
        uint32_t tramp[2] = { 0, 0 };
#endif
        /* Write signal trampoline to user stack via page table (not direct ptr) */
        copy_to_user((void *)sp, tramp, sizeof(tramp));
        TRAP_CTX_SP(ctx) = sp;

        TRAP_CTX_EPC(ctx) = sa->sa_handler;
        TRAP_CTX_ARG0(ctx) = sig;
        TRAP_CTX_RA(ctx) = sp;
        return;
    }
}

// 设置信号处理函数（sigaction 系统调用的实现）
int sys_sigaction_impl(int signum, const sigaction_t *act, sigaction_t *oldact) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    if (signum == SIGKILL || signum == SIGSTOP) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    if (oldact) {
        /* Write to user pointer via page table walk, not direct dereference */
        if (copy_to_user(oldact, &ss->actions[signum], sizeof(sigaction_t)) < 0)
            return -EFAULT;
    }
    if (act) {
        sigaction_t kact;
        if (copy_from_user(&kact, act, sizeof(sigaction_t)) < 0)
            return -EFAULT;
        ss->actions[signum] = kact;
    }
    return 0;
}

// 修改信号掩码（sigprocmask 系统调用的实现）
int sys_sigprocmask_impl(int how, const uint64_t *set, uint64_t *oldset) {
    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    if (oldset) {
        /* Write old mask to user pointer via page table walk */
        if (copy_to_user(oldset, &ss->blocked, sizeof(uint64_t)) < 0)
            return -EFAULT;
    }
    if (!set) return 0;

    uint64_t mask;
    if (copy_from_user(&mask, set, sizeof(uint64_t)) < 0)
        return -EFAULT;
    mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    switch (how) {
        case SIG_BLOCK:   ss->blocked |=  mask; break;
        case SIG_UNBLOCK: ss->blocked &= ~mask; break;
        case SIG_SETMASK: ss->blocked  =  mask; break;
        default: return -EINVAL;
    }
    return 0;
}
