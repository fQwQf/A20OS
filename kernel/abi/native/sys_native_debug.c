/*
 * A20OS Native ABI — Debug (0x0900).
 *
 * Thin wrapper over the ABI-agnostic kernel debugging interface
 * (proc_debug_*, kernel/proc/debug.c): attach/traceme/detach/resume,
 * stop/event reporting, register file access, and address-space
 * read/write.  This replaces the former limited-compatibility Debug
 * partition (NATIVE_DEBUG_LIMITED_CONTRACT) with full stop/resume
 * semantics backed by the same state machine the Linux ptrace(2) wrapper
 * uses.
 *
 * Object model: a debug session is an A20_OBJ_DEBUG handle whose object is
 * the target's pid.  Operation rights (docs/native-abi/06-security.md §3.2):
 *   READ      debug_read / debug_read_regs / debug_map_memory
 *   WRITE     debug_write / debug_write_regs
 *   WAIT      debug_wait / debug_event
 *   SIGNAL    debug_kill
 *   CONTROL   debug_resume / debug_detach
 *   ADMIN     debug_attach (requires ADMIN on the target task handle)
 */

#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/timer.h"
#include "trap_frame.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/debug.h"
#include "mm/mm.h"
#include "sys/usercopy.h"
#include "abi/native/types.h"
#include "abi/native/objects.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/handle_table.h"

#define A20_ARG(n) (args->arg[(n)])

/* Resolve a debug handle to a referenced target task.  Caller must
 * proc_put() the result. */
static task_t *a20_debug_target_get(struct a20_ht_internal *ht,
                                    a20_handle_t dbg_h,
                                    a20_rights_t required_rights)
{
    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            required_rights, &entry);
    if (r < 0)
        return NULL;
    return proc_find_get((int)(uintptr_t)entry.object);
}

/* Snapshot the current stop into a user-visible event structure.  Caller
 * holds proc_lock; the tracee is stopped, so the fields are stable. */
static void a20_debug_fill_event(task_t *target, a20_debug_event_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->version = 1;
    info->kind = (uint32_t)target->ptrace_stop_kind;
    info->sig = (uint32_t)target->ptrace_stop_sig;
    info->event = (uint32_t)target->ptrace_event;
    info->event_msg = target->ptrace_event_msg;
}

/* The Native ABI has no wait4/WUNTRACED channel, so debug_wait/debug_event
 * also report task exit: a zombie target is reported once as an EXIT event
 * whose message is the exit code (ptrace_exit_reported is the one-shot
 * marker).  Caller holds proc_lock. */
static int a20_debug_target_has_event(task_t *target)
{
    if (target->ptrace_stop_active && target->state == PROC_STOPPED)
        return 1;
    if (target->state == PROC_ZOMBIE && !target->ptrace_exit_reported)
        return 1;
    return 0;
}

static void a20_debug_fill_exit_event(task_t *target,
                                      a20_debug_event_info_t *info)
{
    memset(info, 0, sizeof(*info));
    info->size = sizeof(*info);
    info->version = 1;
    info->kind = A20_DEBUG_STOP_EVENT;
    info->event = A20_DEBUG_EVENT_EXIT;
    info->event_msg = (uint64_t)(uintptr_t)target->exit_code;
    target->ptrace_exit_reported = 1;
}

int64_t sys_a20_debug_traceme(const a20_syscall_args_t *args)
{
    (void)args;
    int r = proc_debug_traceme();
    if (r < 0)
        return -A20_ERR_ACCESS;
    return A20_OK;
}

int64_t sys_a20_debug_attach(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    /* docs/native-abi/06-security.md §8.1: attach requires ADMIN right on
     * the target task handle; proc_debug_attach re-checks uid/capability. */
    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_ADMIN, &entry);
    if (r < 0) return r;

    int pid = (int)(uintptr_t)entry.object;
    task_t *target = proc_find_get(pid);
    if (!target) return -A20_ERR_BAD_HANDLE;
    proc_put(target);
    if (target && proc_current())
        printf("[NDBG] attach pid=%d cur=%d state=%d cur_uid=%d/%d/%d/%d tgt_uid=%d/%d/%d/%d cap=0x%lx\n",
               pid, proc_current()->pid, (int)target->state,
               proc_current()->cred.fsuid, proc_current()->cred.uid,
               proc_current()->cred.euid, proc_current()->cred.suid,
               target->cred.fsuid, target->cred.uid,
               target->cred.euid, target->cred.suid,
               (unsigned long)proc_current()->cred.cap_effective);

    r = proc_debug_attach(pid);
    printf("[NDBG] attach ret=%ld\n", (long)r);
    if (r == -EPERM) return -A20_ERR_ACCESS;
    if (r < 0) return -A20_ERR_BAD_HANDLE;

    /* The session handle carries every debug operation right; a holder may
     * narrow them with handle_replace later. */
    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)pid, A20_OBJ_DEBUG,
                                   A20_RIGHT_READ | A20_RIGHT_WRITE |
                                   A20_RIGHT_WAIT | A20_RIGHT_SIGNAL |
                                   A20_RIGHT_CONTROL | A20_RIGHT_ADMIN);
    if (h < 0)
        (void)proc_debug_detach(pid, 0);
    return h;
}

int64_t sys_a20_debug_wait(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    uint64_t timeout_us = A20_ARG(1);
    a20_debug_event_info_t *out = (a20_debug_event_info_t *)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    uint64_t deadline = 0;
    if (timeout_us != 0 && timeout_us != A20_TIMEOUT_INFINITE)
        deadline = timer_get_ticks() + US_TO_TICKS(timeout_us);

    for (;;) {
        task_t *target = a20_debug_target_get(ht, dbg_h, A20_RIGHT_WAIT);
        if (!target)
            return -A20_ERR_BAD_HANDLE;

        uint64_t flags = spin_lock_irqsave(&proc_lock);
        int stopped = a20_debug_target_has_event(target);
        if (stopped) {
            a20_debug_event_info_t info;
            if (target->ptrace_stop_active && target->state == PROC_STOPPED) {
                a20_debug_fill_event(target, &info);
                /* One-shot report: a later debug_wait waits for the next
                 * stop. */
                target->stop_report_pending = 0;
            } else {
                a20_debug_fill_exit_event(target, &info);
            }
            spin_unlock_irqrestore(&proc_lock, flags);
            proc_put(target);
            if (out) {
                if (copy_to_user(out, &info, sizeof(info)) < 0)
                    return -A20_ERR_FAULT;
            }
            return A20_OK;
        }
        if (deadline != 0 && timer_get_ticks() >= deadline) {
            spin_unlock_irqrestore(&proc_lock, flags);
            proc_put(target);
            return -A20_ERR_TIMED_OUT;
        }
        if (timeout_us == 0) {
            spin_unlock_irqrestore(&proc_lock, flags);
            proc_put(target);
            return -A20_ERR_WOULD_BLOCK;
        }

        /* Park like a child waiter; the tracee's stop path wakes child
         * waiters of its (reparented) parent, i.e. us. */
        cur->waiting_for_child = 1;
        proc_wait_token_t token =
            proc_park_prepare_locked(PROC_WAIT_INTERRUPTIBLE, deadline);
        spin_unlock_irqrestore(&proc_lock, flags);
        proc_put(target);

        proc_wake_reason_t reason = proc_park_commit(token);
        proc_park_finish(token);

        uint64_t f2 = spin_lock_irqsave(&proc_lock);
        cur->waiting_for_child = 0;
        spin_unlock_irqrestore(&proc_lock, f2);

        if (reason == PROC_WAKE_TIMEOUT)
            return -A20_ERR_TIMED_OUT;
        /* Spurious wake (e.g. SIGCHLD traffic): re-check the stop. */
    }
}

int64_t sys_a20_debug_event(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    a20_debug_event_info_t *out = (a20_debug_event_info_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    task_t *target = a20_debug_target_get(ht, dbg_h, A20_RIGHT_WAIT);
    if (!target) return -A20_ERR_BAD_HANDLE;

    a20_debug_event_info_t info;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    int stopped = a20_debug_target_has_event(target);
    if (stopped) {
        if (target->ptrace_stop_active && target->state == PROC_STOPPED)
            a20_debug_fill_event(target, &info);
        else
            a20_debug_fill_exit_event(target, &info);
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    proc_put(target);

    if (!stopped)
        return -A20_ERR_WOULD_BLOCK;
    if (copy_to_user(out, &info, sizeof(info)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_debug_resume(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    uint32_t mode = (uint32_t)A20_ARG(1);

    if (mode != A20_DEBUG_RESUME_CONT &&
        mode != A20_DEBUG_RESUME_SYSCALL)
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;
    int pid = (int)(uintptr_t)entry.object;

    int pr = proc_debug_resume(pid, 0,
                mode == A20_DEBUG_RESUME_SYSCALL ?
                PT_DEBUG_RESUME_SYSCALL : PT_DEBUG_RESUME_CONT);
    if (pr == -ESRCH) return -A20_ERR_BAD_HANDLE;
    return A20_OK;
}

int64_t sys_a20_debug_detach(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;
    int pid = (int)(uintptr_t)entry.object;

    int pr = proc_debug_detach(pid, 0);
    (void)a20_handle_remove(ht, dbg_h);
    if (pr == -ESRCH) return -A20_ERR_BAD_HANDLE;
    return A20_OK;
}

int64_t sys_a20_debug_kill(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_SIGNAL, &entry);
    if (r < 0) return r;
    int pid = (int)(uintptr_t)entry.object;

    int pr = proc_debug_kill(pid);
    if (pr == -ESRCH) return -A20_ERR_BAD_HANDLE;
    return A20_OK;
}

int64_t sys_a20_debug_read(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    uint64_t addr = A20_ARG(1);
    uint64_t len = A20_ARG(2);
    void *buf = (void *)A20_ARG(3);
    if (!buf || len == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;
    int pid = (int)(uintptr_t)entry.object;

    char kbuf[512];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        long n = proc_debug_read(pid, (uintptr_t)(addr + done), kbuf, chunk);
        if (n < 0) {
            if (done > 0) break;
            return -A20_ERR_FAULT;
        }
        if (copy_to_user((char *)buf + done, kbuf, (size_t)n) < 0)
            return -A20_ERR_FAULT;
        done += (uint64_t)n;
    }
    return (int64_t)done;
}

int64_t sys_a20_debug_write(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    uint64_t addr = A20_ARG(1);
    uint64_t len = A20_ARG(2);
    const void *buf = (const void *)A20_ARG(3);
    if (!buf || len == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;
    int pid = (int)(uintptr_t)entry.object;

    char kbuf[512];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        if (copy_from_user(kbuf, (const char *)buf + done, chunk) < 0)
            return -A20_ERR_FAULT;
        long n = proc_debug_write(pid, (uintptr_t)(addr + done), kbuf, chunk);
        if (n < 0) {
            if (done > 0) break;
            return -A20_ERR_FAULT;
        }
        done += (uint64_t)n;
    }
    return (int64_t)done;
}

int64_t sys_a20_debug_read_regs(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    a20_regs_t *out = (a20_regs_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;
    int pid = (int)(uintptr_t)entry.object;

    proc_debug_regs_t regs;
    r = proc_debug_getregs(pid, &regs);
    if (r == -ESRCH) return -A20_ERR_BAD_HANDLE;
    if (r < 0) return -A20_ERR_WOULD_BLOCK; /* not stopped */

    a20_regs_t out_regs;
    memset(&out_regs, 0, sizeof(out_regs));
    for (int i = 0; i < 32; i++)
        out_regs.regs[i] = regs.regs[i];
    out_regs.pc = regs.pc;
    out_regs.sp = regs.sp;
    out_regs.sr = regs.status;
    if (copy_to_user(out, &out_regs, sizeof(out_regs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_debug_write_regs(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    const a20_regs_t *uregs = (const a20_regs_t *)A20_ARG(1);
    if (!uregs) return -A20_ERR_FAULT;

    a20_regs_t kregs;
    if (copy_from_user(&kregs, uregs, sizeof(kregs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;
    int pid = (int)(uintptr_t)entry.object;

    /* Fetch first so arch-specific fields outside a20_regs_t survive. */
    proc_debug_regs_t regs;
    r = proc_debug_getregs(pid, &regs);
    if (r == -ESRCH) return -A20_ERR_BAD_HANDLE;
    if (r < 0) return -A20_ERR_WOULD_BLOCK;
    for (int i = 0; i < 32; i++)
        regs.regs[i] = kregs.regs[i];
    regs.pc = kregs.pc;
    regs.sp = kregs.sp;
    regs.status = kregs.sr;
    r = proc_debug_setregs(pid, &regs);
    if (r == -ESRCH) return -A20_ERR_BAD_HANDLE;
    if (r < 0) return -A20_ERR_WOULD_BLOCK;
    return A20_OK;
}

int64_t sys_a20_debug_map_memory(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    uint64_t remote_addr = A20_ARG(1);
    uint64_t len = A20_ARG(2);
    uint32_t prot = (uint32_t)A20_ARG(3);

    if (len == 0 || len > (16ULL << 20)) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;
    int pid = (int)(uintptr_t)entry.object;

    uint64_t local = proc_mmap(0, (size_t)len, (int)prot ? (int)prot : 3,
                               0x20 /* MAP_ANONYMOUS */, -1, 0);
    if (local == 0)
        return -A20_ERR_NO_MEMORY;

    char kbuf[512];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        long n = proc_debug_read(pid, (uintptr_t)(remote_addr + done),
                                 kbuf, chunk);
        if (n < 0)
            goto out_unmap;
        if (copy_to_user((void *)(local + done), kbuf, (size_t)n) < 0) {
            r = -A20_ERR_FAULT;
            goto out_unmap;
        }
        done += (uint64_t)n;
    }
    return (int64_t)local;

out_unmap:
    proc_munmap(local, (size_t)len);
    return r < 0 ? r : -A20_ERR_FAULT;
}
