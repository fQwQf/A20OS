#ifndef _LS2K1000_PLATFORM_H
#define _LS2K1000_PLATFORM_H

/*
 * U-Boot exposes DDR through the standard LoongArch cached DMW at VSEG 9.
 * Keep MMIO on the uncached VSEG 8 alias and kernel RAM on the cached alias.
 */
#define PAGE_OFFSET        0x9000000000000000UL
#define LS2K_UNCACHED_BASE 0x8000000000000000UL

/*
 * The first hardware bring-up intentionally uses only the low safe bank.
 * Linux reports 0x00200000..0x0affffff as RAM; U-Boot reserves memory from
 * 0x0cbf4c30 upward and the framebuffer starts at physical 0x0b000000.
 */
#define PHYS_MEMORY_BASE   0x00200000UL
#define PHYS_MEMORY_END    0x0B000000UL
#define KERNEL_ENTRY       0x02000000UL
#define USER_VA_LIMIT      0x4000000000UL

#define UART0_BASE         (LS2K_UNCACHED_BASE + 0x1FE20000UL)
#define UART0_IRQ          0U

#define LS2K_LIOINTC_BASE       (LS2K_UNCACHED_BASE + 0x1FE01400UL)
#define LS2K_LIOINTC_CORE0_ISR  (LS2K_UNCACHED_BASE + 0x1FE01040UL)
#define LS2K_LIOINTC_PARENT_IRQ 3U

void ls2k1000_handle_device_irq(void);
uint64_t ls2k1000_irq_cascade_count(void);
uint64_t ls2k1000_irq_source_count(uint32_t source);
uint64_t ls2k1000_irq_spurious_count(void);
uint64_t ls2k1000_irq_storm_count(void);

/* Device IRQ routing is not available yet, so uart_getc() must poll often
 * enough to drain the 16550-compatible receive FIFO at 115200 baud. */
#define UART_POLL_INTERVAL_TICKS 100000UL /* 1 ms at the 100 MHz counter */

#endif
