/*
 * A20OS — Signal Handling
 *
 * Provides POSIX-compatible signal delivery infrastructure.
 * Signals are delivered synchronously at the next trap boundary.
 */

#include "proc/signal.h"
#include "proc/proc.h"
#include "proc/task_object.h"
#include "proc/proc_internal.h"
#include "proc/coredump.h"
#include "proc/debug.h"
#include "proc/pidns.h"
#include "proc/userns.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "ipc/ipc.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/klog.h"
#include "sys/usercopy.h"
#include "proc/signal_core.h"

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

/* The interrupted stack may reserve ABI-owned bytes below its pointer. */
__attribute__((weak)) uint64_t arch_signal_stack_top(uint64_t sp)
{
    return sp;
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
    return a20_signal_core_dump_default(sig);
}

/* Linux sets the 0x80 WCOREDUMP bit only when a core file was actually
 * produced, so the caller passes whether coredump_on_fatal_signal() wrote
 * one rather than only whether the signal is core-dump-by-default. */
static int signal_wait_status_dumped(int sig, int dumped) {
    return a20_signal_wait_status_dumped(sig, dumped);
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

/* signal_default_terminate's only C caller is signal_action_fatal_locked's
 * C-reference branch below; the Rust-selected path decides fatality in
 * a20_signal_action_fatal, so the C table is not compiled there. */


static void build_siginfo_code(arch_siginfo_t *si, int sig, task_t *sender, int code)
{
    memset(si, 0, sizeof(*si));
    si->si_signo = sig;
    si->si_code = code;
    if (sender) {
        si->_sifields[0] = sender->pid;
        si->_sifields[1] = (int)sender->cred.uid;
    }
}

static void build_siginfo(arch_siginfo_t *si, int sig, task_t *sender)
{
    build_siginfo_code(si, sig, sender, SI_USER);
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

/* Borrowed-pointer view of the queue state for the Rust transaction core.
 * The per-thread pending-info object is optional (lazily allocated). */
a20_signal_queue_state_t signal_queue_state_fill(void *task, signal_state_t *ss)
{
    task_t *t = (task_t *)task;
    signal_pending_info_t *pending = t->thread_signal_info;
    return (a20_signal_queue_state_t){
        .process_pending = &ss->pending,
        .process_has_info = ss->pending_has_info,
        .process_info = &ss->pending_info[0][0],
        .thread_pending = &t->thread_pending,
        .thread_has_info = pending ? pending->has_info : NULL,
        .thread_info = pending ? &pending->info[0][0] : NULL,
    };
}

static uint64_t signal_deliverable_locked(task_t *t, signal_state_t *ss)
{
    return a20_signal_deliverable_mask(ss->pending, t->thread_pending,
                                       t->sig_blocked);
}


static int signal_action_fatal_locked(task_t *t, signal_state_t *ss,
                                      int signum)
{
    return a20_signal_action_fatal(t->sig_blocked,
                                   ss->actions[signum].sa_handler,
                                   signum);
}


static int signal_queue_one(task_t *t, int signum, const void *info,
                             size_t info_size, int thread_directed)
{
    if (!t || !t->signals)
        return -EINVAL;
#if defined(CONFIG_KLOG_SESSION) && CONFIG_KLOG_SESSION
    if (signum == SIGKILL || signum == SIGTERM || signum == SIGABRT) {
        task_t *sender = proc_current();
        klog_record_only(KLOG_INFO, "[SIGNAL] request sender=%u name=%s target=%u name=%s signal=%d thread=%d\n",
                         sender ? (unsigned)sender->pid : 0,
                         sender ? sender->name : "kernel", (unsigned)t->pid,
                         t->name, signum, thread_directed);
    }
#endif

#if (defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)) && \
    !defined(CONFIG_MCU)
    /* Catchable Native signals belong exclusively to its checkpoint queue.
     * Queuing them in Linux state too would apply a second SIG_DFL action
     * before libc can honor its handler, mask or SIG_IGN (notably SIGPIPE).
     * Unmaskable KILL/STOP retain kernel delivery and safe-boundary control. */
    if (t->abi_mode == 1 && t->a20_ht && signum != SIGKILL && signum != SIGSTOP) {
        extern void a20_ht_sig_pend(struct a20_ht_internal *, int);
        extern void a20_thread_sig_pend(task_t *, int);
        if (thread_directed) a20_thread_sig_pend(t, signum);
        else a20_ht_sig_pend((struct a20_ht_internal *)t->a20_ht, signum);
        a20_task_object_notify(t, A20_EVENT_SIGNALED, (uint64_t)signum, 0, 0);
        if (signum == SIGCONT) (void)proc_sched_resume_stopped(t, 1);
        (void)proc_interrupt_wait(t, PROC_WAKE_SIGNAL);
        return 0;
    }
#endif

    signal_state_t *ss = (signal_state_t *)t->signals;
    int is_user = t->pgdir != NULL;
    int fatal = 0;
    int deliverable = 0;
    int sigwait_match = 0;

    signal_pending_info_t *candidate = NULL;
    if (thread_directed && info && info_size &&
        !__atomic_load_n(&t->thread_signal_info, __ATOMIC_RELAXED)) {
        candidate = kmalloc(sizeof(*candidate));
        if (!candidate) return -ENOMEM;
        memset(candidate, 0, sizeof(*candidate));
    }
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    /* Rust owns the whole queue transaction: generation-cancel in both
     * scopes, discard decision, coalescing + siginfo write, pending bit.
     * The C side contributes only the lock and the lazy object install. */
    if (thread_directed && !t->thread_signal_info && candidate) {
        __atomic_store_n(&t->thread_signal_info, candidate, __ATOMIC_RELEASE);
        candidate = NULL;
    }
    if (signum == SIGSTOP)
        t->jobctl_stop_epoch = __atomic_load_n(
            &(t->tg_leader ? t->tg_leader : t)->jobctl_state, __ATOMIC_ACQUIRE) & ~3u;
    a20_signal_queue_state_t st = signal_queue_state_fill(t, ss);
    a20_signal_queue_result_t result;
    a20_signal_queue_locked(&st, signum, (const uint8_t *)info, info_size,
                            ss->actions[signum].sa_handler, t->sig_blocked,
                            thread_directed, t->sigwait_active, t->sigwait_mask,
                            &result);
    fatal = result.fatal;
    deliverable = result.deliverable;
    sigwait_match = result.sigwait_match;
    int discarded = result.discard;
    spin_unlock_irqrestore(&ss->lock, flags);

    kfree(candidate);
    if (discarded)
        return 0;

    wait_queue_wake_all(&ss->readiness_waiters, 0, PROC_WAKE_EVENT);

    int immediate_kernel_exit = !is_user && fatal;
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

/* SIGSTOP is process-wide even when a sibling takes the shared occurrence.
 * Queue a private unmaskable stop for every member. SIGCONT resumes all of
 * them, but publishes only the original single catchable signal occurrence.
 * jobctl_lock serializes generations; tasklist_lock pins a member snapshot;
 * no signal-state lock is held while a scheduler wake takes park_lock. */
static int signal_queue_task(task_t *t, int signum, const void *info,
                             size_t info_size, int thread_directed)
{
    if (!t || !t->signals || !t->pgdir ||
        (signum != SIGSTOP && signum != SIGCONT))
        return signal_queue_one(t, signum, info, info_size, thread_directed);

    task_t *leader = t->tg_leader ? t->tg_leader : t;
    task_t *local[16], **members = local;
    size_t capacity = 16, count;
    uint64_t control_flags;
    for (;;) {
        control_flags = spin_lock_irqsave(&leader->jobctl_lock);
        uint64_t list_flags = spin_lock_irqsave(&tasklist_lock);
        count = 0;
        int overflow = 0;
        for (task_t *member = leader; member; member = member->tg_next) {
            int state = proc_task_state_get(member);
            if (state == PROC_UNUSED || state == PROC_ZOMBIE || !member->signals)
                continue;
            if (count == capacity) { overflow = 1; break; }
            task_t *ref = proc_get(member);
            if (ref) members[count++] = ref;
        }
        if (!overflow) {
            /* Publish the generation while clone publication is excluded.
             * A later sibling inherits this state before it can run. */
            uint32_t state = __atomic_load_n(&leader->jobctl_state, __ATOMIC_RELAXED);
            if (signum == SIGCONT) state = (state + 4u) & ~3u;
            else if (!(state & 1u)) state = (state & ~2u) | 1u;
            __atomic_store_n(&leader->jobctl_state, state, __ATOMIC_RELEASE);
            spin_unlock_irqrestore(&tasklist_lock, list_flags);
            break;
        }
        spin_unlock_irqrestore(&tasklist_lock, list_flags);
        spin_unlock_irqrestore(&leader->jobctl_lock, control_flags);
        for (size_t i = 0; i < count; i++) proc_put(members[i]);
        if (members != local) kfree(members);
        if (capacity > ((size_t)-1) / 2 / sizeof(*members)) return -ENOMEM;
        capacity *= 2;
        members = kcalloc(capacity, sizeof(*members));
        if (!members) return -ENOMEM;
    }
    /* Scheduler resume emits parent notifications which may walk the task
     * list. Keep only referenced members and the generation lock here. */
    int ret = 0;
    for (size_t i = 0; i < count; i++) {
        task_t *member = members[i];
        uint64_t sf;
        if (signum == SIGSTOP) {
            int rc = signal_queue_one(member, signum, info, info_size, 1);
            if (rc < 0 && !ret) ret = rc;
        } else {
            signal_state_t *ms = (signal_state_t *)member->signals;
            sf = spin_lock_irqsave(&ms->lock);
            a20_signal_queue_state_t st = signal_queue_state_fill(member, ms);
            uint64_t cancel = a20_signal_generation_cancel_mask(SIGCONT);
            /* The Rust consume operation clears pending bits and siginfo
             * together. Each signum has at most one occurrence per scope. */
            for (int sig = 1; sig < NSIG; sig++) {
                if (!(cancel & signal_mask_bit(sig))) continue;
                (void)a20_signal_consume_locked(&st, sig, NULL, 0);
                (void)a20_signal_consume_locked(&st, sig, NULL, 0);
            }
            spin_unlock_irqrestore(&ms->lock, sf);
            (void)proc_sched_resume_stopped(member, 1);
        }
    }
    if (signum == SIGCONT)
        ret = signal_queue_one(t, signum, info, info_size, thread_directed);
    spin_unlock_irqrestore(&leader->jobctl_lock, control_flags);
    for (size_t i = 0; i < count; i++) proc_put(members[i]);
    if (members != local) kfree(members);
    return ret;
}

/* A new sibling or exec image joins an already-stopped process. Clone holds
 * tasklist_lock; exec holds jobctl_lock, before its first user instruction. */
void signal_group_stop_inherit(void *task)
{
    task_t *t = task;
    if (!t || !t->signals) return;
    task_t *leader = t->tg_leader ? t->tg_leader : t;
    uint32_t state = __atomic_load_n(&leader->jobctl_state, __ATOMIC_ACQUIRE);
    if (!(state & 1u)) return;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    t->jobctl_stop_epoch = state & ~3u;
    a20_signal_queue_state_t st = signal_queue_state_fill(t, ss);
    a20_signal_queue_result_t result;
    a20_signal_queue_locked(&st, SIGSTOP, NULL, 0, 0, 0, 1, 0, 0, &result);
    spin_unlock_irqrestore(&ss->lock, flags);
}

int signal_task_group_stop_cancelled(void *task)
{
    task_t *t = task;
    if (!t) return 0;
    task_t *leader = t->tg_leader ? t->tg_leader : t;
    return t->jobctl_stop_epoch !=
        (__atomic_load_n(&leader->jobctl_state, __ATOMIC_ACQUIRE) & ~3u);
}

int signal_task_group_stop_active(void *task)
{
    task_t *t = task;
    if (!t) return 0;
    task_t *leader = t->tg_leader ? t->tg_leader : t;
    return (__atomic_load_n(&leader->jobctl_state, __ATOMIC_ACQUIRE) & 1u) != 0;
}

/* A parent must not observe a process stop while another thread still runs.
 * Each member calls this after publishing STOPPED and releasing park_lock.
 * Membership and signal generations are stable under tasklist_lock; inspect
 * one park_lock at a time, and emit one leader wait report per generation. */
void signal_group_stop_completed(void *task, int sig)
{
    task_t *t = task;
    if (!t || !t->signals) return;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    task_t *leader = t->tg_leader ? t->tg_leader : t;
    uint32_t state = __atomic_load_n(&leader->jobctl_state, __ATOMIC_ACQUIRE);
    int report = (state & 3u) == 1u && t->jobctl_stop_epoch == (state & ~3u);
    if (!report) goto out;
    for (task_t *member = leader; member; member = member->tg_next) {
        int state = proc_task_state_get(member);
        if (state != PROC_UNUSED && state != PROC_ZOMBIE && state != PROC_STOPPED)
            goto out;
    }
    if (proc_task_state_get(leader) != PROC_STOPPED) goto out;
    __atomic_store_n(&leader->jobctl_state, state | 2u, __ATOMIC_RELEASE);
    uint64_t lf = spin_lock_irqsave(&leader->park_lock);
    leader->exit_code = sig;
    leader->stop_report_pending = 1;
    leader->continue_report_pending = 0;
    spin_unlock_irqrestore(&leader->park_lock, lf);
    task_t *parent = leader->parent ? proc_get(leader->parent) : NULL;
    spin_unlock_irqrestore(&tasklist_lock, flags);
    if (parent) {
        proc_wake_child_waiters(parent);
        if (!signal_task_sigchld_no_cldstop(parent))
            (void)signal_send(parent->pid, SIGCHLD);
        proc_put(parent);
    }
    return;
out:
    spin_unlock_irqrestore(&tasklist_lock, flags);
}

int signal_send_task_info(void *task, int signum, const void *info,
                          size_t info_size)
{
    if (!task)
        return -ESRCH;
    if (signum <= 0 || signum >= NSIG)
        return -EINVAL;
    return signal_queue_task((task_t *)task, signum, info, info_size, 0);
}

int signal_send_info(int pid, int signum, const void *info, size_t info_size) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find_get(pid);
    if (!t) return -ESRCH;
    int ret = signal_send_task_info(t, signum, info, info_size);
    proc_put(t);
    return ret;
}

/* A completed child group is identified by its leader, including when the
 * last task to exit is a sibling. Keep Linux siginfo independent of the
 * exiting worker's credentials and PID. Both tasks are referenced by caller. */
void signal_notify_child_exit(void *parent, void *child)
{
    task_t *p = parent, *c = child;
    if (!p || !c || c->exit_signal <= 0)
        return;
    arch_siginfo_t info;
    memset(&info, 0, sizeof(info));
    info.si_signo = c->exit_signal;
    int code = c->exit_code;
    info.si_code = code < 0 ? ((-code & 0x80) ? CLD_DUMPED : CLD_KILLED) : CLD_EXITED;
    info._sifields[0] = task_pid_nr_ns(c, pidns_task_own(p));
    info._sifields[1] = userns_from_kuid(userns_task_own(p), c->cred.uid);
    info._sifields[2] = code < 0 ? (-code & 0x7f) : (code & 0xff);
    (void)signal_send_info(p->pid, c->exit_signal, &info, sizeof(info));
}

int signal_send_user(int pid, int signum) {
    arch_siginfo_t si;
    build_siginfo(&si, signum, proc_current());
    return signal_send_info(pid, signum, &si, sizeof(si));
}

int signal_send_thread_info(int tid, int signum, const void *info, size_t size) {
    if (signum <= 0 || signum >= NSIG) return -EINVAL;
    task_t *t = proc_find_get(tid);
    if (!t) return -ESRCH;
    int ret = signal_queue_task(t, signum, info, size, 1);
    proc_put(t);
    return ret;
}

int signal_send_thread_fault(int tid, int signum, uintptr_t addr, int code) {
    arch_siginfo_t info;
    build_siginfo_code(&info, signum, NULL, code);
    memcpy(info._sifields, &addr, sizeof(addr));
    return signal_send_thread_info(tid, signum, &info, sizeof(info));
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
    a20_signal_queue_state_t st = signal_queue_state_fill(t, ss);
    int ret = a20_signal_peek_locked(&st, signum, out, size);
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

int signal_task_blocks_or_ignores(void *task, int sig) {
    task_t *t = task;
    if (!t || !t->signals || sig <= 0 || sig >= NSIG) return 0;
    signal_state_t *ss = t->signals;
    uint64_t f = spin_lock_irqsave(&ss->lock);
    int blocked = (t->sig_blocked & signal_mask_bit(sig)) || ss->actions[sig].sa_handler == SIG_IGN;
    spin_unlock_irqrestore(&ss->lock, f);
    return blocked;
}

int signal_task_has_unblocked(void *task) {
    task_t *t = (task_t *)task;
    if (!t) return 0;
    if (__atomic_load_n(&t->exit_pending, __ATOMIC_ACQUIRE))
        return 1;
#if (defined(CONFIG_ABI_NATIVE) || defined(CONFIG_ABI_BOTH)) && !defined(CONFIG_MCU)
    if (t->abi_mode == 1 && t->a20_ht) {
        /* Native handlers and masks belong to the libc checkpoint protocol.
         * Check without consuming before an interruptible park, closing the
         * enqueue-before-park race for file locks, pipes and futexes too. */
        extern uint64_t a20_thread_sig_check(task_t *, uint32_t, uint64_t);
        if (a20_thread_sig_check(t, 2u /* A20_SIGNAL_PEEK */, 0)) return 1;
        signal_state_t *native_state = (signal_state_t *)t->signals;
        uint64_t unmaskable = signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP);
        return native_state && ((__atomic_load_n(&native_state->pending, __ATOMIC_ACQUIRE) |
            __atomic_load_n(&t->thread_pending, __ATOMIC_ACQUIRE)) & unmaskable);
    }
#endif
    if (!t->signals) return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    uint64_t deliverable = signal_deliverable_locked(t, ss);
    int result = 0;
    for (int sig = 1; sig < NSIG; sig++) {
        if (!(deliverable & signal_mask_bit(sig)))
            continue;
        sigaction_t *sa = &ss->actions[sig];
        if (a20_signal_action_deliverable(t->sig_blocked, sa->sa_handler, sig)) {
            result = 1;
            break;
        }
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
        if (a20_signal_restart_skips(sa->sa_handler, sig))
            continue;
        if (a20_signal_restart_vetoes(sa->sa_handler, sa->sa_flags))
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
    int available = a20_signal_action_user_handler_available(
        action.sa_handler, (t->sig_blocked & signal_mask_bit(signum)) != 0);
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
    return a20_signal_sigchld_auto_reap(action.sa_handler, action.sa_flags);
}

int signal_task_sigchld_no_cldstop(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    int no_cldstop = a20_signal_sigchld_no_cldstop(ss->actions[SIGCHLD].sa_flags);
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
    int pending = a20_signal_continue_pending(ss->pending | t->thread_pending);
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
    t->sig_blocked = a20_signal_temporary_mask(new_mask);
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
    if (a20_signal_sigsuspend_restore_active(t->sigsuspend_active,
                                             t->sig_handling)) {
        t->sig_blocked = t->sigsuspend_old_blocked;
        t->sigsuspend_active = 0;
    }
    spin_unlock_irqrestore(&ss->lock, flags);
}

/* Exec must not reset dispositions still shared with a different process
 * created by CLONE_SIGHAND. Allocate before exec's irreversible commit. */
int signal_exec_unshare(void *task)
{
    task_t *t = task;
    signal_state_t *old = t ? t->signals : NULL;
    if (!old || refcount_read(&old->refcount) == 1)
        return 0;
    signal_state_t *copy = kmalloc(sizeof(*copy));
    if (!copy)
        return -ENOMEM;
    signal_copy(old, copy);
    /* Group control keeps referenced tasks, then accesses their current
     * signal state. Serialize replacement so it cannot retain a freed SS. */
    task_t *leader = t->tg_leader ? t->tg_leader : t;
    uint64_t control_flags = spin_lock_irqsave(&leader->jobctl_lock);
    t->signals = copy;
    if (refcount_dec_and_test(&old->refcount))
        kfree(old);
    spin_unlock_irqrestore(&leader->jobctl_lock, control_flags);
    return 0;
}

void signal_exec_reset(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return;
    task_t *leader = t->tg_leader ? t->tg_leader : t;
    uint64_t control_flags = spin_lock_irqsave(&leader->jobctl_lock);
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    /* One Rust transaction: disposition reset (SIG_IGN survives, flags and
     * mask cleared), all pending state from both scopes, task scalars. */
    a20_signal_queue_state_t st = signal_queue_state_fill(t, ss);
    a20_signal_task_slots_t slots = {
        .sig_handling = &t->sig_handling,
        .sigsuspend_active = &t->sigsuspend_active,
        .sigwait_active = &t->sigwait_active,
        .sigwait_mask = &t->sigwait_mask,
    };
    a20_signal_exec_reset_locked(&st, (uint8_t *)ss->actions, NSIG,
                                 sizeof(ss->actions[0]),
                                 offsetof(sigaction_t, sa_handler),
                                 sizeof(ss->actions[0].sa_handler),
                                 offsetof(sigaction_t, sa_mask),
                                 offsetof(sigaction_t, sa_flags),
                                 &slots);
    /* POSIX: the new image starts with an empty signal mask.  This is a
     * per-task field protected by the same signal-state lock. */
    t->sig_blocked = 0;
    spin_unlock_irqrestore(&ss->lock, flags);
    signal_group_stop_inherit(t);
    spin_unlock_irqrestore(&leader->jobctl_lock, control_flags);
}

uint64_t signal_task_pending_blocked(void *task)
{
    task_t *t = (task_t *)task;
    if (!t || !t->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    uint64_t pending = a20_signal_pending_blocked(ss->pending,
                                                  t->thread_pending,
                                                  t->sig_blocked);
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

    /* Rust owns the whole delivery-selection state transaction; the C side
     * keeps the scheduling/termination side effects only. */
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&ss->lock);
        a20_signal_queue_state_t st = signal_queue_state_fill(t, ss);
        a20_signal_delivery_txn_t txn = {
            .blocked = t->sig_blocked,
            .user = 0,
            .traced = 0,
            .ptrace_deliver_sig = 0,
        };
        a20_signal_delivery_select_locked(&st, (const uint8_t *)ss->actions,
            NSIG, sizeof(ss->actions[0]),
            offsetof(sigaction_t, sa_handler), sizeof(ss->actions[0].sa_handler),
            offsetof(sigaction_t, sa_mask), offsetof(sigaction_t, sa_flags),
            &txn, NULL, 0);
        spin_unlock_irqrestore(&ss->lock, flags);

        if (txn.outcome == A20_DELIVERY_NONE)
            return;
        if (txn.outcome == A20_DELIVERY_DROP_IGNORED)
            continue;
        if (txn.outcome == A20_DELIVERY_STOP) {
            proc_sched_stop_current(txn.sig);
            continue;
        }
        if (txn.outcome == A20_DELIVERY_FATAL) {
            klog_record_only(KLOG_WARN, "[SIGNAL] process terminated: pid=%u name=%s signal=%d\n",
                             (unsigned)t->pid, t->name, txn.sig);
            /* Kernel-context delivery cannot produce a core file, so the
             * WCOREDUMP bit must stay clear here. */
            proc_exit_group(-signal_wait_status_dumped(txn.sig, 0));
        }

        void (*handler)(int) = (void (*)(int))(uintptr_t)txn.handler;
        handler(txn.sig);
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

/* Mechanical handler-frame build shared by both delivery paths: stack /
 * altstack selection, siginfo + ucontext fill, trampoline publication and
 * the usercopy of frame + trampoline.  Every policy input arrives as an
 * argument (Rust transaction outputs on the Rust-selected path). */
static void signal_build_handler_frame(trap_context_t *ctx, task_t *t, int sig,
                                       uint64_t handler, int sa_flags,
                                       uint64_t old_blocked,
                                       int has_queued_info,
                                       const arch_siginfo_t *queued_info)
{
    uint64_t sp = arch_signal_stack_top(TRAP_CTX_SP(ctx));

    if ((sa_flags & SA_ONSTACK) &&
        t->sigaltstack.ss_flags == 0 &&
        t->sigaltstack.ss_sp != 0 &&
        t->sigaltstack.ss_size >= MINSIGSTKSZ) {
        sp = (uintptr_t)t->sigaltstack.ss_sp + t->sigaltstack.ss_size;
    }

    arch_sig_rt_frame_t frame;
    memset(&frame, 0, sizeof(frame));
    arch_sigframe_flag_set(&frame, 0x77777777ULL);
    if (has_queued_info)
        *arch_sigframe_info_ptr(&frame) = *queued_info;
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
    if (return_addr == tramp_addr &&
        mm_make_page_exec(t->mm, tramp_addr) < 0)
        proc_exit_group(-signal_wait_status_dumped(SIGSEGV, 0));

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
    TRAP_CTX_EPC(ctx) = handler;
    TRAP_CTX_ARG0(ctx) = sig;

    if (sa_flags & SA_SIGINFO) {
        TRAP_CTX_ARG1(ctx) = sp + arch_sigframe_info_offset();
        TRAP_CTX_ARG2(ctx) = sp + arch_sigframe_uc_offset();
    }
    TRAP_CTX_RA(ctx) = return_addr;
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
    if (!(process_pending | thread_pending))
        return;
    if (a20_signal_deliverable_mask(process_pending, thread_pending,
                                    t->sig_blocked) == 0)
        return;
    /* Rust owns the whole per-iteration delivery state transaction
     * (selection, ignored-occurrence consumption + sigsuspend restore,
     * ptrace interception, stop/fatal consumption, handler-path mask
     * composition and RESETHAND); the C side keeps the trap-context save,
     * the frame publication and the scheduling/termination effects. */
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&ss->lock);
        a20_signal_queue_state_t st = signal_queue_state_fill(t, ss);
        arch_siginfo_t queued_info;
        a20_signal_delivery_txn_t txn = {
            .blocked = t->sig_blocked,
            .sigsuspend_active = t->sigsuspend_active,
            .sigsuspend_old_blocked = t->sigsuspend_old_blocked,
            .user = 1,
            .traced = proc_debug_is_traced(t),
            .ptrace_deliver_sig = t->ptrace_deliver_sig,
        };
        a20_signal_delivery_select_locked(&st, (const uint8_t *)ss->actions,
            NSIG, sizeof(ss->actions[0]),
            offsetof(sigaction_t, sa_handler), sizeof(ss->actions[0].sa_handler),
            offsetof(sigaction_t, sa_mask), offsetof(sigaction_t, sa_flags),
            &txn, (uint8_t *)&queued_info, sizeof(queued_info));
        if (txn.outcome == A20_DELIVERY_STOP && txn.sig != SIGSTOP)
            t->jobctl_stop_epoch = __atomic_load_n(
                &(t->tg_leader ? t->tg_leader : t)->jobctl_state, __ATOMIC_ACQUIRE) & ~3u;
        t->sig_blocked = txn.blocked;
        t->sigsuspend_active = txn.sigsuspend_active;
        t->ptrace_deliver_sig = txn.ptrace_deliver_sig;
        if (txn.outcome == A20_DELIVERY_HANDLER) {
            ARCH_TRAP_FAST_RETURN_DISARM(ctx);
            t->sig_saved_ctx = *ctx;
            t->sig_old_blocked = txn.old_blocked;
            t->sig_handling = txn.sig_handling;
            if (txn.reset_handler)
                ss->actions[txn.sig].sa_handler = SIG_DFL;
        }
        spin_unlock_irqrestore(&ss->lock, flags);

        if (txn.outcome == A20_DELIVERY_NONE)
            return;
        if (txn.outcome == A20_DELIVERY_DROP_IGNORED)
            continue;
        if (txn.outcome == A20_DELIVERY_PTRACE_STOP) {
            (void)proc_debug_signal_stop(txn.sig);
            continue;
        }
        if (txn.outcome == A20_DELIVERY_STOP) {
            proc_sched_stop_current(txn.sig);
            continue;
        }
        if (txn.outcome == A20_DELIVERY_FATAL) {
            /* CORE_DUMP_HOOK: emit the ELF core dump while the task's mm
             * and register context are still live, then terminate the
             * group. */
            int dumped = 0;
            if (signal_core_dump_default(txn.sig))
                dumped = coredump_on_fatal_signal(txn.sig, ctx);
            proc_exit_group(-signal_wait_status_dumped(txn.sig, dumped));
        }

        signal_build_handler_frame(ctx, t, txn.sig, txn.handler, txn.flags,
                                   txn.old_blocked, txn.has_info,
                                   &queued_info);
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
    int v = a20_signal_action_valid(signum, sigsetsize, ARCH_SIGSET_SIZE);
    if (v < 0) return v;

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
    mask = a20_signal_mask_strip_unblockable(mask);
    if (a20_signal_procmask_how_valid(how) < 0)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&ss->lock);
    t->sig_blocked = a20_signal_mask_apply(how, t->sig_blocked, mask);
    spin_unlock_irqrestore(&ss->lock, flags);
    return 0;
}
