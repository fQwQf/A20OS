/*
 * A20OS Architecture Abstraction Layer
 *
 * This header dispatches to the correct arch-specific headers based on
 * the CONFIG_* define set by the Makefile (-DCONFIG_RISCV64,
 * -DCONFIG_RISCV32, -DCONFIG_LOONGARCH64, -DCONFIG_AARCH64,
 * -DCONFIG_ARM32, -DCONFIG_ARMV7M, -DCONFIG_PPC64LE or -DCONFIG_X86_64).
 *
 * All arch-specific code (inline asm, register access, page table format,
 * trap context layout, hardware addresses) lives under kernel/arch/$(ARCH)/.
 * Shared kernel code includes only this header (or the individual
 * sub-headers it pulls in) and NEVER touches arch-specific registers or
 * instructions directly.
 */
#ifndef _ARCH_H
#define _ARCH_H

#include "core/types.h"

/*
 * Sub-headers provided by each architecture:
 *   arch.h       — master include (pulls in everything below)
 *   cpu.h        — barriers, irq control, wfi, TLB flush, CSR/register access
 *   page_table.h — page table format (PTE flags, VPN/PPN macros, SATP/TTBR)
 *   trap_frame.h — trap_context_t, task_context_t, syscall register access
 *   platform.h   — HW base addresses, IRQ numbers, exception codes, PAGE_OFFSET
 *   firmware.h   — shutdown/reboot/console/timer firmware calls (SBI or equiv)
 */

#if defined(CONFIG_RISCV64)
# include "arch/riscv64/include/arch.h"
#elif defined(CONFIG_RISCV32)
# include "arch/riscv32/include/arch.h"
#elif defined(CONFIG_LOONGARCH64)
# include "arch/loongarch64/include/arch.h"
#elif defined(CONFIG_AARCH64)
# include "arch/aarch64/include/arch.h"
#elif defined(CONFIG_ARM32)
# include "arch/arm32/include/arch.h"
#elif defined(CONFIG_ARMV7M)
# include "arch/armv7m/include/arch.h"
#elif defined(CONFIG_PPC64LE)
# include "arch/ppc64le/include/arch.h"
#elif defined(CONFIG_X86_64)
# include "arch/x86_64/include/arch.h"
#else
# error "No architecture defined. Set ARCH=riscv64, ARCH=riscv32, ARCH=loongarch64, ARCH=aarch64, ARCH=arm32, ARCH=armv7m, ARCH=ppc64le or ARCH=x86_64."
#endif

/*
 * Optional architecture capabilities.  Architecture headers opt in by
 * defining the corresponding ARCH_* macro; shared scheduler/process code
 * consumes only these hooks and does not branch on CONFIG_<architecture>.
 */
#ifndef ARCH_TASK_CONTEXT_SET_USER_TP
# define ARCH_TASK_CONTEXT_SET_USER_TP(ctx, user_tp) \
    do { (void)(ctx); (void)(user_tp); } while (0)
#endif

#ifndef ARCH_PT_LEVEL_ENTRIES
# define ARCH_PT_LEVEL_ENTRIES(level) ARCH_PT_ENTRIES
#endif

#ifndef ARCH_PT_ROOT_ORDER
# define ARCH_PT_ROOT_ORDER 0
#endif

static inline int arch_pt_level_entries(int level)
{
    (void)level;
    return ARCH_PT_LEVEL_ENTRIES(level);
}

#ifndef ARCH_FORK_REQUIRES_PRIVATE_COPY
# define ARCH_FORK_REQUIRES_PRIVATE_COPY 0
#endif

static inline int arch_fork_requires_private_copy(void)
{
    return ARCH_FORK_REQUIRES_PRIVATE_COPY;
}

static inline void arch_task_context_set_user_tp(task_context_t *ctx,
                                                  uintptr_t user_tp)
{
    ARCH_TASK_CONTEXT_SET_USER_TP(ctx, user_tp);
}

static inline void arch_syscall_dispatch_enter(void)
{
#ifndef ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE
    arch_local_irq_enable();
#endif
}

static inline void arch_syscall_dispatch_leave(void)
{
#ifndef ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE
    arch_local_irq_disable();
#endif
}

static inline int arch_syscall_resched_allowed(void)
{
#ifdef ARCH_SYSCALL_DISPATCH_NONPREEMPTIBLE
    return 0;
#else
    return 1;
#endif
}

/* Arch name string (for uname, procfs, etc.) */
#if defined(CONFIG_RISCV64)
# define ARCH_NAME "riscv64"
#elif defined(CONFIG_RISCV32)
# define ARCH_NAME "riscv32"
#elif defined(CONFIG_LOONGARCH64)
# define ARCH_NAME "loongarch64"
#elif defined(CONFIG_AARCH64)
# define ARCH_NAME "aarch64"
#elif defined(CONFIG_ARM32)
# define ARCH_NAME "arm32"
#elif defined(CONFIG_ARMV7M)
# define ARCH_NAME "armv7m"
#elif defined(CONFIG_PPC64LE)
# define ARCH_NAME "ppc64le"
#elif defined(CONFIG_X86_64)
# define ARCH_NAME "x86_64"
#endif

/* Optional arch hook used by the ELF loader for dynamic-linker fallbacks. */
int arch_resolve_interp_fallback(const char *exec_path, const char *interp_path,
                                 char *resolved, size_t resolved_size);

#endif /* _ARCH_H */
