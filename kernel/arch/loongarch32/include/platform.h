#ifndef _ARCH_LOONGARCH32_PLATFORM_H
#define _ARCH_LOONGARCH32_PLATFORM_H

#include "core/types.h"

/*
 * LoongArch32 (LA32R) memory layout for the NaiLoong Core (LA32R
 * SoC).  DRAM is a 512 MiB window at 0x80000000; MMIO (UART ...) lives in
 * the 0x1C000000-0x1FFFFFFF window.  Both windows are identity mapped
 * (VA == PA) through the two 512 MiB DMW windows (DMW0 / DMW1).
 */
#define PHYS_MEMORY_BASE   0x80000000UL
#define PHYS_MEMORY_END    0xA0000000UL
#define KERNEL_ENTRY       0x80000000UL
#define PAGE_OFFSET        0x00000000UL
#define USER_VA_LIMIT      0x80000000UL

size_t arch_ram_range_count(void);
int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end);
void loongarch32_memory_init(void);

#define UART0_BASE         0x1FE001E0UL
#define UART0_IRQ          2

/* The NaiLoong SoC has no PCI / virtio / GED / PLIC; keep the addresses out
 * of this port.  ARCH_TIMER_FREQ must match the stable counter that
 * rdcntvl.w/rdcntvh.w expose (the CPU clock on the target board). */
#define ARCH_TIMER_FREQ    100000000UL

/* LoongArch ESTAT IS bit positions (identical to LA64) */
#define IRQ_S_SOFT         0   /* SWI0 */
#define IRQ_S_TIMER        11  /* TI — timer interrupt */
#define IRQ_S_IPI          12  /* not wired on NaiLoong (single core) */
#define IRQ_S_EXT          2   /* HWI0 */

/*
 * LoongArch Ecode values (ESTAT[21:16]).
 */
#define CAUSE_ECALL_U           0x0B /* SYS (11) — syscall instruction */

#define CAUSE_INSN_PAGE_FAULT   0x03 /* PIF (3)  — page invalid for fetch */
#define CAUSE_LOAD_PAGE_FAULT   0x01 /* PIL (1)  — page invalid for load */
#define CAUSE_STORE_PAGE_FAULT  0x02 /* PIS (2)  — page invalid for store */
#define CAUSE_PAGE_MODIFICATION 0x04 /* PME (4)  — page modification (COW) */
#define CAUSE_PAGE_NOT_READABLE 0x05 /* PNR (5)  */
#define CAUSE_PAGE_NOT_EXEC     0x06 /* PNX (6)  */
#define CAUSE_PAGE_PRIVILEGE    0x07 /* PPI (7)  — page privilege denied */

#define ARCH_IS_USER_PAGE_PERMISSION_FAULT(code) \
    ((code) == CAUSE_PAGE_NOT_READABLE || \
     (code) == CAUSE_PAGE_NOT_EXEC || \
     (code) == CAUSE_PAGE_PRIVILEGE)

#define CAUSE_INSN_FAULT        0x08 /* ADEF (8)  — address error for fetch */
#define CAUSE_LOAD_FAULT        0x09 /* ALE (9)  — address error for load/store */
#define CAUSE_STORE_FAULT       0x09 /* ALE (9)  */

#define CAUSE_BREAKPOINT        0x0C /* BRK (12) — break instruction */
#define CAUSE_ILLEGAL_INSN      0x0D /* INE (13) — instruction not exist */

#define CAUSE_INSN_MISALIGNED   0xFF
#define CAUSE_LOAD_MISALIGNED   0xFF
#define CAUSE_STORE_MISALIGNED  0xFF

/* NaiLoong keeps the synthetic cause value 32-bit wide: interrupt flag is
 * bit 31, exception code occupies the low bits. */
#define CAUSE_INTR_MASK         (1UL << 31)
#define CAUSE_CODE_MASK         ((1UL << 31) - 1)

#define SIE_SSIE       (1UL << 0)
#define SIE_STIE       (1UL << 1)
#define SIE_SEIE       (1UL << 2)

/*
 * PRMD (Previous CRMD) for LoongArch32:
 *   [1:0] PPLV, [2] PIE
 * SSTATUS_SPIE | SSTATUS_FS_CLEAN → PPLV=3 + PIE=1 = 0x7 for user ertn.
 */
#define SSTATUS_SIE       (1UL << 2)
#define SSTATUS_SPIE      (1UL << 2)
#define SSTATUS_SPP       (0UL)
#define SSTATUS_FS_OFF    (0UL << 0)
#define SSTATUS_FS_INITIAL (1UL << 0)
#define SSTATUS_FS_CLEAN   (3UL << 0)
#define SSTATUS_FS_DIRTY   (3UL << 0)
#define SSTATUS_FS_MASK    (3UL << 0)

extern uint32_t boot_pgdir[1024];

static inline void arch_unmap_boot_identity(void) { }

#endif
