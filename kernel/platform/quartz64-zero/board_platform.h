#ifndef _QUARTZ64_ZERO_PLATFORM_H
#define _QUARTZ64_ZERO_PLATFORM_H
/* DDR starts at zero; keep the firmware/secure prefix out of the allocator. */
#define PHYS_MEMORY_BASE 0x00200000UL
#define PHYS_MEMORY_END 0x40000000UL
#define KERNEL_ENTRY 0x02080000UL
#define UART0_BASE (0xfe660000UL + PAGE_OFFSET)
#define UART0_IRQ 150U
#define CONFIG_AARCH64_GICV3 1
#define GICD_BASE (0xfd400000UL + PAGE_OFFSET)
uint64_t quartz64_zero_gicr_base(void);
#define GICR_BASE quartz64_zero_gicr_base()
#define GICC_BASE 0UL /* GICv3 uses ICC_* system registers. */
#define AARCH64_PSCI_USE_SMC 1
uint64_t aarch64_timer_freq(void);
#define ARCH_TIMER_FREQ aarch64_timer_freq()
int quartz64_zero_ram_range(size_t idx, paddr_t *base, paddr_t *end);
#define ARCH_BOARD_RAM_RANGE quartz64_zero_ram_range
#endif
