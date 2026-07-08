#ifndef _ARCH_RISCV32_SYSCALL_HOOK_H
#define _ARCH_RISCV32_SYSCALL_HOOK_H

#include "abi/linux/syscall_entry.h"

static inline void arch_syscall_adjust_args(linux_syscall_args_t *args) {
    (void)args;
}

static inline void arch_adjust_clone_args(linux_syscall_args_t *args) {
    (void)args;
}

#endif /* _ARCH_RISCV32_SYSCALL_HOOK_H */
