#ifndef _ARCH_RISCV32_PLATFORM_H
#define _ARCH_RISCV32_PLATFORM_H

#include "core/types.h"

#define PHYS_MEMORY_BASE 0x80000000UL
#define PHYS_MEMORY_END 0xC0000000UL
#define KERNEL_ENTRY 0x80200000UL
#ifdef CONFIG_NOMMU
#define PAGE_OFFSET 0x0UL
#define USER_VA_LIMIT PHYS_MEMORY_END
#else
#define PAGE_OFFSET 0x00000000UL
#define USER_VA_LIMIT 0x80000000UL
#endif

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

#define UART0_PHYS_BASE 0x10000000UL
#define ACLINT_PHYS_BASE 0x02000000UL
#define PLIC_PHYS_BASE 0x0C000000UL
#define VIRTIO_PHYS_BASE 0x10001000UL

#ifdef CONFIG_NOMMU
#define UART0_BASE UART0_PHYS_BASE
#define ACLINT_BASE ACLINT_PHYS_BASE
#define VIRTIO_BASE VIRTIO_PHYS_BASE
#define PLIC_BASE PLIC_PHYS_BASE
#else
/*
 * New page tables only inherit boot_pgdir's upper half, so keep supervisor
 * MMIO windows in that shared half instead of the user half used by identity
 * mappings during very early boot.
 */
#define ACLINT_BASE 0xC2000000UL
#define PLIC_BASE 0xCC000000UL
#define UART0_BASE 0xD0000000UL
#define VIRTIO_BASE (UART0_BASE + 0x1000UL)
#endif

#define ACLINT_MTIME (ACLINT_BASE + 0xBFF8UL)
#define ACLINT_MTIMECMP(h) (ACLINT_BASE + 0x4000UL + (uint32_t)(h) * 8UL)
#define UART0_IRQ 10

#define PLIC_PRIORITY (PLIC_BASE + 0x0000UL)
#define PLIC_PENDING (PLIC_BASE + 0x1000UL)
#define PLIC_SENABLE(h) (PLIC_BASE + 0x2080UL + (uint32_t)(h) * 0x100UL)
#define PLIC_SPRIORITY(h) (PLIC_BASE + 0x201000UL + (uint32_t)(h) * 0x2000UL)
#define PLIC_SCLAIM(h) (PLIC_BASE + 0x201004UL + (uint32_t)(h) * 0x2000UL)

#define ARCH_TIMER_FREQ 10000000UL

#define IRQ_S_SOFT 1UL
#define IRQ_S_TIMER 5UL
#define IRQ_S_EXT 9UL
#define CAUSE_ECALL_U 8UL
#define CAUSE_INSN_MISALIGNED 0UL
#define CAUSE_INSN_FAULT 1UL
#define CAUSE_ILLEGAL_INSN 2UL
#define CAUSE_BREAKPOINT 3UL
#define CAUSE_LOAD_MISALIGNED 4UL
#define CAUSE_LOAD_FAULT 5UL
#define CAUSE_STORE_MISALIGNED 6UL
#define CAUSE_STORE_FAULT 7UL
#define CAUSE_INSN_PAGE_FAULT 12UL
#define CAUSE_LOAD_PAGE_FAULT 13UL
#define CAUSE_STORE_PAGE_FAULT 15UL
#define CAUSE_PAGE_MODIFICATION 0xFFUL

#define CAUSE_INTR_MASK (1UL << 31)
#define CAUSE_CODE_MASK ((1UL << 31) - 1UL)

#define SIE_SSIE (1UL << 1)
#define SIE_STIE (1UL << 5)
#define SIE_SEIE (1UL << 9)

#define SSTATUS_SIE (1UL << 1)
#define SSTATUS_SPIE (1UL << 5)
#define SSTATUS_SPP (1UL << 8)
#define SSTATUS_FS_OFF (0UL << 13)
#define SSTATUS_FS_INITIAL (1UL << 13)
#define SSTATUS_FS_CLEAN (2UL << 13)
#define SSTATUS_FS_DIRTY (3UL << 13)
#define SSTATUS_FS_MASK (3UL << 13)

extern uint32_t boot_pgdir[1024];

static inline void arch_unmap_boot_identity(void) {
}

#endif
