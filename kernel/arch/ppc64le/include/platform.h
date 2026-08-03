#ifndef _ARCH_PPC64LE_PLATFORM_H
#define _ARCH_PPC64LE_PLATFORM_H

#include "core/types.h"

#define PHYS_MEMORY_BASE   0x00000000UL
#define PHYS_MEMORY_END    0x40000000UL
#define KERNEL_ENTRY       0x02000000UL
/*
 * PAGE_OFFSET must give the kernel linear map a Radix root index of 256
 * (EA bits 51..39 == 0x100).  The shared page-table code keeps user pages in
 * root entries 0..255 and copies root entries 256..511 (pt_map_kernel) into
 * every new address space for the kernel.  With the old 0xC000000000000000
 * base the hardware would walk kernel VAs through root entry 0, which is the
 * user root, so a task's kernel mappings would never be found.
 */
#define PAGE_OFFSET        0xC000800000000000UL
#define USER_VA_LIMIT      0x0000800000000000UL

static inline size_t arch_ram_range_count(void) {
    return 1;
}

static inline int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx != 0 || !base || !end)
        return -1;
    /*
     * Book3S exception vectors occupy real addresses 0x100..0xcff and the
     * following page is reserved for real-mode trap scratch storage.
     */
    *base = 0x40000UL;
    *end = PHYS_MEMORY_END;
    return 0;
}

#define UART0_IRQ          0U
#define VIRTIO_BASE        (0x00000000UL + PAGE_OFFSET)
#define PCIE_ECAM_BASE     (0x00000000UL + PAGE_OFFSET)
#define PCIE_BUS_START     0
#define PCIE_BUS_END       0

#define ARCH_TIMER_FREQ    512000000UL

#define IRQ_S_TIMER        0x900UL
#define IRQ_S_EXT          0x500UL
#define IRQ_S_SOFT         0x400UL

#define CAUSE_ECALL_U           0xC00UL
#define CAUSE_INSN_PAGE_FAULT   0x301UL
#define CAUSE_LOAD_PAGE_FAULT   0x300UL
#define CAUSE_DATA_SEGMENT      0x380UL
#define CAUSE_STORE_PAGE_FAULT  0x380UL
#define CAUSE_PAGE_MODIFICATION 0x381UL
#define CAUSE_INSN_FAULT        0x700UL
#define CAUSE_LOAD_FAULT        0x701UL
#define CAUSE_STORE_FAULT       0x702UL
#define CAUSE_BREAKPOINT        0x700UL
#define CAUSE_ILLEGAL_INSN      0x7000UL
#define CAUSE_INSN_MISALIGNED   CAUSE_INSN_FAULT
#define CAUSE_LOAD_MISALIGNED   CAUSE_LOAD_FAULT
#define CAUSE_STORE_MISALIGNED  CAUSE_STORE_FAULT

#define CAUSE_INTR_MASK         (1UL << 63)
#define CAUSE_CODE_MASK         ((1UL << 63) - 1)

#define SIE_SSIE                0UL
#define SIE_STIE                0UL
#define SIE_SEIE                0UL

#define PPC64_MSR_EE            (1UL << 15)

#define SSTATUS_SIE             PPC64_MSR_EE
#define SSTATUS_SPIE            0UL
#define SSTATUS_SPP             0UL
#define SSTATUS_FS_OFF          0UL
#define SSTATUS_FS_INITIAL      0UL
#define SSTATUS_FS_CLEAN        0UL
#define SSTATUS_FS_DIRTY        0UL
#define SSTATUS_FS_MASK         0UL

extern uint64_t boot_pgdir[];

static inline void arch_unmap_boot_identity(void) { }

#endif
