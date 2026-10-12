#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
/* Shared Native allocation entry; flat ownership is a memory-model adapter. */
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "core/errno.h"
#include <stdint.h>
#include "sys_validate.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/vm.h"
#ifdef CONFIG_FLAT_USER_MEMORY
#include "mm/flat_user.h"
static int64_t flat_status(int error) {
    switch (error) {
        case 0:
            return A20_OK;
        case -ENOMEM:
            return -A20_ERR_NO_MEMORY;
        case -EBUSY:
            return -A20_ERR_BUSY;
        case -EFAULT:
            return -A20_ERR_FAULT;
        default:
            return -A20_ERR_INVALID_ARGUMENT;
    }
}
#endif
int64_t sys_a20_vm_alloc(const a20_syscall_args_t *args) {
    a20_vm_alloc_args_t *uargs = (void *)(uintptr_t)args->arg[0];
    a20_vm_alloc_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    if (!kargs.length)
        return -A20_ERR_INVALID_ARGUMENT;
#ifdef CONFIG_FLAT_USER_MEMORY
    task_t *task = proc_current();
    if (task && task->flat_user) {
        if (kargs.length > SIZE_MAX || kargs.addr_hint || kargs.prot != 3 ||
            (kargs.flags != 0 && kargs.flags != 0x20))
            return -A20_ERR_INVALID_ARGUMENT;
        uintptr_t addr;
        int r = a20_flat_alloc(task->flat_user, (size_t)kargs.length, &addr);
        if (r)
            return flat_status(r);
        kargs.out_addr = addr;
        if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
            (void)a20_flat_unmap(task->flat_user, addr, (size_t)kargs.length);
            return -A20_ERR_FAULT;
        }
        return A20_OK;
    }
#endif
#ifndef CONFIG_MCU
    uint64_t addr = proc_mmap(kargs.addr_hint, (size_t)kargs.length,
                              (int)kargs.prot, 0x20, -1, 0);
    if (!addr || mm_addr_is_error((vaddr_t)addr))
        return -A20_ERR_NO_MEMORY;
    kargs.out_addr = addr;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
#else
    return -A20_ERR_NOT_SUPPORTED;
#endif
}
int64_t sys_a20_vm_unmap(const a20_syscall_args_t *args) {
    uint64_t addr = args->arg[0], length = args->arg[1];
    if (!length)
        return -A20_ERR_INVALID_ARGUMENT;
#ifdef CONFIG_FLAT_USER_MEMORY
    task_t *task = proc_current();
    if (task && task->flat_user) {
        if (addr > UINTPTR_MAX || length > SIZE_MAX)
            return -A20_ERR_INVALID_ARGUMENT;
        return flat_status(
            a20_flat_unmap(task->flat_user, (uintptr_t)addr, (size_t)length));
    }
#endif
#ifndef CONFIG_MCU
    return proc_munmap(addr, (size_t)length);
#else
    return -A20_ERR_NOT_SUPPORTED;
#endif
}
#endif
