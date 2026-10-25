#ifndef _LS2K0300_PLATFORM_H
#define _LS2K0300_PLATFORM_H

/* LS2K0300 User Manual v1.01, sections 3.2, 3.5 and 11.
 * These are NOT the LS2K1000 peripheral addresses. */
#define PAGE_OFFSET             0x9000000000000000UL
#define LS2K0300_UNCACHED_BASE   0x8000000000000000UL
#define UART0_BASE              (LS2K0300_UNCACHED_BASE + 0x16100000UL)
#define UART0_IRQ               0U
#define LS2K0300_LIOINTC_BASE    0x16001400UL
#define LS2K0300_LIOINTC_ISR0    0x16001040UL

/* Initial allocator window, deliberately smaller than the reference DTS low
 * bank [0x00200000, 0x0f000000). Confirm with the delivered board's firmware
 * before executing the RAM image. No DDR initialization is performed here. */
#define PHYS_MEMORY_BASE        0x00200000UL
#define PHYS_MEMORY_END         0x08000000UL
#define KERNEL_ENTRY            0x02000000UL
#define USER_VA_LIMIT           0x4000000000UL

/* CPUCFG.4/5 describe the stable counter; do not reuse QEMU's 100 MHz. */
uint64_t ls2k0300_timer_freq(void);
#define ARCH_TIMER_FREQ         ls2k0300_timer_freq()
#define UART_POLL_INTERVAL_TICKS (ARCH_TIMER_FREQ / 1000UL)

#endif
