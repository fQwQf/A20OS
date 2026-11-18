#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
/*
 * A20OS Native ABI — Sync (0x0B00) syscall implementations.
 *
 * A futex is a synchronisation primitive on a user address, not a kernel
 * object, so it is not allocated a handle.  Its semantics match Zircon's
 * zx_futex_wait / zx_futex_wake; the implementation reuses the kernel futex
 * core (ipc/futex.c) and maps error codes into the native errno
 * space.
 * Design reference: docs/zh/native-abi/01-types.md §22
 */
#include "core/types.h"
#include "core/defs.h"
#include "sys/usercopy.h"
#include "sys/futex.h"
#include "core/errno.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"

static int64_t a20_sync_map_status(int err)
{
    if (err >= 0) return err;
    switch (-err) {
    case EAGAIN:       return -A20_ERR_WOULD_BLOCK;
    case ETIMEDOUT:    return -A20_ERR_TIMED_OUT;
    case EFAULT:       return -A20_ERR_FAULT;
    case EINVAL:       return -A20_ERR_INVALID_ARGUMENT;
    case ENOMEM:       return -A20_ERR_NO_MEMORY;
    case ERESTARTSYS:
    case EINTR:        return -A20_ERR_INTERRUPTED;
    default:           return -A20_ERR_IO;
    }
}

int64_t sys_a20_futex_wait(const a20_syscall_args_t *args)
{
    a20_futex_wait_args_t *uargs = (a20_futex_wait_args_t *)(uintptr_t)args->arg[0];
    if (!uargs) return -A20_ERR_FAULT;

    a20_futex_wait_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags != 0) return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.addr > UINTPTR_MAX || !kargs.addr)
        return -A20_ERR_FAULT;
    if (kargs.addr & (sizeof(uint32_t) - 1)) return -A20_ERR_INVALID_ARGUMENT;

    int r = futex_wait_user_ns((int *)(uintptr_t)kargs.addr,
                               (int)kargs.expected, kargs.timeout_ns);
    return a20_sync_map_status(r);
}

int64_t sys_a20_futex_wake(const a20_syscall_args_t *args)
{
    a20_futex_wake_args_t *uargs = (a20_futex_wake_args_t *)(uintptr_t)args->arg[0];
    if (!uargs) return -A20_ERR_FAULT;

    a20_futex_wake_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.flags != 0) return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.count == 0 || kargs.reserved)
        return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.addr > UINTPTR_MAX || !kargs.addr)
        return -A20_ERR_FAULT;
    if (kargs.addr & (sizeof(uint32_t) - 1)) return -A20_ERR_INVALID_ARGUMENT;

    /* The Native API requires an accessible user word. The kernel-internal
     * wake helper remains usable by drivers that already own its lifetime. */
    uint32_t probe;
    if (copy_from_user(&probe, (void *)(uintptr_t)kargs.addr, sizeof(probe)) <
        0)
        return -A20_ERR_FAULT;

    uint32_t count = kargs.count;
    if (count > (uint32_t)INT32_MAX) count = (uint32_t)INT32_MAX; /* "wake all" */

    int r = futex_wake_user((int *)(uintptr_t)kargs.addr, (int)count);
    if (r < 0) return a20_sync_map_status(r);

    kargs.out_woken = (uint32_t)r;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_atomic32(const a20_syscall_args_t *args) {
    a20_atomic32_args_t *uargs = (void *)(uintptr_t)args->arg[0];
    a20_atomic32_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    if (kargs.flags || kargs.operation > A20_ATOMIC32_COMPARE_EXCHANGE)
        return -A20_ERR_INVALID_ARGUMENT;
    if (!kargs.addr || kargs.addr > UINTPTR_MAX)
        return -A20_ERR_FAULT;
    int r = futex_atomic_user32((uint32_t *)(uintptr_t)kargs.addr,
                                kargs.operation, kargs.expected, kargs.desired,
                                &kargs.observed, &kargs.exchanged);
    if (r < 0)
        return a20_sync_map_status(r);
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}
#endif
