#ifndef _RASPBERRYPI4_PLATFORM_H
#define _RASPBERRYPI4_PLATFORM_H
#define PHYS_MEMORY_BASE 0UL
/* Initial direct map covers the low 1 GiB. The allocator uses a smaller
 * firmware-described, reservation-free prefix; this is only the aperture. */
#define PHYS_MEMORY_END 0x40000000UL
#define KERNEL_ENTRY 0x80000UL
#define UART0_BASE (0xfe201000UL + PAGE_OFFSET)
#define UART0_IRQ 153U
#define GICD_BASE (0xff841000UL + PAGE_OFFSET)
#define GICC_BASE (0xff842000UL + PAGE_OFFSET)
#define PL011_BAUD_IBRD 26
#define PL011_BAUD_FBRD 3
uint64_t aarch64_timer_freq(void);
#define ARCH_TIMER_FREQ aarch64_timer_freq()
int rpi4_ram_range(size_t idx, paddr_t *base, paddr_t *end);
#define ARCH_BOARD_RAM_RANGE rpi4_ram_range
#endif
