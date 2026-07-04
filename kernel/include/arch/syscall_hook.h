#ifndef _ARCH_SYSCALL_HOOK_H
#define _ARCH_SYSCALL_HOOK_H

#include "abi/linux/syscall_entry.h"

/*
 * Architecture-specific syscall-number/argument normalization.
 *
 * The default implementation is a no-op; architectures whose user ABI differs
 * from the kernel-internal ABI (e.g. x86_64 Linux vs. the generic riscv64
 * numbering) override this in their arch-specific arch/syscall_hook.h.
 */
static inline void arch_syscall_adjust_args(linux_syscall_args_t *args)
{
    (void)args;
}

#endif /* _ARCH_SYSCALL_HOOK_H */
