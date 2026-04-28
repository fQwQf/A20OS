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

static int sig_diag_count = 0;

static int signal_core_dump_default(int sig) {
    switch (sig) {
        case SIGQUIT:
        case SIGILL:
        case SIGABRT:
        case 7:  /* SIGBUS */
        case 8:  /* SIGFPE */
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

static int signal_fatal_default(int sig) {
    switch (sig) {
        case SIGCHLD:
            return 0;
        default:
            return 1;
    }
}

typedef struct user_rt_sigaction {
    uintptr_t handler;
    uint64_t  flags;
    uint32_t  mask[2];
    uint64_t  restorer_or_unused;
} user_rt_sigaction_t;

typedef struct user_sigset {
    uint64_t bits[1];
} user_sigset_t;

// 初始化信号状态
void signal_init(signal_state_t *ss) {
    memset(ss, 0, sizeof(*ss));
}

// 复制信号状态（用于 fork 时继承父进程的信号处理函数）
void signal_copy(const signal_state_t *src, signal_state_t *dst) {
    memcpy(dst, src, sizeof(*dst));
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
    if (!(ss->blocked & signal_mask_bit(signum)) &&
        ss->actions[signum].sa_handler == SIG_DFL &&
        signal_fatal_default(signum)) {
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
    if (sig_diag_count < 64 && signum == SIGCHLD) {
        sig_diag_count++;
        kdebug("[SIGDBG] send pid=%d sig=%d pending=0x%lx state=%d\n",
              pid, signum, (unsigned long)ss->pending, t->state);
    }

    if (t->state == PROC_BLOCKED) {
        proc_make_ready(t);
    }

    if (t == proc_current()) {
        signal_deliver();
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
    return (ss->pending & ~ss->blocked) != 0;
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
            ss->pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            continue;
        }

        if (sa->sa_handler == SIG_DFL) {
            ss->pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            switch (sig) {
                case SIGCHLD:  continue;
                case SIGKILL:
                case SIGTERM:
                case SIGINT:
                case SIGQUIT:
                case SIGSEGV:
                case SIGILL:
                case SIGABRT:
                case SIGPIPE:
                    proc_exit(-signal_wait_status(sig));
                default:
                    proc_exit(-signal_wait_status(sig));
            }
        }

        if (is_user) {
            continue;
        }

        ss->pending &= ~signal_mask_bit(sig);
        ss->pending_has_info[sig] = 0;
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

        if (sig_diag_count < 64 && (sig == SIGCHLD || sig == SIGINT || sig == SIGTERM)) {
            sig_diag_count++;
            kdebug("[SIGDBG] deliver pid=%d sig=%d handler=0x%lx flags=0x%x pending=0x%lx blocked=0x%lx epc=0x%lx\n",
                  t->pid, sig, (unsigned long)sa->sa_handler, sa->sa_flags,
                  (unsigned long)ss->pending, (unsigned long)ss->blocked,
                  (unsigned long)TRAP_CTX_EPC(ctx));
        }

        if (sa->sa_handler == SIG_IGN) {
            ss->pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            continue;
        }

        if (sa->sa_handler == SIG_DFL) {
            ss->pending &= ~signal_mask_bit(sig);
            ss->pending_has_info[sig] = 0;
            switch (sig) {
                case SIGCHLD: continue;
                default: proc_exit(-signal_wait_status(sig));
            }
        }

        ss->pending &= ~signal_mask_bit(sig);
        ss->pending_has_info[sig] = 0;

        t->sig_saved_ctx = *ctx;
        t->sig_handling = sig;
        t->sig_old_blocked = t->sigsuspend_active ? t->sigsuspend_old_blocked : ss->blocked;
        t->sigsuspend_active = 0;

        ss->blocked |= sa->sa_mask;
        ss->blocked |= signal_mask_bit(sig);

        uint64_t sp = TRAP_CTX_SP(ctx);
        sp -= 16;
        sp &= ~15ULL;
        uint32_t tramp[2];
        arch_signal_prepare_trampoline(tramp);
        /* Write signal trampoline to user stack via page table (not direct ptr) */
        if (copy_to_user((void *)sp, tramp, sizeof(tramp)) < 0)
            proc_exit(-signal_wait_status(SIGSEGV));
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
        user_rt_sigaction_t oldk;
        memset(&oldk, 0, sizeof(oldk));
        oldk.handler = ss->actions[signum].sa_handler;
        oldk.flags = (uint64_t)(unsigned)ss->actions[signum].sa_flags;
        uint64_t old_user_mask = signal_mask_to_user(ss->actions[signum].sa_mask);
        oldk.mask[0] = (uint32_t)(old_user_mask & 0xffffffffU);
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
        if (sig_diag_count < 64 && (signum == SIGCHLD || signum == SIGINT || signum == SIGTERM)) {
            sig_diag_count++;
            kdebug("[SIGDBG] install pid=%d sig=%d handler=0x%lx flags=0x%lx mask=0x%lx unused=0x%lx\n",
                  t->pid, signum, (unsigned long)ukact.handler,
                  (unsigned long)ukact.flags,
                  (unsigned long)(((uint64_t)ukact.mask[1] << 32) | (uint64_t)ukact.mask[0]),
                  (unsigned long)ukact.restorer_or_unused);
        }
    }
    return 0;
}

// 修改信号掩码（sigprocmask 系统调用的实现）
int sys_sigprocmask_impl(int how, const uint64_t *set, uint64_t *oldset) {
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
