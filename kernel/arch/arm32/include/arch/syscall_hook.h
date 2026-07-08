#ifndef _ARCH_ARM32_SYSCALL_HOOK_H
#define _ARCH_ARM32_SYSCALL_HOOK_H

#include "abi/linux/syscall_entry.h"
#include "syscall_nr_arm32.h"

static inline void arch_syscall_adjust_args(linux_syscall_args_t *args) {
    uint32_t host_nr = (uint32_t)args->nr;
    uint32_t kernel_nr = arm32_syscall_to_kernel_nr(host_nr);
    if (kernel_nr != (uint32_t)-1) {
        arm32_syscall_rewrite_args(host_nr, args);
        args->nr = kernel_nr;
    }
}

static inline void arch_adjust_clone_args(linux_syscall_args_t *args) {
    (void)args;
}

#endif
