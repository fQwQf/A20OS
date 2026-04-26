#ifndef _ARCH_RISCV64_PLATFORM_H
#define _ARCH_RISCV64_PLATFORM_H

#include "core/types.h"

/* Physical memory layout (QEMU virt) */
#define PHYS_MEMORY_BASE   0x80000000UL
#define PHYS_MEMORY_END    0xA0000000UL
#define KERNEL_ENTRY       0x80200000UL
#define PAGE_OFFSET        0xFFFFFFC000000000UL
#define USER_VA_LIMIT      0x4000000000UL

static inline size_t arch_ram_range_count(void) {
    return 1;
}

static inline int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx != 0 || !base || !end) return -1;
    *base = PHYS_MEMORY_BASE;
    *end = PHYS_MEMORY_END;
    return 0;
}

/* MMIO base addresses */
#define UART0_BASE         (0x10000000UL + PAGE_OFFSET)
#define CLINT_BASE         (0x02000000UL + PAGE_OFFSET)
#define VIRTIO_BASE        (0x10001000UL + PAGE_OFFSET)
#define PLIC_BASE          (0x0C000000UL + PAGE_OFFSET)
#define UART0_IRQ          10

/* PLIC register offsets */
#define PLIC_PRIORITY      (PLIC_BASE + 0x0000UL)
#define PLIC_PENDING       (PLIC_BASE + 0x1000UL)
#define PLIC_SENABLE(h)    (PLIC_BASE + 0x2080UL + (uint64_t)(h) * 0x100UL)
#define PLIC_SPRIORITY(h)  (PLIC_BASE + 0x201000UL + (uint64_t)(h) * 0x2000UL)
#define PLIC_SCLAIM(h)     (PLIC_BASE + 0x201004UL + (uint64_t)(h) * 0x2000UL)

/* CLINT timer */
#define CLINT_MTIME        (CLINT_BASE + 0xBFF8UL)
#define CLINT_MTIMECMP(h)  (CLINT_BASE + 0x4000UL + ((unsigned long)(h) * 8))
#define CLINT_TIMER_FREQ   10000000UL

/* Exception codes (scause) */
#define IRQ_S_SOFT         1
#define IRQ_S_TIMER        5
#define IRQ_S_EXT          9
#define CAUSE_ECALL_U      8
#define CAUSE_INSN_MISALIGNED   0
#define CAUSE_INSN_FAULT        1
#define CAUSE_ILLEGAL_INSN      2
#define CAUSE_BREAKPOINT        3
#define CAUSE_LOAD_MISALIGNED   4
#define CAUSE_LOAD_FAULT        5
#define CAUSE_STORE_MISALIGNED  6
#define CAUSE_STORE_FAULT       7
#define CAUSE_INSN_PAGE_FAULT   12
#define CAUSE_LOAD_PAGE_FAULT   13
#define CAUSE_STORE_PAGE_FAULT  15
#define CAUSE_PAGE_MODIFICATION 0xFF  /* Not a real RISC-V exception; placeholder to satisfy trap.c */

#define CAUSE_INTR_MASK         (1UL << 63)
#define CAUSE_CODE_MASK         ((1UL << 63) - 1)

/* SIE interrupt enable bits */
#define SIE_SSIE       (1L << 1)
#define SIE_STIE       (1L << 5)
#define SIE_SEIE       (1L << 9)

/* sstatus bits */
#define SSTATUS_SIE     (1UL << 1)
#define SSTATUS_SPIE    (1UL << 5)
#define SSTATUS_SPP     (1UL << 8)
#define SSTATUS_FS_OFF    (0UL << 13)
#define SSTATUS_FS_INITIAL (1UL << 13)
#define SSTATUS_FS_CLEAN   (2UL << 13)
#define SSTATUS_FS_DIRTY   (3UL << 13)
#define SSTATUS_FS_MASK    (3UL << 13)

extern uint64_t boot_pgdir[512];

#endif
