#ifndef _ARCH_AARCH64_SYSCALL_HOOK_H
#define _ARCH_AARCH64_SYSCALL_HOOK_H

#include "abi/linux/syscall_entry.h"

static inline void arch_syscall_adjust_args(linux_syscall_args_t *args)
{
    (void)args;
}

static inline void arch_adjust_clone_args(linux_syscall_args_t *args)
{
    uint64_t tmp = args->arg[3];
    args->arg[3] = args->arg[4];
    args->arg[4] = tmp;
}

#endif /* _ARCH_AARCH64_SYSCALL_HOOK_H */
