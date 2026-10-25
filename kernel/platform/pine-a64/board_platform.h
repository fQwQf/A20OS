#ifndef _PINE_A64_PLATFORM_H
#define _PINE_A64_PLATFORM_H
/* Linux v6.17 sun50i-a64.dtsi; PINE A64 512 MiB model only.
 * The allocator is narrowed further using firmware RAM/reservations. */
#define PHYS_MEMORY_BASE 0x40000000UL
#define PHYS_MEMORY_END 0x60000000UL
#define KERNEL_ENTRY 0x40080000UL
#define UART0_BASE (0x01c28000UL + PAGE_OFFSET)
#define UART0_IRQ 32U
#define GICD_BASE (0x01c81000UL + PAGE_OFFSET)
#define GICC_BASE (0x01c82000UL + PAGE_OFFSET)
#define AARCH64_PSCI_USE_SMC 1
#define AARCH64_TIMER_ERRATUM_SUN50I_A64 1
uint64_t aarch64_timer_freq(void);
#define ARCH_TIMER_FREQ aarch64_timer_freq()
int pine_a64_ram_range(size_t idx, paddr_t *base, paddr_t *end);
#define ARCH_BOARD_RAM_RANGE pine_a64_ram_range
#endif
