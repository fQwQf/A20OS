#ifndef _ARCH_ARMV7M_PLATFORM_H
#define _ARCH_ARMV7M_PLATFORM_H

#include "core/types.h"
#include "board_config.h"

#define FLASH_MEMORY_BASE  ARMV7M_FLASH_BASE
#define PHYS_MEMORY_BASE   ARMV7M_RAM_BASE
#define PHYS_MEMORY_END    (PHYS_MEMORY_BASE + (STM32_RAM_KB * 1024UL))
#define KERNEL_ENTRY       FLASH_MEMORY_BASE
#define PAGE_OFFSET        0UL
#define USER_VA_LIMIT      PHYS_MEMORY_END

#define UART0_BASE         ARMV7M_CONSOLE_UART_BASE
#define UART0_IRQ          ARMV7M_CONSOLE_UART_IRQ
#define IRQ_S_TIMER        15U
#define IRQ_S_EXT          UART0_IRQ
#define IRQ_S_SOFT         14U
#define ARCH_TIMER_FREQ    1000UL

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

#define CAUSE_INTR_MASK         (1UL << 31)
#define CAUSE_CODE_MASK         ((1UL << 31) - 1)

#define SIE_SSIE                0UL
#define SIE_STIE                0UL
#define SIE_SEIE                0UL
#define SSTATUS_SIE             0UL
#define SSTATUS_SPIE            0UL
#define SSTATUS_SPP             0UL
#define SSTATUS_FS_OFF          0UL
#define SSTATUS_FS_INITIAL      0UL
#define SSTATUS_FS_CLEAN        0UL
#define SSTATUS_FS_DIRTY        0UL
#define SSTATUS_FS_MASK         0UL

static inline size_t arch_ram_range_count(void) { return 1; }

static inline int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx != 0 || !base || !end)
        return -1;
    *base = PHYS_MEMORY_BASE;
    *end = PHYS_MEMORY_END;
    return 0;
}

static inline void arch_unmap_boot_identity(void) {}

#endif
