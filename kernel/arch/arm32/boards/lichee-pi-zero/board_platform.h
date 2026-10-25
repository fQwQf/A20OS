#ifndef _LICHEE_PI_ZERO_PLATFORM_H
#define _LICHEE_PI_ZERO_PLATFORM_H
#define PHYS_MEMORY_BASE 0x40000000UL
#define PHYS_MEMORY_END 0x44000000UL
#define KERNEL_ENTRY 0x40080000UL
#define UART0_BASE (0x01c28000UL + PAGE_OFFSET)
#define UART0_IRQ 32U
#define GICD_BASE (0x01c81000UL + PAGE_OFFSET)
#define GICC_BASE (0x01c82000UL + PAGE_OFFSET)
#define ARCH_TIMER_FREQ 24000000UL
int v3s_ram_range(size_t idx, paddr_t *base, paddr_t *end);
size_t v3s_ram_range_count(void);
#define ARCH_BOARD_RAM_RANGE_COUNT v3s_ram_range_count
#define ARCH_BOARD_RAM_RANGE v3s_ram_range
#endif
