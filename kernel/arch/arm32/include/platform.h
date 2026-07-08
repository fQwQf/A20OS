#ifndef _ARCH_ARM32_PLATFORM_H
#define _ARCH_ARM32_PLATFORM_H

#include "core/types.h"

#define PHYS_MEMORY_BASE   0x40000000UL
#define PHYS_MEMORY_END    0x80000000UL
#define KERNEL_ENTRY       0x40080000UL
#ifdef CONFIG_NOMMU
#define PAGE_OFFSET        0x0UL
#define USER_VA_LIMIT      PHYS_MEMORY_END
#else
#define PAGE_OFFSET        0x80000000UL
#define USER_VA_LIMIT      0x70000000UL
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

#define UART0_BASE         (0x09000000UL + PAGE_OFFSET)
#define GICD_BASE          (0x08000000UL + PAGE_OFFSET)
#define GICC_BASE          (0x08010000UL + PAGE_OFFSET)
#define VIRTIO_BASE        (0x0A000000UL + PAGE_OFFSET)
#define PCIE_ECAM_BASE     (0x3F000000UL + PAGE_OFFSET)
#define PCIE_BUS_START     0
#define PCIE_BUS_END       0
#define UART0_IRQ          33U
#define IRQ_S_TIMER        30U
#define IRQ_S_EXT          UART0_IRQ
#define IRQ_S_SOFT         1U
#define ARCH_TIMER_FREQ    62500000UL

#define CAUSE_ECALL_U           11U
#define CAUSE_INSN_PAGE_FAULT   12U
#define CAUSE_LOAD_PAGE_FAULT   13U
#define CAUSE_STORE_PAGE_FAULT  14U
#define CAUSE_PAGE_MODIFICATION CAUSE_STORE_PAGE_FAULT
#define CAUSE_INSN_FAULT        1U
#define CAUSE_LOAD_FAULT        2U
#define CAUSE_STORE_FAULT       3U
#define CAUSE_BREAKPOINT        4U
#define CAUSE_ILLEGAL_INSN      5U
#define CAUSE_INSN_MISALIGNED   CAUSE_INSN_FAULT
#define CAUSE_LOAD_MISALIGNED   CAUSE_LOAD_FAULT
#define CAUSE_STORE_MISALIGNED  CAUSE_STORE_FAULT

#define CAUSE_INTR_MASK         (1UL << 31)
#define CAUSE_CODE_MASK         ((1UL << 31) - 1)

#define SIE_SSIE                0UL
#define SIE_STIE                0UL
#define SIE_SEIE                0UL

#define SSTATUS_SIE             (1UL << 7)
#define SSTATUS_SPIE            (1UL << 5)
#define SSTATUS_SPP             (1UL << 0)
#define SSTATUS_FS_OFF          0UL
#define SSTATUS_FS_INITIAL      0UL
#define SSTATUS_FS_CLEAN        0UL
#define SSTATUS_FS_DIRTY        0UL
#define SSTATUS_FS_MASK         0UL

extern uint32_t boot_pgdir[4096];

static inline void arch_unmap_boot_identity(void) { }

#endif
