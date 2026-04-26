/*
 * A20OS Architecture Abstraction Layer
 *
 * This header dispatches to the correct arch-specific headers based on
 * the CONFIG_* define set by the Makefile (-DCONFIG_RISCV64,
 * -DCONFIG_LOONGARCH64 or -DCONFIG_AARCH64).
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
#elif defined(CONFIG_LOONGARCH64)
# include "arch/loongarch64/include/arch.h"
#elif defined(CONFIG_AARCH64)
# include "arch/aarch64/include/arch.h"
#else
# error "No architecture defined. Set ARCH=riscv64, ARCH=loongarch64 or ARCH=aarch64."
#endif

/* Arch name string (for uname, procfs, etc.) */
#if defined(CONFIG_RISCV64)
# define ARCH_NAME "riscv64"
#elif defined(CONFIG_LOONGARCH64)
# define ARCH_NAME "loongarch64"
#elif defined(CONFIG_AARCH64)
# define ARCH_NAME "aarch64"
#endif

/* Optional arch hook used by the ELF loader for dynamic-linker fallbacks. */
int arch_resolve_interp_fallback(const char *exec_path, const char *interp_path,
                                 char *resolved, size_t resolved_size);

#endif /* _ARCH_H */
