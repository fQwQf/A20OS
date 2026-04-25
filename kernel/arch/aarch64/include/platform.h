#ifndef _ARCH_AARCH64_PLATFORM_H
#define _ARCH_AARCH64_PLATFORM_H

#include "core/types.h"

/*
 * QEMU virt (AArch64)
 *
 * DRAM starts at PA 0x40000000.  The kernel is linked in the higher half at
 * VA = PAGE_OFFSET + PA, but all per-process page tables still share a single
 * root page table via TTBR0_EL1.  Root entry 0 covers user space; root entry 1
 * covers the linear-mapped kernel window [PAGE_OFFSET, PAGE_OFFSET + 512G).
 */
#define PHYS_MEMORY_BASE   0x40000000UL
#define PHYS_MEMORY_END    0x60000000UL
#define KERNEL_ENTRY       0x40080000UL
#define PAGE_OFFSET        0x0000008000000000UL
#define USER_VA_LIMIT      0x0000004000000000UL

static inline size_t arch_ram_range_count(void) {
    return 1;
}

static inline int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx != 0 || !base || !end)
        return -1;
    *base = PHYS_MEMORY_BASE;
    *end = PHYS_MEMORY_END;
    return 0;
}

#define UART0_BASE         (0x09000000UL + PAGE_OFFSET)
#define GICD_BASE          (0x08000000UL + PAGE_OFFSET)
#define GICC_BASE          (0x08010000UL + PAGE_OFFSET)
#define VIRTIO_BASE        (0x0A000000UL + PAGE_OFFSET)
#define UART0_IRQ          33U

#define CLINT_BASE         0x0UL
#define CLINT_TIMER_FREQ   62500000UL

/*
 * Synthetic trap/interrupt codes returned by arch_read_cause().
 * These need only be self-consistent with trap.c.
 */
#define IRQ_S_TIMER        30U
#define IRQ_S_EXT          UART0_IRQ
#define IRQ_S_SOFT         1U

#define CAUSE_ECALL_U           0x15U
#define CAUSE_INSN_PAGE_FAULT   0x20U
#define CAUSE_LOAD_PAGE_FAULT   0x24U
#define CAUSE_STORE_PAGE_FAULT  0x25U
#define CAUSE_PAGE_MODIFICATION 0xFFU
#define CAUSE_INSN_FAULT        0x22U
#define CAUSE_LOAD_FAULT        0x26U
#define CAUSE_STORE_FAULT       0x27U
#define CAUSE_BREAKPOINT        0x3CU
#define CAUSE_ILLEGAL_INSN      0x00U
#define CAUSE_INSN_MISALIGNED   CAUSE_INSN_FAULT
#define CAUSE_LOAD_MISALIGNED   CAUSE_LOAD_FAULT
#define CAUSE_STORE_MISALIGNED  CAUSE_STORE_FAULT

#define CAUSE_INTR_MASK         (1UL << 63)
#define CAUSE_CODE_MASK         ((1UL << 63) - 1)

#define SIE_SSIE       0UL
#define SIE_STIE       0UL
#define SIE_SEIE       0UL

/*
 * Generic code stores "status" in trap/task contexts.  On AArch64 the only
 * status value that matters for user return is SPSR_EL1, and we want EL0t with
 * IRQs unmasked.  That encoding is simply zero, so the generic SSTATUS_* flags
 * collapse to 0 here.
 */
#define SSTATUS_SIE        0UL
#define SSTATUS_SPIE       0UL
#define SSTATUS_SPP        0UL
#define SSTATUS_FS_OFF     0UL
#define SSTATUS_FS_INITIAL 0UL
#define SSTATUS_FS_CLEAN   0UL
#define SSTATUS_FS_DIRTY   0UL
#define SSTATUS_FS_MASK    0UL

extern uint64_t boot_pgdir[512];

#endif
