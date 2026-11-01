#ifndef A20_RISCV_MCU_PLATFORM_H
#define A20_RISCV_MCU_PLATFORM_H
#include "core/types.h"
#include "board_config.h"
#define PHYS_MEMORY_BASE MCU_RAM_BASE
#define PHYS_MEMORY_END MCU_RAM_END
#define KERNEL_ENTRY MCU_CODE_BASE
#define PAGE_OFFSET 0UL
#define USER_VA_LIMIT PHYS_MEMORY_END
#ifndef ARCH_TIMER_FREQ
#define ARCH_TIMER_FREQ 1000UL
#endif
#define CAUSE_INTR_MASK (1UL << 31)
#define CAUSE_CODE_MASK (CAUSE_INTR_MASK - 1UL)
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
#define CAUSE_PAGE_MODIFICATION 255UL
/* Generic task status accessors use these names; values are M-mode bits. */
#define SSTATUS_SIE (1UL << 3)
#define SSTATUS_SPIE (1UL << 7)
#define SSTATUS_SPP (3UL << 11)
#define SSTATUS_FS_OFF 0UL
#define SSTATUS_FS_INITIAL 0UL
#define SSTATUS_FS_CLEAN 0UL
#define SSTATUS_FS_DIRTY 0UL
#define SSTATUS_FS_MASK 0UL
static inline size_t arch_ram_range_count(void) { return 1; }
static inline int arch_ram_range(size_t i, paddr_t *b, paddr_t *e) {
    if (i || !b || !e) return -1;
    *b = PHYS_MEMORY_BASE; *e = PHYS_MEMORY_END; return 0;
}
static inline void arch_unmap_boot_identity(void) {}
#endif
