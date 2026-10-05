/*
 * A20OS — Signal Handling
 *
 * Provides POSIX-compatible signal delivery infrastructure.
 * Signals are delivered synchronously at the next trap boundary.
 */

#include "proc/signal.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/coredump.h"
#include "proc/debug.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "ipc/ipc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "sys/usercopy.h"

/*
 * Make the page containing @addr executable so the signal trampoline
 * can run.  This is needed because user-allocated stacks (e.g. via
 * malloc in clone02) lack VM_EXEC, and the sigreturn trampoline is
 * placed on the stack.  We upgrade PTE flags by unmapping and
 * remapping with execute permissions added.
 */
static void signal_make_page_exec(uint64_t addr) {
    task_t *t = proc_current();
    if (!t || !t->mm || !t->mm->pgdir) return;
    vaddr_t page = addr & ~(vaddr_t)(PAGE_SIZE - 1);
    if (user_prepare_write(t, (uint64_t)page) < 0) return;
    paddr_t pa = pt_translate(t->pgdir, page);
    if (!pa) return;

    /*
     * Keep the permissions the VMA already grants and add execute, rather than
     * substituting the trampoline's own flags.  This page is part of a live
     * stack: taking the flags from arch_signal_tramp_pte_flags() alone would
     * drop the write bit on architectures where that constant is R+X (riscv64),
     * and the process would take a store page fault the first time it stored
     * through the stack again -- a crash caused by making a page executable.
     */
    pte_t flags = arch_signal_tramp_pte_flags();
    spin_lock(&t->mm->lock);
    mm_seg_t *vma = mm_seg_find(t->mm, page);
    if (vma)
        flags = vma->pte_flags | (flags & PTE_X);
    spin_unlock(&t->mm->lock);

    pt_unmap(t->mm, page);
    pt_map(t->pgdir, page, pa, flags);
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

/*
 * Default: the handler returns to the trampoline slot inside the signal frame.
 * Architectures that cannot execute the frame in place override this (see the
 * declaration in signal.h).
 *
 * aarch64 is the case that forced the hook.  It runs with SCTLR_EL1.WXN set
 * (arch/aarch64/mm/kwx.c), which makes every EL0-writable descriptor
 * execute-never at EL0.  The signal frame lives on the user stack, so the
 * in-frame trampoline is on a page that is writable by definition: the leaf had
 * to be AP=01 (RW) to remain a usable stack, and the CPU then refused to fetch
 * from it -- ESR EC=0x20 (instruction abort from a lower EL) with FSC=0x0f, a
 * permission fault, while the software PTE plainly carried PTE_X.  The only
 * working difference from a text leaf was that AP, so no choice of flags could
 * make one page both a live stack and an executable trampoline.  aarch64
 * therefore gets its own read-only trampoline page and returns there.
 */
__attribute__((weak)) uint64_t arch_signal_tramp_addr(struct mm_struct *mm,
                                                      uint64_t stack_tramp_addr) {
    (void)mm;
    return stack_tramp_addr;
}

/* Default: the handler is entered at the (16-aligned) frame base.  x86_64
 * overrides this to enter 8 bytes lower, per the SysV ABI. */
__attribute__((weak)) uint64_t arch_signal_handler_sp(uint64_t frame_sp) {
    return frame_sp;
}

/*
 * CORE_DUMP_HOOK: the fatal default-action path below calls this before
 * proc_exit_group() for signals whose default action dumps core.  The strong
 * definition in kernel/proc/coredump.c emits the ELF core file; this weak
 * default is a no-op so MCU/profile builds that do not link coredump.c keep
 * working.  Registered-hook style: signal.c does not know the dump details.
 */
__attribute__((weak)) int coredump_on_fatal_signal(int sig,
                                                   trap_context_t *ctx) {
    (void)sig;
    (void)ctx;
    return 0;
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

/* Linux sets the 0x80 WCOREDUMP bit only when a core file was actually
 * produced, so the caller passes whether coredump_on_fatal_signal() wrote
 * one rather than only whether the signal is core-dump-by-default. */
static int signal_wait_status_dumped(int sig, int dumped) {
    int status = sig & 0x7f;
    if (dumped)
        status |= 0x80;
    return status;
}

static int signal_wait_status(int sig) {
    return signal_wait_status_dumped(sig, signal_core_dump_default(sig));
}

/* Exported for kernel/core/trap.c's unhandled-fault fast path (the other
 * fatal termination route besides signal_deliver_user). */
int signal_dumps_core(int sig) {
    return signal_core_dump_default(sig);
}

int signal_fatal_exit_code(int sig) {
    return -signal_wait_status(sig);
}

/* Same, but truthful about whether a core file was produced: the 0x80
 * WCOREDUMP bit is only set when coredump_on_fatal_signal() wrote one. */
int signal_fatal_exit_code_dumped(int sig, int dumped) {
    return -signal_wait_status_dumped(sig, dumped);
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

/* SI_KERNEL is A20OS's generic code for a synchronous fault; Linux instead
 * distinguishes SEGV_MAPERR (1) from SEGV_ACCERR (2).  The code and the
 * faulting address both matter: HotSpot decides whether a SIGSEGV is an
 * implicit null check (its array/field access relies on the fault, not an
 * explicit compare) from them, and misclassifying turns a throwable NPE into
 * a hard VM crash. */
#define SIGINFO_SEGV_MAPERR 1

static void build_siginfo_fault(arch_siginfo_t *si, int sig, const trap_context_t *ctx)
{
    build_siginfo_code(si, sig, NULL, SIGINFO_SEGV_MAPERR);
    uint64_t addr = (uint64_t)TRAP_CTX_KScratch0(ctx);
    memcpy(&si->_sifields[1], &addr, sizeof(addr));
}

void signal_init(signal_state_t *ss) {
    memset(ss, 0, sizeof(*ss));
    refcount_set(&ss->refcount, 1);
    spin_init(&ss->lock);
    spin_set_debug(&ss->lock, "signal_state", ss);
    wait_queue_init(&ss->readiness_waiters);
}

/* Copy the signal state, so that fork inherits the parent's handlers. */
void signal_copy(const signal_state_t *src, signal_state_t *dst) {
    signal_init(dst);
    if (!src)
        return;
    signal_state_t *mutable_src = (signal_state_t *)src;
    uint64_t flags = spin_lock_irqsave(&mutable_src->lock);
    memcpy(dst->actions, src->actions, sizeof(dst->actions));
    dst->rlim_core = src->rlim_core;
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

#if (defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)) && \
    !defined(CONFIG_MCU)
    /* Native tasks observe signals through the handle-table checkpoint set
     * (sys_a20_signal_check); their handlers live in the libc (mlibc), not
     * in the kernel signal state.  Pend into the checkpoint set FIRST so the
     * kernel's default-ignore discard below (e.g. SIGCHLD) cannot swallow a
     * signal a native sigsuspend/checkpoint must see. */
    if (t->abi_mode == 1 && t->a20_ht) {
        struct a20_ht_internal *ht = (struct a20_ht_internal *)t->a20_ht;
        extern void a20_ht_sig_pend(struct a20_ht_internal *ht, int sig);
        a20_ht_sig_pend(ht, signum);
        /* Signals also surface as EventQ events (08-runtime-status
         * deep-water #2): signo rides data0.  Watch keys are pid-as-pointer
         * under both TASK and THREAD types, so notify both. */
        a20_event_notify((void *)(uintptr_t)t->pid, A20_OBJ_TASK,
                         A20_EVENT_SIGNALED, (uint64_t)signum, 0);
        a20_event_notify((void *)(uintptr_t)t->pid, A20_OBJ_THREAD,
                         A20_EVENT_SIGNALED, (uint64_t)signum, 0);
    }
#endif

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
     * holding the task's park_lock, closing SIGCONT-versus-STOPPED publication races.
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

    wait_queue_wake_all(&ss->readiness_waiters, 0, PROC_WAKE_EVENT);

    if (immediate_kernel_exit) {
        proc_force_exit(t, -signal_wait_status_dumped(signum, 0));
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

int signal_signo_pending_scoped(void *task, int signum, int thread_scoped)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals || signum < 1 || signum >= NSIG)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    uint64_t mask = thread_scoped ? t->thread_pending : ss->pending;
    int pending = (mask & signal_mask_bit(signum)) != 0;
    spin_unlock_irqrestore(&ss->lock, flags);
    return pending;
}

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

uint64_t signal_task_rlim_core(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    uint64_t limit = ss->rlim_core;
    spin_unlock_irqrestore(&ss->lock, flags);
    return limit;
}

void signal_task_set_rlim_core(void *task, uint64_t soft)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    ss->rlim_core = soft;
    spin_unlock_irqrestore(&ss->lock, flags);
}

/* Deliver a signal.  Used by kernel threads. */
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
            /* Kernel-context delivery cannot produce a core file, so the
             * WCOREDUMP bit must stay clear here. */
            proc_exit_group(-signal_wait_status_dumped(sig, 0));
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
    /* Signal producers publish pending bits before waking the target.  The
     * overwhelmingly common no-signal return path does not need the signal
     * state lock or a full NSIG scan.  A signal racing after this snapshot is
     * accompanied by a wake/reschedule request and is handled at the next
     * user-return boundary. */
    uint64_t process_pending =
        __atomic_load_n(&ss->pending, __ATOMIC_ACQUIRE);
    uint64_t thread_pending =
        __atomic_load_n(&t->thread_pending, __ATOMIC_ACQUIRE);
    if (((process_pending | thread_pending) & ~t->sig_blocked) == 0)
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
            /* CORE_DUMP_HOOK: emit the ELF core dump while the task's mm and
             * register context are still live, then terminate the group. */
            int dumped = 0;
            if (signal_core_dump_default(sig))
                dumped = coredump_on_fatal_signal(sig, ctx);
            proc_exit_group(-signal_wait_status_dumped(sig, dumped));
        }

        arch_siginfo_t queued_info;
        int has_queued_info = ss->pending_has_info[sig];
        if (has_queued_info)
            memcpy(&queued_info, ss->pending_info[sig], sizeof(queued_info));

        signal_clear_pending_locked(t, ss, sig);

        if (action.sa_flags & SA_RESETHAND)
            ss->actions[sig].sa_handler = SIG_DFL;

        ARCH_TRAP_FAST_RETURN_DISARM(ctx);
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
            t->sigaltstack.ss_sp != 0 &&
            t->sigaltstack.ss_size >= MINSIGSTKSZ) {
            sp = (uintptr_t)t->sigaltstack.ss_sp + t->sigaltstack.ss_size;
        }

        arch_sig_rt_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        arch_sigframe_flag_set(&frame, 0x77777777ULL);
        if (has_queued_info)
            *arch_sigframe_info_ptr(&frame) = queued_info;
        else if (sig == SIGSEGV || sig == SIGBUS)
            build_siginfo_fault(arch_sigframe_info_ptr(&frame), sig, ctx);
        else
            build_siginfo_code(arch_sigframe_info_ptr(&frame), sig, NULL, SI_KERNEL);
        build_ucontext(arch_sigframe_ucontext_ptr(&frame), ctx, old_blocked, &t->sigaltstack);
        arch_signal_build_frame_extra(arch_sigframe_extra_ptr(&frame), ctx);

        sp -= arch_sigframe_size();
        sp &= ~15ULL;

        uint32_t tramp[2];
        arch_signal_prepare_trampoline(tramp);
        uint64_t tramp_addr = sp + arch_sigframe_tramp_offset();
        /* Where the handler actually returns to.  Normally the in-frame slot
         * above, which is also where the trampoline words below are written;
         * an architecture that cannot execute the frame in place returns to its
         * own dedicated page instead, and the frame copy is then ABI state the
         * user can still read but never fetches from. */
        uint64_t return_addr = arch_signal_tramp_addr(t->mm, tramp_addr);
        arch_signal_prepare_frame(&frame, tramp_addr, ctx);

        if (copy_to_user((void *)(uintptr_t)sp, &frame, sizeof(frame)) < 0)
            proc_exit_group(-signal_wait_status_dumped(SIGSEGV, 0));

        if (copy_to_user((void *)(uintptr_t)tramp_addr, tramp, sizeof(tramp)) < 0)
            proc_exit_group(-signal_wait_status_dumped(SIGSEGV, 0));

        /* Only the in-frame trampoline needs its page upgraded; a dedicated
         * trampoline page was already mapped executable at address-space
         * setup and must stay read-only, or SCTLR_EL1.WXN would make it
         * execute-never again. */
        if (return_addr == tramp_addr)
            signal_make_page_exec(tramp_addr);

        /* x86_64 enters the handler below the 16-aligned frame (see
         * arch_signal_handler_sp); its return address goes at that entry sp
         * so the handler's final `ret` reaches the sigreturn trampoline. */
        uint64_t handler_sp = arch_signal_handler_sp(sp);
        if (handler_sp != sp) {
            uint64_t restorer = (uint64_t)arch_sigframe_flag_get(&frame);
            if (copy_to_user((void *)(uintptr_t)handler_sp, &restorer, sizeof(restorer)) < 0)
                proc_exit_group(-signal_wait_status_dumped(SIGSEGV, 0));
        }

        TRAP_CTX_SP(ctx) = handler_sp;
        TRAP_CTX_EPC(ctx) = action.sa_handler;
        TRAP_CTX_ARG0(ctx) = sig;

        if (action.sa_flags & SA_SIGINFO) {
            TRAP_CTX_ARG1(ctx) = sp + arch_sigframe_info_offset();
            TRAP_CTX_ARG2(ctx) = sp + arch_sigframe_uc_offset();
        }
        TRAP_CTX_RA(ctx) = return_addr;
        return;
    }
}

int64_t sys_rt_sigreturn_impl(trap_context_t *ctx) {
    task_t *t = proc_current();
    if (!t || !t->signals) return -EFAULT;

    uint64_t sp = TRAP_CTX_SP(ctx);
    arch_sig_rt_frame_t frame;
    if (copy_from_user(&frame, (void *)(uintptr_t)sp, sizeof(frame)) < 0)
        return -EFAULT;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    t->sig_blocked = arch_user_sigset_to_kernel(
        arch_ucontext_sigmask_const_ptr(arch_sigframe_ucontext_ptr(&frame)));
    t->sig_handling = 0;
    spin_unlock_irqrestore(&ss->lock, flags);

    arch_signal_restore_mcontext(ctx, &arch_sigframe_ucontext_ptr(&frame)->uc_mcontext);
    arch_signal_restore_frame_extra(ctx, arch_sigframe_extra_ptr(&frame));
    ARCH_TRAP_FAST_RETURN_DISARM(ctx);
    return 0;
}

/* Install a signal handler; the implementation of the rt_sigaction syscall. */
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

/* Change the signal mask; the implementation of the sigprocmask syscall. */
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
