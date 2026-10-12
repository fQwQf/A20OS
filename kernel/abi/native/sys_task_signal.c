/* Native task control is shared by MCU and server execution domains. */
#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
#include "proc/proc.h"
#include "proc/signal.h"
#include "proc/task_object.h"
#include "ipc/handle_table.h"
#include "sys/usercopy.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "uapi/a20/task_signal.h"
#include "sys_validate.h"
#define A20_ARG(n) (args->arg[(n)])

int64_t sys_a20_task_kill(const a20_syscall_args_t *args) {
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    int32_t sig = (int32_t)A20_ARG(1);
    uint64_t mode = A20_ARG(2);
    if ((mode & ~A20_SIGNAL_THREAD) || sig < 0 || sig >= NSIG)
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;

    task_t *target = NULL;
    int64_t r = a20_handle_task_get(ht, task_h, A20_OBJ_INVALID,
                                    A20_RIGHT_SIGNAL, &target);
    if (r < 0)
        return r;
    if (!target)
        return -A20_ERR_BAD_HANDLE;

    if (!sig) {
        proc_put(target);
        return A20_OK;
    }
    if (sig == 9) {
        /* Force the referenced target to exit at its safe boundary. Calling
         * proc_exit here would terminate the requesting agent instead. */
        proc_force_exit(target, 128 + 9);
        proc_put(target);
        return A20_OK;
    }

    /* Checkpoint-based delivery: record the signal and wake the target if it
     * is parked at a checkpoint (event/futex wait).  The handler runs on the
     * next explicit checkpoint (signal_check), never
     * asynchronously. */
    if (mode & A20_SIGNAL_THREAD) {
        a20_thread_sig_pend(target, sig);
    } else {
        struct a20_ht_internal *tht = task_get_a20_ht_ref(target);
        if (tht) {
            a20_ht_sig_pend(tht, sig);
            a20_ht_put_ref(tht);
        }
    }
    a20_task_object_notify(target, A20_EVENT_SIGNALED, (uint64_t)sig, 0, 0);
    proc_interrupt_wait(target, PROC_WAKE_SIGNAL);
    proc_put(target);
    return A20_OK;
}

/* Pointer transport avoids both argument and return-register truncation on
 * 32-bit CPUs. Its operation and layout are identical on every architecture. */
static int64_t signal_control(a20_signal_control_t *user) {
    a20_signal_control_t control;
    A20_VALIDATE_AND_COPY(user, control);
    task_t *cur = proc_current();
    if (!task_get_a20_ht(cur))
        return -A20_ERR_BAD_HANDLE;
    if (!(control.flags & A20_SIGNAL_THREAD))
        return -A20_ERR_INVALID_ARGUMENT;
    if (control.operation == A20_SIGNAL_CONTROL_CHECK) {
        if (control.flags &
            ~(A20_SIGNAL_THREAD | A20_SIGNAL_PEEK | A20_SIGNAL_SELECT))
            return -A20_ERR_INVALID_ARGUMENT;
        control.result =
            a20_thread_sig_check(cur, control.flags, control.selected);
    } else if (control.operation == A20_SIGNAL_CONTROL_MASK) {
        if (control.flags & ~(A20_SIGNAL_THREAD | A20_SIGNAL_MASK_QUERY))
            return -A20_ERR_INVALID_ARGUMENT;
        control.result = a20_thread_sig_mask(
            cur, control.mask, control.flags & A20_SIGNAL_MASK_QUERY);
    } else {
        return -A20_ERR_INVALID_ARGUMENT;
    }
    return a20_copy_struct_to_user(user, &control, sizeof(control));
}

int64_t sys_a20_signal_check(const a20_syscall_args_t *args) {
    uint64_t mode = A20_ARG(0);
    if (mode == A20_SIGNAL_STRUCT)
        return signal_control((void *)(uintptr_t)A20_ARG(1));
    if ((mode & ~(A20_SIGNAL_THREAD | A20_SIGNAL_PEEK | A20_SIGNAL_SELECT)) ||
        (mode && !(mode & A20_SIGNAL_THREAD)))
        return -A20_ERR_INVALID_ARGUMENT;
    task_t *cur = proc_current();
    if (mode & A20_SIGNAL_THREAD)
        return (int64_t)a20_thread_sig_check(cur, mode, A20_ARG(1));
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return 0;
    return (int64_t)a20_ht_sig_take(ht);
}

int64_t sys_a20_signal_mask(const a20_syscall_args_t *args) {
    uint64_t new_mask = A20_ARG(0);
    uint64_t *old_out = (uint64_t *)(uintptr_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht)
        return -A20_ERR_BAD_HANDLE;
    uint64_t mode = A20_ARG(2);
    if ((mode & ~(A20_SIGNAL_THREAD | A20_SIGNAL_MASK_QUERY)) ||
        (mode && !(mode & A20_SIGNAL_THREAD)))
        return -A20_ERR_INVALID_ARGUMENT;
    uint64_t old =
        (mode & A20_SIGNAL_THREAD)
            ? a20_thread_sig_mask(cur, new_mask, mode & A20_SIGNAL_MASK_QUERY)
            : a20_ht_sig_set_blocked(ht, new_mask);
    if (old_out && copy_to_user(old_out, &old, sizeof(old)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

#endif
