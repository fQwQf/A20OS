#ifndef _ARCH_LOONGARCH64_PLATFORM_H
#define _ARCH_LOONGARCH64_PLATFORM_H

#include "types.h"

/* Identity-mapped: VA == PA. DMW0/DMW1 both map segment 0 → segment 0. */

#define PHYS_MEMORY_BASE   0x80000000UL
#define PHYS_MEMORY_END    0x90000000UL
#define KERNEL_ENTRY       0x80000000UL
#define PAGE_OFFSET        0x00000000UL

#define UART0_BASE         0x1FE001E0UL
#define VIRTIO_BASE        0x10001000UL
#define PLIC_BASE          0x10000000UL

#define PCIE_ECAM_BASE     0x20000000UL
#define PCIE_ECAM_SIZE     0x8000000UL
#define PCIE_BUS_START     0
#define PCIE_BUS_END       127
#define PCIE_MMIO_BASE     0x40000000UL
#define PCIE_MMIO_SIZE     0x40000000UL
#define UART0_IRQ          2

#define CLINT_BASE         0x00000000UL
#define CLINT_TIMER_FREQ   10000000UL

/* LoongArch ESTAT IS bit positions */
#define IRQ_S_SOFT         0   /* SWI0 */
#define IRQ_S_TIMER        11  /* TI — timer interrupt */
#define IRQ_S_EXT          2   /* HWI0 */

/*
 * LoongArch Ecode values (ESTAT[21:16]).
 * These are DIFFERENT from RISC-V — each arch defines its own in platform.h.
 *
 * ESTAT layout:
 *   [15:0]   IS  — Interrupt Status (one bit per IRQ line)
 *   [21:16]  Ecode — Exception code
 *
 * arch_read_cause() returns a synthetic value:
 *   Interrupts: (1UL << 63) | irq_number
 *   Exceptions: Ecode value
 */
#define CAUSE_ECALL_U      0x07  /* SYS — syscall instruction */

#define CAUSE_INSN_MISALIGNED   0xFF /* not defined on LA64 */
#define CAUSE_INSN_FAULT        0x04 /* ADEF — address error for fetch */
#define CAUSE_ILLEGAL_INSN      0x09 /* INE — instruction not exist */
#define CAUSE_BREAKPOINT        0x08 /* BRK — break instruction */
#define CAUSE_LOAD_MISALIGNED   0xFF /* not defined on LA64 */
#define CAUSE_LOAD_FAULT        0x05 /* ALE — address error for load */
#define CAUSE_STORE_MISALIGNED  0xFF /* not defined on LA64 */
#define CAUSE_STORE_FAULT       0x06 /* ASE — address error for store */
#define CAUSE_INSN_PAGE_FAULT   0x03 /* PIF — page invalid for fetch */
#define CAUSE_LOAD_PAGE_FAULT   0x01 /* PIL — page invalid for load */
#define CAUSE_STORE_PAGE_FAULT  0x02 /* PIS — page invalid for store */

#define CAUSE_INTR_MASK         (1UL << 63)
#define CAUSE_CODE_MASK         ((1UL << 63) - 1)

#define SIE_SSIE       (1UL << 0)
#define SIE_STIE       (1UL << 1)
#define SIE_SEIE       (1UL << 2)

/*
 * PRMD (Previous CRMD) register mapping for LoongArch.
 * Generic code uses SSTATUS_* names via TRAP_CTX_STATUS / TASK_CTX_STATUS macros.
 *
 * PRMD bit layout:
 *   [1:0] = PPLV (Previous Privilege Level — 3 for user mode)
 *   [2]   = PIE  (Previous Interrupt Enable)
 *
 * SSTATUS_SPIE | SSTATUS_FS_CLEAN must produce PPLV=3 + PIE=1 = 0x7
 * for correct user-mode entry via ertn.
 */
#define SSTATUS_SIE       (1UL << 2)
#define SSTATUS_SPIE      (1UL << 2)
#define SSTATUS_SPP       (0UL)
#define SSTATUS_FS_OFF    (0UL << 0)
#define SSTATUS_FS_INITIAL (1UL << 0)
#define SSTATUS_FS_CLEAN   (3UL << 0)
#define SSTATUS_FS_DIRTY   (3UL << 0)
#define SSTATUS_FS_MASK    (3UL << 0)

extern uint64_t boot_pgdir[512];

#endif
