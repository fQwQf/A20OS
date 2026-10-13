/* Native thread admission is shared by MMU servers and flat execution domains.
 * No child runs until its handle and userspace result have been published. */
#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "proc/proc.h"
#include "core/errno.h"
#include "ipc/handle_table.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "sys_validate.h"
#define A20_ARG(n) (args->arg[(n)])

int64_t sys_a20_thread_create(const a20_syscall_args_t *args) {
    a20_thread_create_args_t *uargs = (void *)(uintptr_t)A20_ARG(0);
    a20_thread_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    if (!kargs.entry || !kargs.stack_base || !kargs.stack_size || kargs.flags ||
        kargs.entry > UINTPTR_MAX || kargs.arg > UINTPTR_MAX ||
        kargs.stack_base > UINTPTR_MAX || kargs.stack_size > SIZE_MAX ||
        kargs.tls_base > UINTPTR_MAX || kargs.stack_size > kargs.stack_base)
        return -A20_ERR_INVALID_ARGUMENT;
    struct a20_ht_internal *ht = task_get_a20_ht(proc_current());
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    task_t *child = NULL;
    int pid = proc_create_thread_deferred(
        kargs.entry, kargs.arg, (vaddr_t)kargs.stack_base,
        (vaddr_t)kargs.tls_base, (size_t)kargs.stack_size, &child);
    if (pid < 0) {
        if (pid == -EINVAL)
            return -A20_ERR_INVALID_ARGUMENT;
        if (pid == -EFAULT)
            return -A20_ERR_FAULT;
        if (pid == -EOPNOTSUPP)
            return -A20_ERR_NOT_SUPPORTED;
        return -A20_ERR_NO_MEMORY;
    }
    if (!child) {
        proc_free_pid(pid);
        return -A20_ERR_NO_MEMORY;
    }
    int64_t h = a20_handle_install_task(
        ht, child, A20_OBJ_THREAD,
        A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT | A20_RIGHT_CONTROL |
            A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_ADMIN |
            A20_RIGHT_READ | A20_RIGHT_WRITE);
    if (h < 0) {
        proc_free_pid(pid);
        proc_put(child);
        return h;
    }
    kargs.out_thread = (a20_handle_t)h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)h);
        proc_free_pid(pid);
        proc_put(child);
        return -A20_ERR_FAULT;
    }
    child->a20_joinable = 1;
    proc_publish_deferred_task(child);
    proc_put(child);
    return h;
}

int64_t sys_a20_handle_close(const a20_syscall_args_t *args) {
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    /*
     * handle_remove detaches the entry and drops the object reference
     * (docs/native-abi/03-handle.md §4.1): vfile-backed objects are closed,
     * channels see peer_closed, event queues and VMOs are released, and
     * watches keyed by the object are torn down.  Closing consumes no
     * operation budget because no lookup is performed.
     */
    return a20_handle_remove(ht, h);
}

int64_t sys_a20_task_wait(const a20_syscall_args_t *args) {
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_flags_t flags = (a20_flags_t)A20_ARG(1);
    a20_task_status_t *out = (a20_task_status_t *)(uintptr_t)A20_ARG(2);
    if (flags & ~A20_TASK_WAIT_NONBLOCK)
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    /* Lookup task handle with WAIT right docs/native-abi/06-security.md §3.2) */
    task_t *target = NULL;
    int64_t ret = a20_handle_task_get(ht, task_h, A20_OBJ_INVALID,
                                      A20_RIGHT_WAIT, &target);
    if (ret < 0)
        return ret;
    if (!target)
        return -A20_ERR_BAD_HANDLE;

    int status = 0;
    /* WNOHANG=1: non-block mode reports WOULD_BLOCK until the target exits. */
    int wret = proc_wait4(target->pid, &status,
                          (flags & A20_TASK_WAIT_NONBLOCK) ? 1 : 0);
    proc_put(target);
    /* Interrupts leave the child and its handle available for a later join.
     * Native callers consume their pending signals and choose whether to retry. */
    if (wret == -ERESTARTSYS || wret == -EINTR)
        return -A20_ERR_INTERRUPTED;
    if (wret < 0)
        return -A20_ERR_INVALID_ARGUMENT;
    if (wret == 0)
        return -A20_ERR_WOULD_BLOCK;

    if (out) {
        a20_task_status_t ts;
        memset(&ts, 0, sizeof(ts));
        ts.size = sizeof(ts);
        ts.version = 1;
        /* Preserve fatal Linux exits after an owner-managed task execs.
         * Native explicit exit codes retain their existing representation. */
        ts.exit_code =
            (status & 0x7f) ? 128 + (status & 0x7f) : (status >> 8) & 0xff;
        ts.exit_reason = 0;
        if (copy_to_user(out, &ts, sizeof(ts)) < 0)
            return -A20_ERR_FAULT;
    }
    return A20_OK;
}

#endif
