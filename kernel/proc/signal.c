/*
 * A20OS — Signal Handling
 *
 * Provides POSIX-compatible signal delivery infrastructure.
 * Signals are delivered synchronously at the next trap boundary.
 */

#include "proc/signal.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/debug.h"
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
 * remapping with execute permissions added.
 */
static void signal_make_page_exec(uint64_t addr) {
    task_t *t = proc_current();
    if (!t || !t->pgdir) return;
    vaddr_t page = addr & ~(vaddr_t)(PAGE_SIZE - 1);
    paddr_t pa = pt_translate(t->pgdir, page);
    if (!pa) return;
    pt_unmap(t->pgdir, page);
    pt_map(t->pgdir, page, pa,
           mm_pte_flags_make_writable_dirty(arch_signal_tramp_pte_flags()));
    arch_tlb_flush_page(page);
}

__attribute__((weak)) void arch_signal_prepare_frame(arch_sig_rt_frame_t *frame,
                                                     vaddr_t tramp_addr,
                                                     trap_context_t *ctx) {
    (void)frame;
    (void)tramp_addr;
    (void)ctx;
}

__attribute__((weak)) void arch_setup_signal_trampoline(struct mm_struct *mm) {
    (void)mm;
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
        case SIGCONT:
            return 1;
        default:
            return 0;
    }
}

static void build_siginfo_code(arch_siginfo_t *si, int sig, task_t *sender, int code)
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

static void build_siginfo(arch_siginfo_t *si, int sig, task_t *sender)
{
    build_siginfo_code(si, sig, sender, SI_USER);
}

// 初始化信号状态
void signal_init(signal_state_t *ss) {
    memset(ss, 0, sizeof(*ss));
    refcount_set(&ss->refcount, 1);
    spin_init(&ss->lock);
    spin_set_debug(&ss->lock, "signal_state", ss);
}

// 复制信号状态（用于 fork 时继承父进程的信号处理函数）
void signal_copy(const signal_state_t *src, signal_state_t *dst) {
    signal_init(dst);
    if (!src)
        return;
    signal_state_t *mutable_src = (signal_state_t *)src;
    uint64_t flags = spin_lock_irqsave(&mutable_src->lock);
    memcpy(dst->actions, src->actions, sizeof(dst->actions));
    spin_unlock_irqrestore(&mutable_src->lock, flags);
}

static uint64_t signal_deliverable_locked(task_t *t, signal_state_t *ss)
{
    return (ss->pending | t->thread_pending) & ~t->sig_blocked;
}

static int signal_action_deliverable_locked(task_t *t, signal_state_t *ss,
                                            int signum)
{
    if (t->sig_blocked & signal_mask_bit(signum))
        return 0;
    sigaction_t *sa = &ss->actions[signum];
    if (sa->sa_handler == SIG_IGN)
        return 0;
    if (sa->sa_handler == SIG_DFL && signal_default_ignore(signum))
        return 0;
    return 1;
}

static int signal_action_fatal_locked(task_t *t, signal_state_t *ss,
                                      int signum)
{
    return signal_action_deliverable_locked(t, ss, signum) &&
           ss->actions[signum].sa_handler == SIG_DFL &&
           signal_default_terminate(signum);
}

static void signal_clear_pending_locked(task_t *t, signal_state_t *ss,
                                        int signum)
{
    uint64_t bit = signal_mask_bit(signum);
    ss->pending &= ~bit;
    t->thread_pending &= ~bit;
    ss->pending_has_info[signum] = 0;
    memset(ss->pending_info[signum], 0, SIGNAL_INFO_SIZE);
}

static void signal_apply_generation_rules_locked(task_t *t,
                                                 signal_state_t *ss,
                                                 int signum)
{
    if (signum == SIGCONT) {
        signal_clear_pending_locked(t, ss, SIGSTOP);
        signal_clear_pending_locked(t, ss, SIGTSTP);
        signal_clear_pending_locked(t, ss, SIGTTIN);
        signal_clear_pending_locked(t, ss, SIGTTOU);
    } else if (signal_default_stop(signum)) {
        signal_clear_pending_locked(t, ss, SIGCONT);
    }
}

static int signal_queue_task(task_t *t, int signum, const void *info,
                             size_t info_size, int thread_directed)
{
    if (!t || !t->signals)
        return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    int is_user = t->pgdir != NULL;
    int fatal = 0;
    int deliverable = 0;
    int sigwait_match = 0;
    int immediate_kernel_exit = 0;

    uint64_t flags = spin_lock_irqsave(&ss->lock);
    signal_apply_generation_rules_locked(t, ss, signum);
    sigaction_t action = ss->actions[signum];

    /*
     * SIGCONT always leaves a pending marker until the target reaches a
     * signal boundary.  proc_sched_stop_current() checks that marker while
     * holding proc_lock, closing SIGCONT-versus-STOPPED publication races.
     * Other ignored/default-ignored signals can be discarded at generation.
     */
    if (signum != SIGCONT &&
        (action.sa_handler == SIG_IGN ||
         (action.sa_handler == SIG_DFL && signal_default_ignore(signum)))) {
        spin_unlock_irqrestore(&ss->lock, flags);
        return 0;
    }

    fatal = signal_action_fatal_locked(t, ss, signum);
    immediate_kernel_exit = !is_user && fatal;

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
    if (thread_directed)
        t->thread_pending |= signal_mask_bit(signum);
    else
        ss->pending |= signal_mask_bit(signum);

    deliverable = signal_action_deliverable_locked(t, ss, signum);
    sigwait_match = t->sigwait_active &&
                    (t->sigwait_mask & signal_mask_bit(signum));
    spin_unlock_irqrestore(&ss->lock, flags);

    if (immediate_kernel_exit) {
        proc_force_exit(t, -signal_wait_status(signum));
        return 0;
    }

    if (signum == SIGCONT)
        (void)proc_sched_resume_stopped(t, 1);
    else if (fatal)
        (void)proc_sched_resume_stopped(t, 0);

    if (deliverable || sigwait_match) {
        proc_wake_reason_t reason =
            fatal ? PROC_WAKE_FATAL_SIGNAL : PROC_WAKE_SIGNAL;
        (void)proc_interrupt_wait(t, reason);
    }

    if (!is_user && t == proc_current())
        signal_deliver();
    return 0;
}

int signal_send_info(int pid, int signum, const void *info, size_t info_size) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find_get(pid);
    if (!t) return -ESRCH;
    int ret = signal_queue_task(t, signum, info, info_size, 0);
    proc_put(t);
    return ret;
}

int signal_send_user(int pid, int signum) {
    arch_siginfo_t si;
    build_siginfo(&si, signum, proc_current());
    return signal_send_info(pid, signum, &si, sizeof(si));
}

int signal_send_thread(int tid, int signum) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find_get(tid);
    if (!t) return -ESRCH;
    int ret = signal_queue_task(t, signum, NULL, 0, 1);
    proc_put(t);
    return ret;
}

int signal_send_thread_user(int tid, int signum) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find_get(tid);
    if (!t) return -ESRCH;
    arch_siginfo_t si;
    build_siginfo_code(&si, signum, proc_current(), SI_TKILL);
    int ret = signal_queue_task(t, signum, &si, sizeof(si), 1);
    proc_put(t);
    return ret;
}

int signal_task_get_pending_info(void *task, int signum, void *out,
                                 size_t size)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals || !out || signum < 1 || signum >= NSIG)
        return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    int ret = -ENOENT;
    if (ss->pending_has_info[signum]) {
        size_t n = size < SIGNAL_INFO_SIZE ? size : SIGNAL_INFO_SIZE;
        memcpy(out, ss->pending_info[signum], n);
        ret = 0;
    }
    spin_unlock_irqrestore(&ss->lock, flags);
    return ret;
}

// 向指定进程发送信号
int signal_send(int pid, int signum) {
    return signal_send_info(pid, signum, NULL, 0);
}

int signal_send_task(void *task, int signum)
{
    if (signum <= 0 || signum >= NSIG)
        return -EINVAL;
    return signal_queue_task((task_t *)task, signum, NULL, 0, 0);
}

int signal_task_has_unblocked(void *task) {
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    if (__atomic_load_n(&t->exit_pending, __ATOMIC_ACQUIRE))
        return 1;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    uint64_t deliverable = signal_deliverable_locked(t, ss);
    int result = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & signal_mask_bit(sig)))
            continue;
        sigaction_t *sa = &ss->actions[sig];
        if (sa->sa_handler == SIG_IGN)
            continue;
        if (sa->sa_handler == SIG_DFL && signal_default_ignore(sig))
            continue;
        result = 1;
        break;
    }
    spin_unlock_irqrestore(&ss->lock, flags);
    return result;
}

int signal_task_has_fatal(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    uint64_t deliverable = signal_deliverable_locked(t, ss);
    int fatal = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if ((deliverable & signal_mask_bit(sig)) &&
            signal_action_fatal_locked(t, ss, sig)) {
            fatal = 1;
            break;
        }
    }
    spin_unlock_irqrestore(&ss->lock, flags);
    return fatal;
}

int signal_task_should_restart(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    uint64_t deliverable = signal_deliverable_locked(t, ss);
    int restart = deliverable != 0;
    for (int sig = 1; sig < NSIG && restart; sig++) {
        if (!(deliverable & signal_mask_bit(sig)))
            continue;
        sigaction_t *sa = &ss->actions[sig];
        if (sa->sa_handler == SIG_IGN ||
            (sa->sa_handler == SIG_DFL && signal_default_ignore(sig)))
            continue;
        if (sa->sa_handler != SIG_DFL && !(sa->sa_flags & SA_RESTART))
            restart = 0;
    }
    spin_unlock_irqrestore(&ss->lock, flags);
    return restart;
}

int signal_task_user_handler_available(void *task, int signum)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals || !t->pgdir ||
        signum <= 0 || signum >= NSIG)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    sigaction_t action = ss->actions[signum];
    int available = action.sa_handler != SIG_DFL &&
                    action.sa_handler != SIG_IGN &&
                    !(t->sig_blocked & signal_mask_bit(signum));
    spin_unlock_irqrestore(&ss->lock, flags);
    return available;
}

int signal_task_sigchld_auto_reap(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    sigaction_t action = ss->actions[SIGCHLD];
    spin_unlock_irqrestore(&ss->lock, flags);
    return action.sa_handler == SIG_IGN ||
           (action.sa_flags & SA_NOCLDWAIT);
}

int signal_task_sigchld_no_cldstop(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    int no_cldstop = (ss->actions[SIGCHLD].sa_flags & SA_NOCLDSTOP) != 0;
    spin_unlock_irqrestore(&ss->lock, flags);
    return no_cldstop;
}

int signal_task_continue_pending(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    int pending = ((ss->pending | t->thread_pending) &
                   signal_mask_bit(SIGCONT)) != 0;
    spin_unlock_irqrestore(&ss->lock, flags);
    return pending;
}

int signal_task_set_temporary_mask(void *task, uint64_t new_mask,
                                   uint64_t *old_mask)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals || !old_mask)
        return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    *old_mask = t->sig_blocked;
    t->sig_blocked = new_mask &
        ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
    spin_unlock_irqrestore(&ss->lock, flags);
    return 0;
}

void signal_task_restore_mask(void *task, uint64_t old_mask)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    t->sig_blocked = old_mask;
    spin_unlock_irqrestore(&ss->lock, flags);
}

void signal_task_defer_mask_restore(void *task, uint64_t old_mask)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    t->sigsuspend_old_blocked = old_mask;
    t->sigsuspend_active = 1;
    spin_unlock_irqrestore(&ss->lock, flags);
}

void signal_task_restore_sigsuspend(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    if (t->sigsuspend_active && !t->sig_handling) {
        t->sig_blocked = t->sigsuspend_old_blocked;
        t->sigsuspend_active = 0;
    }
    spin_unlock_irqrestore(&ss->lock, flags);
}

void signal_exec_reset(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    for (int sig = 1; sig < NSIG; sig++) {
        if (ss->actions[sig].sa_handler != SIG_IGN &&
            ss->actions[sig].sa_handler != SIG_DFL)
            ss->actions[sig].sa_handler = SIG_DFL;
        ss->actions[sig].sa_flags = 0;
        ss->actions[sig].sa_mask = 0;
    }
    ss->pending = 0;
    memset(ss->pending_has_info, 0, sizeof(ss->pending_has_info));
    memset(ss->pending_info, 0, sizeof(ss->pending_info));
    t->sig_handling = 0;
    t->thread_pending = 0;
    t->sigsuspend_active = 0;
    t->sigwait_active = 0;
    t->sigwait_mask = 0;
    spin_unlock_irqrestore(&ss->lock, flags);
}

uint64_t signal_task_pending_blocked(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    uint64_t pending =
        (ss->pending | t->thread_pending) & t->sig_blocked;
    spin_unlock_irqrestore(&ss->lock, flags);
    return pending;
}

// 传递信号（内核线程使用）
void signal_deliver(void) {
    task_t *t = proc_current();
    if (!t || !t->signals) return;

    signal_state_t *ss = (signal_state_t *)t->signals;
    int is_user = t->pgdir != NULL;
    if (is_user)
        return;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&ss->lock);
        uint64_t deliverable = signal_deliverable_locked(t, ss);
        int sig = 0;
        for (int candidate = 1; candidate < NSIG; candidate++) {
            if (deliverable & signal_mask_bit(candidate)) {
                sig = candidate;
                break;
            }
        }
        if (!sig) {
            spin_unlock_irqrestore(&ss->lock, flags);
            return;
        }

        sigaction_t action = ss->actions[sig];
        if (action.sa_handler == SIG_IGN ||
            (action.sa_handler == SIG_DFL && signal_default_ignore(sig))) {
            signal_clear_pending_locked(t, ss, sig);
            spin_unlock_irqrestore(&ss->lock, flags);
            continue;
        }

        signal_clear_pending_locked(t, ss, sig);
        spin_unlock_irqrestore(&ss->lock, flags);

        if (action.sa_handler == SIG_DFL) {
            if (signal_default_stop(sig)) {
                proc_sched_stop_current(sig);
                continue;
            }
            proc_exit_group(-signal_wait_status(sig));
        }

        void (*handler)(int) =
            (void (*)(int))(uintptr_t)action.sa_handler;
        handler(sig);
    }
}

static void build_ucontext(arch_ucontext_t *uc, trap_context_t *ctx,
                           uint64_t old_blocked, arch_sigaltstack_t *altstack)
{
    memset(uc, 0, sizeof(*uc));
    arch_ucontext_sigmask_set(uc, old_blocked);
    uc->uc_stack.ss_sp = altstack->ss_sp;
    uc->uc_stack.ss_flags = altstack->ss_flags;
    uc->uc_stack.ss_size = altstack->ss_size;
    arch_signal_build_mcontext(&uc->uc_mcontext, ctx);
}

void signal_deliver_user(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (!t || !t->signals || !t->pgdir) return;

    signal_state_t *ss = (signal_state_t *)t->signals;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&ss->lock);
        uint64_t deliverable = signal_deliverable_locked(t, ss);
        int sig = 0;
        for (int candidate = 1; candidate < NSIG; candidate++) {
            if (deliverable & signal_mask_bit(candidate)) {
                sig = candidate;
                break;
            }
        }
        if (!sig) {
            spin_unlock_irqrestore(&ss->lock, flags);
            return;
        }

        sigaction_t action = ss->actions[sig];
        if (action.sa_handler == SIG_IGN ||
            (action.sa_handler == SIG_DFL && signal_default_ignore(sig))) {
            signal_clear_pending_locked(t, ss, sig);
            if (t->sigsuspend_active) {
                t->sig_blocked = t->sigsuspend_old_blocked;
                t->sigsuspend_active = 0;
            }
            spin_unlock_irqrestore(&ss->lock, flags);
            continue;
        }

        /*
         * PTRACE_DELIVERY_BOUNDARY: a traced task intercepts every
         * deliverable signal (except SIGKILL) as a ptrace signal-stop
         * instead of delivering it.  The tracer either suppresses it
         * (resume with sig 0) or re-queues it (resume with sig N); a
         * resume-with-signal consumes the one-shot ptrace_deliver_sig
         * marker here so delivery proceeds without a second stop.
         */
        if (proc_debug_is_traced(t) && sig != SIGKILL) {
            if (t->ptrace_deliver_sig == sig) {
                t->ptrace_deliver_sig = 0;
            } else {
                signal_clear_pending_locked(t, ss, sig);
                spin_unlock_irqrestore(&ss->lock, flags);
                (void)proc_debug_signal_stop(sig);
                continue;
            }
        }

        if (action.sa_handler == SIG_DFL) {
            signal_clear_pending_locked(t, ss, sig);
            if (signal_default_stop(sig)) {
                t->sig_blocked = t->sigsuspend_active ?
                              t->sigsuspend_old_blocked : t->sig_blocked;
                t->sigsuspend_active = 0;
                spin_unlock_irqrestore(&ss->lock, flags);
                proc_sched_stop_current(sig);
                continue;
            }
            spin_unlock_irqrestore(&ss->lock, flags);
            proc_exit_group(-signal_wait_status(sig));
        }

        arch_siginfo_t queued_info;
        int has_queued_info = ss->pending_has_info[sig];
        if (has_queued_info)
            memcpy(&queued_info, ss->pending_info[sig], sizeof(queued_info));

        signal_clear_pending_locked(t, ss, sig);

        if (action.sa_flags & SA_RESETHAND)
            ss->actions[sig].sa_handler = SIG_DFL;

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
        t->sig_blocked |= action.sa_mask;
        if (!(action.sa_flags & SA_NODEFER))
            t->sig_blocked |= signal_mask_bit(sig);

        t->sig_handling = sig;
        spin_unlock_irqrestore(&ss->lock, flags);

        uint64_t sp = TRAP_CTX_SP(ctx);

        if ((action.sa_flags & SA_ONSTACK) &&
            t->sigaltstack.ss_flags == 0 &&
            t->sigaltstack.ss_sp != NULL &&
            t->sigaltstack.ss_size >= MINSIGSTKSZ) {
            sp = (uintptr_t)t->sigaltstack.ss_sp + t->sigaltstack.ss_size;
        }

        arch_sig_rt_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        arch_sigframe_flag_set(&frame, 0x77777777ULL);
        if (has_queued_info)
            *arch_sigframe_info_ptr(&frame) = queued_info;
        else
            build_siginfo_code(arch_sigframe_info_ptr(&frame), sig, NULL, SI_KERNEL);
        build_ucontext(arch_sigframe_ucontext_ptr(&frame), ctx, old_blocked, &t->sigaltstack);
        arch_signal_build_frame_extra(arch_sigframe_extra_ptr(&frame), ctx);

        sp -= arch_sigframe_size();
        sp &= ~15ULL;

        uint32_t tramp[2];
        arch_signal_prepare_trampoline(tramp);
        uint64_t tramp_addr = sp + arch_sigframe_tramp_offset();
        arch_signal_prepare_frame(&frame, tramp_addr, ctx);

        if (copy_to_user((void *)sp, &frame, sizeof(frame)) < 0)
            proc_exit_group(-signal_wait_status(SIGSEGV));

        if (copy_to_user((void *)tramp_addr, tramp, sizeof(tramp)) < 0)
            proc_exit_group(-signal_wait_status(SIGSEGV));

        signal_make_page_exec(tramp_addr);

        TRAP_CTX_SP(ctx) = sp;
        TRAP_CTX_EPC(ctx) = action.sa_handler;
        TRAP_CTX_ARG0(ctx) = sig;

        if (action.sa_flags & SA_SIGINFO) {
            TRAP_CTX_ARG1(ctx) = sp + arch_sigframe_info_offset();
            TRAP_CTX_ARG2(ctx) = sp + arch_sigframe_uc_offset();
        }
        TRAP_CTX_RA(ctx) = tramp_addr;
        return;
    }
}

int64_t sys_rt_sigreturn_impl(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (!t || !t->signals) return -EFAULT;

    uint64_t sp = TRAP_CTX_SP(ctx);
    arch_sig_rt_frame_t frame;
    if (copy_from_user(&frame, (void *)sp, sizeof(frame)) < 0)
        return -EFAULT;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    t->sig_blocked = arch_user_sigset_to_kernel(
        arch_ucontext_sigmask_const_ptr(arch_sigframe_ucontext_ptr(&frame)));
    t->sig_handling = 0;
    spin_unlock_irqrestore(&ss->lock, flags);

    arch_signal_restore_mcontext(ctx, &arch_sigframe_ucontext_ptr(&frame)->uc_mcontext);
    arch_signal_restore_frame_extra(ctx, arch_sigframe_extra_ptr(&frame));
    return 0;
}

// 设置信号处理函数（rt_sigaction 系统调用的实现）
int sys_sigaction_impl(int signum, const void *act, void *oldact, size_t sigsetsize) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    if (signum == SIGKILL || signum == SIGSTOP) return -EINVAL;
    if (sigsetsize != ARCH_SIGSET_SIZE) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    arch_user_sigaction_t oldk;
    memset(&oldk, 0, sizeof(oldk));
    if (oldact) {
        uint64_t flags = spin_lock_irqsave(&ss->lock);
        sigaction_t old_action = ss->actions[signum];
        spin_unlock_irqrestore(&ss->lock, flags);
        arch_sigaction_set_handler(&oldk, old_action.sa_handler);
        arch_sigaction_set_flags(
            &oldk, (uint64_t)(uint32_t)old_action.sa_flags);
        arch_sigaction_set_mask(&oldk, old_action.sa_mask);
        if (copy_to_user(oldact, &oldk, sizeof(oldk)) < 0)
            return -EFAULT;
    }
    if (act) {
        arch_user_sigaction_t ukact;
        if (copy_from_user(&ukact, act, sizeof(ukact)) < 0)
            return -EFAULT;
        uint64_t flags = spin_lock_irqsave(&ss->lock);
        ss->actions[signum].sa_handler = arch_sigaction_get_handler(&ukact);
        ss->actions[signum].sa_mask = arch_sigaction_get_mask(&ukact);
        ss->actions[signum].sa_flags = (int)arch_sigaction_get_flags(&ukact);
        spin_unlock_irqrestore(&ss->lock, flags);
    }
    return 0;
}

// 修改信号掩码（sigprocmask 系统调用的实现）
int sys_sigprocmask_impl(int how, const void *set, void *oldset, size_t sigsetsize) {
    if (sigsetsize != ARCH_SIGSET_SIZE) return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;
    if (oldset) {
        uint64_t flags = spin_lock_irqsave(&ss->lock);
        uint64_t blocked = t->sig_blocked;
        spin_unlock_irqrestore(&ss->lock, flags);
        arch_sigset_t oldmask = arch_user_sigset_from_kernel(blocked);
        if (copy_to_user(oldset, &oldmask, sizeof(oldmask)) < 0)
            return -EFAULT;
    }
    if (!set) return 0;

    arch_sigset_t usermask;
    if (copy_from_user(&usermask, set, sizeof(usermask)) < 0)
        return -EFAULT;
    uint64_t mask = arch_user_sigset_to_kernel(&usermask);
    mask &= ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));

    uint64_t flags = spin_lock_irqsave(&ss->lock);
    switch (how) {
        case SIG_BLOCK:   t->sig_blocked |=  mask; break;
        case SIG_UNBLOCK: t->sig_blocked &= ~mask; break;
        case SIG_SETMASK: t->sig_blocked  =  mask; break;
        default:
            spin_unlock_irqrestore(&ss->lock, flags);
            return -EINVAL;
    }
    spin_unlock_irqrestore(&ss->lock, flags);
    return 0;
}
