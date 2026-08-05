/*
 * A20OS — kernel-internal debugging interface (proc_debug_*).
 *
 * ABI-agnostic ptrace-class functionality: attaching an observer task to a
 * tracee, stopping the tracee at signal/syscall/exec/exit boundaries,
 * reading and writing its register file and address space, and resuming it
 * with a signal.  The Linux ABI layer (abi/linux/sys_ptrace.c) is a thin
 * request-number wrapper; a Native ABI debugger object maps onto the same
 * surface.
 *
 * State model (extending the task state contract in proc.h):
 *   TRACED task, running  -> receives signal   -> ptrace signal-stop
 *   TRACED task, syscall-stop mode -> syscall entry/exit -> ptrace stop
 *   TRACED task, TRACEEXEC -> execve          -> PTRACE_EVENT_EXEC stop
 *   TRACED task, TRACEEXIT  -> exit           -> PTRACE_EVENT_EXIT stop
 *   ptrace stop (PROC_STOPPED, ptrace_stop_active)
 *       -> resume (CONT/SYSCALL/DETACH) -> READY/RUNNING, signal optional
 *
 * ptrace_stop_active is published under proc_lock before the tracee blocks
 * in sched(), and cleared again under proc_lock on resume; the tracer's
 * read-only accesses (registers, siginfo, memory) happen only while it is
 * set, so no extra locking is needed on the snapshot fields.
 */

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "proc/debug.h"
#include "core/klog.h"
#include "core/string.h"
#include "mm/mm.h"
#include "mm/fault.h"

#define PT_DEBUG_OPTION_MASK  (PT_DEBUG_FLAG_SYSGOOD | \
                               PT_DEBUG_FLAG_TRACEEXEC | \
                               PT_DEBUG_FLAG_TRACEEXIT | \
                               PT_DEBUG_FLAG_EXITKILL)

static void ptrace_snapshot_siginfo(task_t *t, int sig)
{
    t->ptrace_siginfo_valid = 0;
    memset(t->ptrace_siginfo, 0, sizeof(t->ptrace_siginfo));
    if (!t || !t->signals)
        return;
    if (signal_task_get_pending_info(t, sig, t->ptrace_siginfo,
                                     sizeof(t->ptrace_siginfo)) == 0)
        t->ptrace_siginfo_valid = 1;
}

static void ptrace_fill_default_siginfo(task_t *t, int sig, int code)
{
    arch_siginfo_t *si = (arch_siginfo_t *)t->ptrace_siginfo;
    memset(si, 0, sizeof(*si));
    si->si_signo = sig;
    si->si_code = code;
    si->_sifields[0] = 0;
    si->_sifields[1] = proc_current() ? proc_current()->pid : 0;
    si->_sifields[2] = proc_current() ? (int)proc_current()->cred.uid : 0;
    t->ptrace_siginfo_valid = 1;
}

/*
 * Core stop primitive, executed by the tracee itself.  The STOPPED state
 * transition lives in proc_sched_stop_for_debug() (proc/sched.c, the
 * state-mutation owner) to keep the task-state boundary; this wrapper
 * publishes the observer-visible stop fields and blocks until the observer
 * resumes the task.  Returns 0 after a normal resume, -EINTR if a fatal
 * signal raced the stop (the caller should proceed with normal
 * delivery/exit instead of stopping).
 */
static int ptrace_stop_enter(int sig, int kind, int event, uint64_t msg)
{
    task_t *t = proc_current();
    if (!t)
        return -EINTR;
    if (!proc_debug_is_traced(t))
        return -EINTR;
    if (sig == SIGKILL)
        return -EINTR;

    if (t->trap_ctx) {
        t->ptrace_saved_ctx = *t->trap_ctx;
        t->ptrace_ctx_valid = 1;
    } else {
        t->ptrace_ctx_valid = 0;
    }
    t->ptrace_stop_sig = sig;
    t->ptrace_event = event;
    t->ptrace_event_msg = msg;
    t->ptrace_stop_kind = kind;
    t->ptrace_deliver_sig = 0;

    return proc_sched_stop_for_debug(t, sig);
}

int proc_debug_signal_stop(int sig)
{
    if (sig == SIGKILL)
        return -EINTR;
    ptrace_snapshot_siginfo(proc_current(), sig);
    if (!proc_current()->ptrace_siginfo_valid)
        ptrace_fill_default_siginfo(proc_current(), sig, 0 /* SI_USER */);
    return ptrace_stop_enter(sig, PT_DEBUG_STOP_SIGNAL, 0, 0);
}

int proc_debug_event_stop(int sig, int event, uint64_t msg)
{
    if (event == PT_DEBUG_EVENT_EXEC)
        ptrace_fill_default_siginfo(proc_current(), SIGTRAP, 1 /* TRAP_BRKPT */);
    else
        ptrace_fill_default_siginfo(proc_current(), SIGTRAP, 0);
    return ptrace_stop_enter(sig, PT_DEBUG_STOP_EVENT, event, msg);
}

/*
 * Shared tracer-side entry point: look up the tracee, validate that the
 * caller is its tracer, and return a referenced task.  Must be paired with
 * proc_put().
 */
static task_t *ptrace_tracee_get(int pid, int need_stopped)
{
    task_t *t = proc_find_get(pid);
    if (!t)
        return NULL;
    if (t->ptracer != proc_current() || !proc_debug_is_traced(t)) {
        proc_put(t);
        return NULL;
    }
    if (need_stopped) {
        uint64_t flags = spin_lock_irqsave(&proc_lock);
        int stopped = t->ptrace_stop_active && t->state == PROC_STOPPED;
        spin_unlock_irqrestore(&proc_lock, flags);
        if (!stopped) {
            proc_put(t);
            return NULL;
        }
    }
    return t;
}

int proc_debug_traceme(void)
{
    task_t *t = proc_current();
    if (!t || !t->parent)
        return -EPERM;
    if (proc_debug_is_traced(t))
        return -EPERM;
    /* The observing task is our parent.  The first exec or signal will
     * produce the initial stop. */
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    t->ptracer = t->parent;
    t->ptrace_flags |= PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_TRACEME;
    t->ptrace_orig_parent_pid = t->parent->pid;
    spin_unlock_irqrestore(&proc_lock, flags);
    return 0;
}

int proc_debug_attach(int pid)
{
    task_t *caller = proc_current();
    task_t *t = proc_find_get(pid);
    if (!t)
        return -ESRCH;
    if (t == caller || t == proc_idle_task()) {
        proc_put(t);
        return -EPERM;
    }

    if (!proc_task_may_access(caller, t)) {
        proc_put(t);
        return -EPERM;
    }

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state == PROC_UNUSED || t->state == PROC_ZOMBIE ||
        proc_debug_is_traced(t)) {
        spin_unlock_irqrestore(&proc_lock, flags);
        proc_put(t);
        return t->state == PROC_ZOMBIE ? -ESRCH : -EPERM;
    }

    t->ptracer = caller;
    t->ptrace_flags |= PT_DEBUG_FLAG_TRACED | PT_DEBUG_FLAG_ATTACHED;
    t->ptrace_orig_parent_pid = t->parent ? t->parent->pid : 0;
    if (t->parent && t->parent->state != PROC_UNUSED)
        t->ppid = caller->pid;
    t->parent = caller;
    spin_unlock_irqrestore(&proc_lock, flags);

    /* Queue SIGSTOP: the tracee stops at its next signal boundary and the
     * stop is reported to us as a ptrace stop (sig == SIGSTOP). */
    (void)signal_send_task(t, SIGSTOP);
    proc_put(t);
    return 0;
}

static void ptrace_reparent_to_original(task_t *t, int orig_pid)
{
    task_t *reaper = NULL;
    if (orig_pid > 0)
        reaper = proc_find_get(orig_pid);
    if (!reaper || reaper->state == PROC_UNUSED ||
        reaper->state == PROC_ZOMBIE) {
        if (reaper)
            proc_put(reaper);
        reaper = proc_find_get(1); /* init */
    }
    if (!reaper)
        return;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state != PROC_UNUSED) {
        t->parent = reaper;
        t->ppid = reaper->pid;
        if (t->state == PROC_ZOMBIE)
            proc_wake_child_waiters_locked(reaper);
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    proc_put(reaper);
}

static int ptrace_detach_internal(task_t *t, int sig)
{
    int resume = 0;
    int orig_pid = 0;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state == PROC_STOPPED && t->ptrace_stop_active) {
        resume = 1;
        t->stop_report_pending = 0;
        t->continue_report_pending = 0;
        t->ptrace_stop_active = 0;
        t->ptrace_stop_kind = PT_DEBUG_STOP_NONE;
        t->ptrace_event = 0;
        if (sig)
            t->ptrace_deliver_sig = sig;
    }
    t->ptracer = NULL;
    t->ptrace_flags = 0;
    if (t->parent == proc_current() || t->ptrace_orig_parent_pid > 0)
        orig_pid = t->ptrace_orig_parent_pid;
    spin_unlock_irqrestore(&proc_lock, flags);

    if (orig_pid > 0)
        ptrace_reparent_to_original(t, orig_pid);

    if (resume)
        (void)proc_sched_resume_stopped(t, 0);
    if (resume && sig)
        (void)signal_send_task(t, sig);
    return 0;
}

int proc_debug_detach(int pid, int sig)
{
    task_t *t = ptrace_tracee_get(pid, 0);
    if (!t)
        return -ESRCH;
    int ret = ptrace_detach_internal(t, sig);
    proc_put(t);
    return ret;
}

static int ptrace_resume_internal(task_t *t, int sig, int mode, int step)
{
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state != PROC_STOPPED || !t->ptrace_stop_active) {
        spin_unlock_irqrestore(&proc_lock, flags);
        return -ESRCH;
    }

    /* Fold in register edits made while stopped. */
    if (t->ptrace_ctx_valid && t->trap_ctx) {
        *t->trap_ctx = t->ptrace_saved_ctx;
    }

    if (t->ptrace_stop_kind == PT_DEBUG_STOP_SYSCALL_ENTRY && t->trap_ctx)
        arch_ptrace_rewind_syscall(t->trap_ctx);

    if (mode == PT_DEBUG_RESUME_SYSCALL)
        t->ptrace_flags |= PT_DEBUG_FLAG_SYSCALL;
    else
        t->ptrace_flags &= ~PT_DEBUG_FLAG_SYSCALL;

    if (step) {
        t->ptrace_flags |= PT_DEBUG_FLAG_STEP;
        if (t->trap_ctx)
            arch_ptrace_set_step(t->trap_ctx);
    } else {
        t->ptrace_flags &= ~PT_DEBUG_FLAG_STEP;
    }

    t->stop_report_pending = 0;
    t->continue_report_pending = 0;
    t->ptrace_stop_active = 0;
    t->ptrace_stop_kind = PT_DEBUG_STOP_NONE;
    t->ptrace_event = 0;
    if (sig)
        t->ptrace_deliver_sig = sig;
    spin_unlock_irqrestore(&proc_lock, flags);

    int resumed = proc_sched_resume_stopped(t, 0);
    if (resumed && sig)
        (void)signal_send_task(t, sig);
    return 0;
}

int proc_debug_resume(int pid, int sig, int mode)
{
    task_t *t = ptrace_tracee_get(pid, 0);
    if (!t)
        return -ESRCH;
    int ret = ptrace_resume_internal(t, sig, mode, 0);
    proc_put(t);
    return ret;
}

int proc_debug_singlestep(int pid, int sig)
{
    task_t *t = ptrace_tracee_get(pid, 0);
    if (!t)
        return -ESRCH;
#if defined(CONFIG_X86_64)
    int ret = ptrace_resume_internal(t, sig, PT_DEBUG_RESUME_CONT, 1);
#else
    /* No hardware single-step on this architecture (Linux riscv64 behaves
     * the same way); gdb falls back to breakpoint stepping. */
    int ret = -EIO;
#endif
    proc_put(t);
    return ret;
}

int proc_debug_kill(int pid)
{
    task_t *t = proc_find_get(pid);
    if (!t)
        return -ESRCH;
    if (t->ptracer != proc_current() || !proc_debug_is_traced(t)) {
        proc_put(t);
        return -ESRCH;
    }
    int ret = 0;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    if (t->state == PROC_STOPPED && t->ptrace_stop_active) {
        t->stop_report_pending = 0;
        t->ptrace_stop_active = 0;
        t->ptrace_stop_kind = PT_DEBUG_STOP_NONE;
        ret = 1; /* resumed; SIGKILL follows below */
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    if (ret)
        (void)proc_sched_resume_stopped(t, 0);
    proc_put(t);
    return signal_send(pid, SIGKILL) == 0 ? 0 : -ESRCH;
}

/* ---- address space access ---- */

int proc_debug_peek_word(int pid, uintptr_t addr, long *out)
{
    if (!out)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    int ret = -EIO;
    if (t->pgdir && addr < USER_VA_LIMIT) {
        void *kaddr = NULL;
        size_t avail = 0;
        if (mm_query_leaf_kaddr(t->pgdir, (vaddr_t)addr, &kaddr, &avail) &&
            avail >= sizeof(long)) {
            mm_leaf_info_t info;
            if (mm_query_leaf(t->pgdir, (vaddr_t)addr, &info) &&
                (info.flags & PTE_U)) {
                memcpy(out, kaddr, sizeof(*out));
                ret = 0;
            }
        }
    }
    proc_put(t);
    return ret;
}

int proc_debug_poke_word(int pid, uintptr_t addr, long data)
{
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    int ret = -EIO;
    if (t->pgdir && addr < USER_VA_LIMIT) {
        vaddr_t va = (vaddr_t)addr;
        mm_leaf_info_t info;
        /* Ptrace writes ignore PTE write permission (gdb inserts breakpoints
         * into read-only text).  A COW page must be broken first so the
         * shared frame is not corrupted. */
        if (t->pid == 8) {
            mm_leaf_info_t tli, sli;
            if (mm_query_leaf(t->pgdir, 0x11000, &tli) && mm_query_leaf(t->pgdir, va, &sli))
                printf("[DBG] poke pid=8 va=0x%lx pa=0x%lx text(0x11000)pa=0x%lx SAME=%d cow=%d\n",
                       (unsigned long)va, (unsigned long)sli.pa,
                       (unsigned long)tli.pa, sli.pa == tli.pa,
                       !!(sli.flags & PTE_COW));
        }
        if (mm_query_leaf(t->pgdir, va, &info) && (info.flags & PTE_U)) {
            if (info.flags & PTE_COW) {
                if (handle_cow_fault(t, va) < 0) {
                    proc_put(t);
                    return ret;
                }
            }
            void *kaddr = NULL;
            size_t avail = 0;
            if (mm_query_leaf_kaddr(t->pgdir, va, &kaddr, &avail) &&
                avail >= sizeof(long)) {
                memcpy(kaddr, &data, sizeof(data));
                ret = 0;
            }
        }
    }
    proc_put(t);
    return ret;
}

/* ---- register file access ---- */

int proc_debug_getregs(int pid, proc_debug_regs_t *out)
{
    if (!out)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    int ret;
    if (t->ptrace_ctx_valid) {
        memset(out, 0, sizeof(*out));
        arch_ptrace_export_regs(&t->ptrace_saved_ctx, out);
        ret = 0;
    } else {
        ret = -EIO;
    }
    proc_put(t);
    return ret;
}

int proc_debug_setregs(int pid, const proc_debug_regs_t *in)
{
    if (!in)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    int ret;
    if (t->ptrace_ctx_valid) {
        arch_ptrace_import_regs(&t->ptrace_saved_ctx, in);
        ret = 0;
    } else {
        ret = -EIO;
    }
    proc_put(t);
    return ret;
}

int proc_debug_getregset(int pid, int kind, void *out, size_t *size)
{
    if (!out || !size)
        return -EINVAL;
    size_t needed = sizeof(proc_debug_regs_t);
    if (*size < needed)
        return -EINVAL;
    proc_debug_regs_t regs;
    int ret = proc_debug_getregs(pid, &regs);
    if (ret)
        return ret;
    memcpy(out, &regs, needed);
    *size = needed;
    return 0;
}

int proc_debug_setregset(int pid, int kind, const void *in, size_t size)
{
    (void)kind;
    if (!in)
        return -EINVAL;
    proc_debug_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    size_t n = size < sizeof(regs) ? size : sizeof(regs);
    memcpy(&regs, in, n);
    return proc_debug_setregs(pid, &regs);
}

int proc_debug_getsiginfo(int pid, void *out, size_t size)
{
    if (!out)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    int ret = -EINVAL;
    if (t->ptrace_siginfo_valid) {
        size_t n = size < sizeof(t->ptrace_siginfo) ?
                   size : sizeof(t->ptrace_siginfo);
        memcpy(out, t->ptrace_siginfo, n);
        ret = 0;
    } else {
        ret = -EINVAL;
    }
    proc_put(t);
    return ret;
}

int proc_debug_setsiginfo(int pid, const void *in, size_t size)
{
    if (!in)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    size_t n = size < sizeof(t->ptrace_siginfo) ?
               size : sizeof(t->ptrace_siginfo);
    memcpy(t->ptrace_siginfo, in, n);
    t->ptrace_siginfo_valid = 1;
    proc_put(t);
    return 0;
}

int proc_debug_setoptions(int pid, unsigned long options)
{
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    t->ptrace_flags = (t->ptrace_flags & ~PT_DEBUG_OPTION_MASK) |
                      (options & PT_DEBUG_OPTION_MASK);
    spin_unlock_irqrestore(&proc_lock, flags);
    proc_put(t);
    return 0;
}

int proc_debug_geteventmsg(int pid, long *out)
{
    if (!out)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    *out = (long)t->ptrace_event_msg;
    proc_put(t);
    return 0;
}

/* ---- syscall boundary hooks (tracee context) ---- */

void proc_debug_syscall_entry(trap_context_t *ctx)
{
    task_t *t = proc_current();
    if (!t || !proc_debug_is_traced(t))
        return;
    if (!(t->ptrace_flags & PT_DEBUG_FLAG_SYSCALL))
        return;
    if (t->ptrace_deliver_sig)
        return; /* resume-with-signal: deliver first, then syscall runs */
    if (t->ptrace_stop_active)
        return;
    (void)ctx;
    ptrace_fill_default_siginfo(t, SIGTRAP, 4 /* TRAP_SYSCALL */);
    (void)ptrace_stop_enter(SIGTRAP, PT_DEBUG_STOP_SYSCALL_ENTRY, 0, 0);
}

void proc_debug_syscall_exit(trap_context_t *ctx)
{
    task_t *t = proc_current();
    if (!t || !proc_debug_is_traced(t))
        return;
    if (!(t->ptrace_flags & PT_DEBUG_FLAG_SYSCALL))
        return;
    if (t->ptrace_stop_active)
        return;
    (void)ctx;
    ptrace_fill_default_siginfo(t, SIGTRAP, 4 /* TRAP_SYSCALL */);
    (void)ptrace_stop_enter(SIGTRAP, PT_DEBUG_STOP_SYSCALL_EXIT, 0, 0);
}

/* ---- tracer death ---- */

void proc_debug_tracer_exiting(task_t *tracer)
{
    if (!tracer)
        return;

    int kill_pids[64];
    int resume_pids[64];
    int tracee_pids[64];
    int kill_count = 0, resume_count = 0, tracee_count = 0;

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t;
         t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED || t == tracer)
            continue;
        if (t->ptracer != tracer)
            continue;

        if (t->ptrace_flags & PT_DEBUG_FLAG_EXITKILL) {
            if (kill_count < 64)
                kill_pids[kill_count++] = t->pid;
        } else if (t->state == PROC_STOPPED && t->ptrace_stop_active) {
            if (resume_count < 64)
                resume_pids[resume_count++] = t->pid;
        }
        if (tracee_count < 64)
            tracee_pids[tracee_count++] = t->pid;

        t->ptracer = NULL;
        t->ptrace_flags = 0;
        t->ptrace_deliver_sig = 0;
        t->ptrace_stop_active = 0;
        t->ptrace_stop_kind = PT_DEBUG_STOP_NONE;
        t->stop_report_pending = 0;
        t->continue_report_pending = 0;
        t->ptrace_event = 0;
    }
    spin_unlock_irqrestore(&proc_lock, flags);

    for (int i = 0; i < tracee_count; i++) {
        task_t *t = proc_find_get(tracee_pids[i]);
        if (!t)
            continue;
        if (t->ptrace_orig_parent_pid > 0) {
            int orig = t->ptrace_orig_parent_pid;
            t->ptrace_orig_parent_pid = 0;
            /* The dying tracer's children were already reparented by the
             * caller (proc_reparent_children); restore the real parent for
             * attach-style tracees so the original parent reaps them. */
            ptrace_reparent_to_original(t, orig);
        }
        proc_put(t);
    }

    for (int i = 0; i < kill_count; i++)
        (void)proc_force_exit(proc_find_get(kill_pids[i]), -SIGKILL);
    for (int i = 0; i < resume_count; i++) {
        task_t *t = proc_find_get(resume_pids[i]);
        if (t) {
            (void)proc_sched_resume_stopped(t, 0);
            proc_put(t);
        }
    }
}
