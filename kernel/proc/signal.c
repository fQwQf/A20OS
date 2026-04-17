/*
 * A20OS — Signal Handling
 *
 * Provides POSIX-compatible signal delivery infrastructure.
 * Signals are delivered synchronously at the next trap boundary.
 */

#include "signal.h"
#include "proc.h"
#include "string.h"
#include "stdio.h"
#include "klog.h"

void signal_init(signal_state_t *ss) {
    memset(ss, 0, sizeof(*ss));
}

void signal_copy(const signal_state_t *src, signal_state_t *dst) {
    memcpy(dst, src, sizeof(*dst));
    dst->pending = 0;
}

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

        uint64_t sp = ctx->x[2];
        sp -= 16;
        sp &= ~15ULL;
        uint32_t *tramp = (uint32_t *)sp;
        tramp[0] = 0x08b00893;
        tramp[1] = 0x00000073;
        ctx->x[2] = sp;

        ctx->sepc = sa->sa_handler;
        ctx->x[10] = sig;
        ctx->x[1] = sp;
        return;
    }
}

int sys_sigaction_impl(int signum, const sigaction_t *act, sigaction_t *oldact) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    if (signum == SIGKILL || signum == SIGSTOP) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    if (oldact) *oldact = ss->actions[signum];
    if (act)    ss->actions[signum] = *act;
    return 0;
}

int sys_sigprocmask_impl(int how, const uint64_t *set, uint64_t *oldset) {
    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    if (oldset) *oldset = ss->blocked;
    if (!set) return 0;

    uint64_t mask = *set & ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    switch (how) {
        case SIG_BLOCK:   ss->blocked |=  mask; break;
        case SIG_UNBLOCK: ss->blocked &= ~mask; break;
        case SIG_SETMASK: ss->blocked  =  mask; break;
        default: return -EINVAL;
    }
    return 0;
}
