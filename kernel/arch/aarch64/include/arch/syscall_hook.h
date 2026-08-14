#ifndef _ARCH_AARCH64_SYSCALL_HOOK_H
#define _ARCH_AARCH64_SYSCALL_HOOK_H

#include "abi/linux/syscall_entry.h"

static inline void arch_syscall_adjust_args(linux_syscall_args_t *args)
{
    (void)args;
}

/* AArch64 clone(2) wire order is (flags, newsp, parent_tidptr, tls,
 * child_tidptr): musl's clone.s moves tls into x3 and ctid into x4, which
 * already matches the A20OS sys_clone(flags, stack, ptid, tls, ctid)
 * signature.  Unlike LoongArch (whose clone.s emits (flags, stack, ptid,
 * ctid, tls)), no adjustment is needed here; swapping would install the
 * child tidptr as tpidr_el0 and break musl's __pthread_self(). */
static inline void arch_adjust_clone_args(linux_syscall_args_t *args)
{
    (void)args;
}

#endif /* _ARCH_AARCH64_SYSCALL_HOOK_H */
