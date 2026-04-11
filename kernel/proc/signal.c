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
    /* Default actions: SIG_DFL for all */
}

void signal_copy(const signal_state_t *src, signal_state_t *dst) {
    memcpy(dst, src, sizeof(*dst));
    dst->pending = 0; /* Don't inherit pending signals */
}

int signal_send(int pid, int signum) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find(pid);
    if (!t) return -ESRCH;
    if (!t->signals) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    ss->pending |= (1ULL << signum);

    /* Wake up blocked process */
    if (t->state == PROC_BLOCKED) {
        t->state = PROC_READY;
    }

    /* Handle fatal signals immediately if it's the current process */
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

    /* Find lowest-numbered pending signal */
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & (1ULL << sig))) continue;
        ss->pending &= ~(1ULL << sig);

        sigaction_t *sa = &ss->actions[sig];

        /* SIG_IGN: skip */
        if (sa->sa_handler == SIG_IGN) continue;

        /* SIG_DFL: default action */
        if (sa->sa_handler == SIG_DFL) {
            /* Default actions for common signals */
            switch (sig) {
                case SIGCHLD:  continue;               /* ignore */
                case SIGPIPE:  proc_exit(128 + sig);   /* terminate */
                case SIGKILL:
                case SIGTERM:
                case SIGINT:
                case SIGQUIT:
                case SIGSEGV:
                case SIGILL:
                case SIGABRT:
                    kdebug("[SIGNAL] pid=%d killed by signal %d\n", t->pid, sig);
                    proc_exit(128 + sig);
                default:
                    kdebug("[SIGNAL] pid=%d default action for sig %d: terminate\n", t->pid, sig);
                    proc_exit(128 + sig);
            }
        }

        /* Custom handler: we would normally set up a signal frame on user stack.
         * For kernel-mode processes, we just call the handler directly.
         * For user-mode processes (future): push signal frame and set sepc. */
        if (sa->sa_handler != SIG_DFL && sa->sa_handler != SIG_IGN) {
            /* Invoke handler — for in-kernel shell this is safe */
            void (*handler)(int) = (void (*)(int))(uintptr_t)sa->sa_handler;
            handler(sig);
        }
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

    /* Can't block SIGKILL or SIGSTOP */
    uint64_t mask = *set & ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));

    switch (how) {
        case SIG_BLOCK:   ss->blocked |=  mask; break;
        case SIG_UNBLOCK: ss->blocked &= ~mask; break;
        case SIG_SETMASK: ss->blocked  =  mask; break;
        default: return -EINVAL;
    }
    return 0;
}
