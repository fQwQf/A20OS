#ifndef _ARCH_RISCV32_SYSCALL_HOOK_H
#define _ARCH_RISCV32_SYSCALL_HOOK_H

#include "abi/linux/syscall_entry.h"

static inline void arch_syscall_adjust_args(linux_syscall_args_t *args) {
    /* RV32 asm-generic passes 64-bit offsets in adjacent 32-bit registers.
     * The dispatch ABI keeps one uint64_t per argument; never drop the high
     * word or treat mmap2's page offset as a byte offset. */
    switch ((uint32_t)args->nr) {
    case SYS_truncate:
    case SYS_ftruncate:
        args->arg[1] = (uint32_t)args->arg[1] |
                      ((uint64_t)(uint32_t)args->arg[2] << 32);
        break;
    case SYS_pread64:
    case SYS_pwrite64:
    case SYS_preadv:
    case SYS_pwritev:
        args->arg[3] = (uint32_t)args->arg[3] |
                      ((uint64_t)(uint32_t)args->arg[4] << 32);
        break;
    case SYS_mmap:
        args->arg[5] = (uint64_t)(uint32_t)args->arg[5] << 12;
        break;
    default:
        break;
    }
}

static inline void arch_adjust_clone_args(linux_syscall_args_t *args) {
    (void)args;
}

#endif /* _ARCH_RISCV32_SYSCALL_HOOK_H */
