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
#define VIRT_GED_EVT_ADDR  0x100E0000UL
#define VIRT_GED_REG_ADDR  0x100E001CUL
#define VIRT_GED_SLEEP_CTL (VIRT_GED_REG_ADDR + 0x0UL)
#define VIRT_GED_RESET_REG (VIRT_GED_REG_ADDR + 0x2UL)
#define VIRT_GED_RESET_VAL 0x42
#define VIRT_GED_SLP_TYP_S5 0x05
#define VIRT_GED_SLP_TYP_POS 0x2
#define VIRT_GED_SLP_EN    0x20

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
#define CAUSE_ECALL_U           0x0B /* SYS (11) — syscall instruction */

#define CAUSE_INSN_PAGE_FAULT   0x03 /* PIF (3)  — page invalid for fetch */
#define CAUSE_LOAD_PAGE_FAULT   0x01 /* PIL (1)  — page invalid for load */
#define CAUSE_STORE_PAGE_FAULT  0x02 /* PIS (2)  — page invalid for store */
#define CAUSE_PAGE_MODIFICATION 0x04 /* PME (4)  — Page Modification Exception (用于 COW) */

#define CAUSE_INSN_FAULT        0x08 /* ADEF (8) — address error for fetch */
#define CAUSE_LOAD_FAULT        0x09 /* ALE (9)  — address error for load/store */
#define CAUSE_STORE_FAULT       0x09 /* ALE (9)  — address error for load/store */

#define CAUSE_BREAKPOINT        0x0C /* BRK (12) — break instruction */
#define CAUSE_ILLEGAL_INSN      0x0D /* INE (13) — instruction not exist */

#define CAUSE_INSN_MISALIGNED   0xFF /* not defined directly as single ecode on LA64, merged in ALE/ADEF */
#define CAUSE_LOAD_MISALIGNED   0xFF /* not defined directly on LA64, merged in ALE */
#define CAUSE_STORE_MISALIGNED  0xFF /* not defined directly on LA64, merged in ALE */

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
