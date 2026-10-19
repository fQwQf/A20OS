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
 * ptrace_stop_active is published under the tracee's park_lock before the
 * tracee blocks in sched(), and cleared again under the same lock on resume;
 * the tracer's read-only accesses (registers, siginfo, memory) happen only
 * while it is set, so no extra locking is needed on the snapshot fields.
 */

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "proc/debug.h"
#include "proc/ptrace_state.h"
#include "core/klog.h"
#include "core/string.h"
#include "mm/mm.h"
#include "mm/fault.h"

#define PT_DEBUG_OPTION_MASK  (PT_DEBUG_FLAG_SYSGOOD | \
                               PT_DEBUG_FLAG_TRACEEXEC | \
                               PT_DEBUG_FLAG_TRACEEXIT | \
                               PT_DEBUG_FLAG_EXITKILL)

/* ---- ptrace state adapter ---- */

int a20_pt_view_fill(task_t *t, pt_view_t *out)
{
    if (!t)
        return 0;
    out->ptracer = (void **)&t->ptracer;
    out->flags = &t->ptrace_flags;
    out->stop_active = &t->ptrace_stop_active;
    out->stop_kind = &t->ptrace_stop_kind;
    out->event = &t->ptrace_event;
    out->event_msg = &t->ptrace_event_msg;
    out->stop_sig = &t->ptrace_stop_sig;
    out->siginfo_valid = &t->ptrace_siginfo_valid;
    out->deliver_sig = &t->ptrace_deliver_sig;
    out->ctx_valid = &t->ptrace_ctx_valid;
    out->orig_parent_pid = &t->ptrace_orig_parent_pid;
    out->stop_report_pending = &t->stop_report_pending;
    out->continue_report_pending = &t->continue_report_pending;
    out->state = (int32_t *)&t->state;
    out->pid = &t->pid;
    out->ppid = &t->ppid;
    out->parent = (void **)&t->parent;
    out->trap_ctx = (void **)&t->trap_ctx;
    return 1;
}

task_t *a20_pt_current(void) { return proc_current(); }

task_t *a20_pt_idle_task(void) { return proc_idle_task(); }

int a20_pt_may_access(task_t *a, task_t *b)
{
    return proc_task_may_access(a, b) ? 1 : 0;
}

int a20_pt_is_traced(const task_t *t) { return proc_debug_is_traced(t) ? 1 : 0; }

task_t *a20_pt_get(task_t *t) { return proc_get(t); }

void a20_pt_put(task_t *t) { proc_put(t); }

uint64_t a20_pt_park_lock(task_t *t) { return spin_lock_irqsave(&t->park_lock); }

void a20_pt_park_unlock(task_t *t, uint64_t flags)
{
    spin_unlock_irqrestore(&t->park_lock, flags);
}

/* The parent's state is sampled in its own critical section before the
 * target's lock is taken, so the two park_locks are never nested (INV-P3). */
int a20_pt_parent_alive(task_t *t)
{
    task_t *parent = t->parent;
    if (!parent)
        return 0;
    uint64_t pf = spin_lock_irqsave(&parent->park_lock);
    int pstate = parent->state;
    spin_unlock_irqrestore(&parent->park_lock, pf);
    return pstate != PROC_UNUSED ? 1 : 0;
}

void a20_pt_save_ctx(task_t *t)
{
    if (t->trap_ctx) {
        t->ptrace_saved_ctx = *t->trap_ctx;
        t->ptrace_ctx_valid = 1;
    } else {
        t->ptrace_ctx_valid = 0;
    }
}

int a20_pt_stop(task_t *t, int sig) { return proc_sched_stop_for_debug(t, sig); }

void a20_pt_snapshot_siginfo(task_t *t, int sig)
{
    if (!t)
        return;
    t->ptrace_siginfo_valid = 0;
    memset(t->ptrace_siginfo, 0, sizeof(t->ptrace_siginfo));
    if (!t->signals)
        return;
    if (signal_task_get_pending_info(t, sig, t->ptrace_siginfo,
                                     sizeof(t->ptrace_siginfo)) == 0)
        t->ptrace_siginfo_valid = 1;
}

void a20_pt_default_siginfo(task_t *t, int sig, int code)
{
    arch_siginfo_t *si = (arch_siginfo_t *)t->ptrace_siginfo;
    memset(si, 0, sizeof(*si));
    si->si_signo = sig;
    si->si_code = code;
    task_t *cur = proc_current();
    si->_sifields[0] = cur ? cur->pid : 0;
    si->_sifields[1] = cur ? (int)cur->cred.uid : 0;
    t->ptrace_siginfo_valid = 1;
}

void a20_pt_send_stop(task_t *t) { (void)signal_send_task(t, SIGSTOP); }

void a20_pt_fold_ctx(task_t *t, int kind)
{
    if (t->ptrace_ctx_valid && t->trap_ctx)
        *t->trap_ctx = t->ptrace_saved_ctx;
    if (kind == PT_DEBUG_STOP_SYSCALL_ENTRY && t->trap_ctx)
        arch_ptrace_rewind_syscall(t->trap_ctx);
}

int a20_pt_resume_stopped(task_t *t) { return proc_sched_resume_stopped(t, 0); }

int a20_pt_signal_send(int pid, int sig) { return signal_send(pid, sig); }

int proc_debug_signal_stop(int sig)
{
    return a20_pt_signal_stop(proc_current(), sig);
}

int proc_debug_event_stop(int sig, int event, uint64_t msg)
{
    return a20_pt_event_stop(proc_current(), sig, event, msg);
}

/*
 * Shared tracer-side entry point: look up the tracee, validate that the
 * caller is its tracer, and return a referenced task.  Must be paired with
 * proc_put().
 */
static task_t *ptrace_tracee_get(int pid, int need_stopped)
{
    /* The pid arrives from ptrace(2), so it is an id in the caller's own
     * namespace. */
    task_t *t = proc_find_get_user(pid);
    if (!t)
        return NULL;
    if (t->ptracer != proc_current() || !proc_debug_is_traced(t)) {
        proc_put(t);
        return NULL;
    }
    if (need_stopped) {
        uint64_t flags = spin_lock_irqsave(&t->park_lock);
        int stopped = t->ptrace_stop_active && t->state == PROC_STOPPED;
        spin_unlock_irqrestore(&t->park_lock, flags);
        if (!stopped) {
            proc_put(t);
            return NULL;
        }
    }
    return t;
}

int proc_debug_traceme(void)
{
    return (int)a20_pt_traceme(proc_current());
}

int proc_debug_attach(int pid)
{
    task_t *caller = proc_current();
    /* User-supplied pid: resolve it in the caller's own namespace. */
    task_t *t = proc_find_get_user(pid);
    if (!t)
        return -ESRCH;
    int64_t r = a20_pt_attach(t, caller, 0);
    proc_put(t);
    return (int)r;
}

int proc_debug_seize(int pid)
{
    task_t *caller = proc_current();
    /* User-supplied pid: resolve it in the caller's own namespace. */
    task_t *t = proc_find_get_user(pid);
    if (!t)
        return -ESRCH;
    int64_t r = a20_pt_attach(t, caller, 1);
    proc_put(t);
    return (int)r;
}

int proc_debug_interrupt(int pid)
{
    task_t *caller = proc_current();
    /* User-supplied pid: resolve it in the caller's own namespace. */
    task_t *t = proc_find_get_user(pid);
    if (!t)
        return -ESRCH;
    int64_t r = a20_pt_interrupt(t, caller);
    proc_put(t);
    return (int)r;
}

/* orig_pid is task_t::parent->pid as the KERNEL recorded it -- a global id --
 * so it is deliberately resolved with proc_find_get() and not the
 * caller-relative lookup: this runs from tracer teardown, where the recorded
 * parent may well live in a different namespace than the dying tracer. */
static void ptrace_reparent_to_original(task_t *t, int orig_pid)
{
    task_t *reaper = NULL;
    if (orig_pid > 0)
        reaper = proc_find_get(orig_pid);
    int rst = reaper ? proc_task_state_get(reaper) : PROC_UNUSED;
    if (!reaper || rst == PROC_UNUSED || rst == PROC_ZOMBIE) {
        if (reaper)
            proc_put(reaper);
        reaper = proc_find_get(1); /* init */
    }
    if (!reaper)
        return;
    /* The reparent rewrites the parent/children chains (tasklist_lock); the
     * waiter wake re-acquires that lock itself, so it runs after the unlock. */
    int wake = 0;
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    if (proc_task_state_get(t) != PROC_UNUSED) {
        proc_reparent_task_locked(reaper, t);
        wake = proc_task_state_get(t) == PROC_ZOMBIE;
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    if (wake)
        proc_wake_child_waiters(reaper);
    proc_put(reaper);
}

void a20_pt_reparent_original(task_t *t, int orig_pid)
{
    ptrace_reparent_to_original(t, orig_pid);
}

int proc_debug_detach(int pid, int sig)
{
    task_t *t = ptrace_tracee_get(pid, 0);
    if (!t)
        return -ESRCH;
    int64_t r = a20_pt_detach(t, sig);
    proc_put(t);
    return (int)r;
}

int proc_debug_resume(int pid, int sig, int mode)
{
    task_t *t = ptrace_tracee_get(pid, 0);
    if (!t)
        return -ESRCH;
    int64_t r = a20_pt_resume(t, sig, mode, 0);
    proc_put(t);
    return (int)r;
}

int proc_debug_singlestep(int pid, int sig)
{
    task_t *t = ptrace_tracee_get(pid, 0);
    if (!t)
        return -ESRCH;
#if ARCH_HW_SINGLE_STEP
    int64_t r = a20_pt_singlestep(t, sig);
#else
    /* No hardware single-step on this architecture (Linux riscv64 behaves the
     * same way); gdb falls back to breakpoint stepping. */
    (void)sig;
    int64_t r = -EIO;
#endif
    proc_put(t);
    return (int)r;
}

int proc_debug_kill(int pid)
{
    task_t *t = proc_find_get_user(pid);
    if (!t)
        return -ESRCH;
    int64_t r = a20_pt_kill(t, proc_current());
    proc_put(t);
    if (r < 0)
        return (int)r;
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

/* ---- bulk address space access ---- */

static long ptrace_copy_page(task_t *t, uintptr_t va, void *buf, size_t len,
                             int write)
{
    mm_leaf_info_t info;
    if (!mm_query_leaf(t->pgdir, (vaddr_t)va, &info) ||
        !(info.flags & PTE_U))
        return -EIO;

    if (write && (info.flags & PTE_COW)) {
        if (handle_cow_fault(t, (vaddr_t)va) < 0)
            return -EIO;
        if (!mm_query_leaf(t->pgdir, (vaddr_t)va, &info) ||
            !(info.flags & PTE_U))
            return -EIO;
    }

    void *kaddr = NULL;
    size_t avail = 0;
    if (!mm_query_leaf_kaddr(t->pgdir, (vaddr_t)va, &kaddr, &avail))
        return -EIO;
    if (avail > len)
        avail = len;
    if (write)
        memcpy(kaddr, buf, avail);
    else
        memcpy(buf, kaddr, avail);
    return (long)avail;
}

long proc_debug_read(int pid, uintptr_t addr, void *buf, size_t len)
{
    if (!buf)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    long done = 0;
    if (t->pgdir && addr < USER_VA_LIMIT) {
        while (done < (long)len) {
            uintptr_t va = addr + (uintptr_t)done;
            if (va >= USER_VA_LIMIT)
                break;
            long n = ptrace_copy_page(t, va, (char *)buf + done,
                                      len - (size_t)done, 0);
            if (n < 0)
                break;
            done += n;
        }
    }
    proc_put(t);
    return done > 0 ? done : -EIO;
}

long proc_debug_write(int pid, uintptr_t addr, const void *buf, size_t len)
{
    if (!buf)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    long done = 0;
    if (t->pgdir && addr < USER_VA_LIMIT) {
        while (done < (long)len) {
            uintptr_t va = addr + (uintptr_t)done;
            if (va >= USER_VA_LIMIT)
                break;
            long n = ptrace_copy_page(t, va, (char *)buf + done,
                                      len - (size_t)done, 1);
            if (n < 0)
                break;
            done += n;
        }
    }
    proc_put(t);
    return done > 0 ? done : -EIO;
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
    (void)kind;
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
    int64_t r = a20_pt_setoptions(t, (uint64_t)options);
    proc_put(t);
    return (int)r;
}

int proc_debug_geteventmsg(int pid, long *out)
{
    if (!out)
        return -EINVAL;
    task_t *t = ptrace_tracee_get(pid, 1);
    if (!t)
        return -ESRCH;
    *out = (long)a20_pt_geteventmsg(t);
    proc_put(t);
    return 0;
}

/* ---- syscall boundary hooks (tracee context) ---- */

void proc_debug_syscall_entry(trap_context_t *ctx)
{
    (void)ctx;
    (void)a20_pt_syscall_stop(proc_current(), 1);
}

void proc_debug_syscall_exit(trap_context_t *ctx)
{
    (void)ctx;
    (void)a20_pt_syscall_stop(proc_current(), 0);
}

/* ---- tracer death ---- */

void proc_debug_tracer_exiting(task_t *tracer)
{
    if (!tracer)
        return;

    pt_tracer_exit_lists_t lists;
    a20_pt_tracer_exiting(tracer, &lists);

    /* These arrays hold global ids the kernel itself recorded earlier, so
     * they are resolved globally rather than in any caller's namespace. */
    for (size_t i = 0; i < lists.tracee_count; i++) {
        task_t *t = proc_find_get(lists.tracee_pids[i]);
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

    for (size_t i = 0; i < lists.kill_count; i++) {
        task_t *t = proc_find_get(lists.kill_pids[i]);
        if (t) {
            (void)proc_force_exit(t, -SIGKILL);
            proc_put(t);
        }
    }
    for (size_t i = 0; i < lists.resume_count; i++) {
        task_t *t = proc_find_get(lists.resume_pids[i]);
        if (t) {
            (void)proc_sched_resume_stopped(t, 0);
            proc_put(t);
        }
    }
}
