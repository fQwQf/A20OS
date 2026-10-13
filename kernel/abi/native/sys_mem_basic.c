#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
/* Shared Native allocation entry; flat ownership is a memory-model adapter. */
#include "abi/native/syscall_entry.h"
#include "abi/native/errno.h"
#include "core/errno.h"
#include "core/mman.h"
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
    if (kargs.length > SIZE_MAX - (PAGE_SIZE - 1) ||
        (kargs.prot & ~7U) || (kargs.flags & ~MAP_ANONYMOUS) ||
        kargs.addr_hint >= USER_VA_LIMIT || (kargs.addr_hint & (PAGE_SIZE - 1)))
        return -A20_ERR_INVALID_ARGUMENT;
    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;
    mm_struct_t *mm = cur->mm;
    mm_tlb_invalidate_begin(mm);
    uint64_t lock_flags = spin_lock_irqsave(&mm->lock);
    uint64_t pages = ROUND_UP((size_t)kargs.length, PAGE_SIZE) / PAGE_SIZE;
    uint64_t limit = cur->limits.as / PAGE_SIZE;
    uint64_t addr;
    if (cur->limits.as && (mm->total_vm > limit || pages > limit - mm->total_vm))
        addr = (vaddr_t)-ENOMEM;
    else
        addr = mm_mmap_native_locked(mm, (vaddr_t)kargs.addr_hint,
                                     (size_t)kargs.length, kargs.prot,
                                     MAP_PRIVATE | MAP_ANONYMOUS);
    spin_unlock_irqrestore(&mm->lock, lock_flags);
    mm_tlb_invalidate_finish(mm);
    if (!addr || mm_addr_is_error((vaddr_t)addr))
        return -A20_ERR_NO_MEMORY;
    kargs.out_addr = addr;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        mm_munmap(mm, addr, (size_t)kargs.length);
        return -A20_ERR_FAULT;
    }
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
