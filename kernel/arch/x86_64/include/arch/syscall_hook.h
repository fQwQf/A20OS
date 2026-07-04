#ifndef _ARCH_X86_64_SYSCALL_HOOK_H
#define _ARCH_X86_64_SYSCALL_HOOK_H

#include "abi/linux/syscall_entry.h"
#include "syscall_nr_x86_64.h"

/*
 * x86_64 user programs use Linux syscall numbers/semantics, which differ from
 * the kernel-internal generic ABI.  Map the host syscall number and rewrite
 * any arguments that have an x86-specific layout before generic dispatch.
 */
static inline void arch_syscall_adjust_args(linux_syscall_args_t *args)
{
    uint32_t host_nr = args->nr;
    uint32_t kernel_nr = x86_syscall_to_kernel_nr(host_nr);

    if (kernel_nr != (uint32_t)-1) {
        args->nr = kernel_nr;
        x86_syscall_rewrite_args(host_nr, args);
    }
}

#endif /* _ARCH_X86_64_SYSCALL_HOOK_H */
