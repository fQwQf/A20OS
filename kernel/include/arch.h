/*
 * A20OS Architecture Abstraction Layer
 *
 * This header dispatches to the correct arch-specific headers based on
 * the CONFIG_* define set by the Makefile (-DCONFIG_RISCV64 or
 * -DCONFIG_LOONGARCH64).
 *
 * All arch-specific code (inline asm, register access, page table format,
 * trap context layout, hardware addresses) lives under kernel/arch/$(ARCH)/.
 * Shared kernel code includes only this header (or the individual
 * sub-headers it pulls in) and NEVER touches arch-specific registers or
 * instructions directly.
 */
#ifndef _ARCH_H
#define _ARCH_H

/*
 * Sub-headers provided by each architecture:
 *   arch.h       — master include (pulls in everything below)
 *   cpu.h        — barriers, irq control, wfi, TLB flush, CSR/register access
 *   mm.h         — page table format (PTE flags, VPN/PPN macros, SATP/TTBR)
 *   trap.h       — trap_context_t, task_context_t, syscall register access
 *   platform.h   — HW base addresses, IRQ numbers, exception codes, PAGE_OFFSET
 *   firmware.h   — shutdown/reboot/console/timer firmware calls (SBI or equiv)
 */

#if defined(CONFIG_RISCV64)
# include "arch/riscv64/arch.h"
#elif defined(CONFIG_LOONGARCH64)
# include "arch/loongarch64/arch.h"
#else
# error "No architecture defined. Set ARCH=riscv64 or ARCH=loongarch64."
#endif

/* Arch name string (for uname, procfs, etc.) */
#if defined(CONFIG_RISCV64)
# define ARCH_NAME "riscv64"
#elif defined(CONFIG_LOONGARCH64)
# define ARCH_NAME "loongarch64"
#endif

#endif /* _ARCH_H */
