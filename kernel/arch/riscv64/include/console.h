#ifndef _ARCH_RISCV64_CONSOLE_H
#define _ARCH_RISCV64_CONSOLE_H

#include "core/types.h"
#include "platform.h"

#ifdef CONFIG_BOARD_OX64
#include "ox64_console.h"
#else

/* SG2002 uses a DW APB UART: reg-shift=2 and reg-io-width=4.
 * QEMU's byte-stride NS16550 must keep its original access width. */
#ifdef CONFIG_BOARD_LICHEERV_NANO
typedef volatile uint32_t arch_uart_regs_t;
#else
typedef volatile uint8_t arch_uart_regs_t;
#endif

static inline void arch_uart_init(void) {
#if defined(CONFIG_BOARD_VISIONFIVE2) || defined(CONFIG_BOARD_LICHEERV_NANO)
    /*
     * U-Boot/OpenSBI already owns the board UART setup (115200 8N1); do not
     * reprogram the divisor.  Output/input below still drive the 16550
     * registers directly: the SBI legacy console drops characters during
     * burst output (garbled boot logs, lost shell prompt/echo on the board).
     */
    return;
#else
    arch_uart_regs_t *uart = (arch_uart_regs_t *)UART0_BASE;
    uart[1] = 0x00;
    uart[3] = 0x80;
    uart[0] = 0x03;
    uart[1] = 0x00;
    uart[3] = 0x03;
    uart[2] = 0x07;
    uart[4] = 0x0B;
    uart[1] = 0x01;
#endif
}

static inline void arch_uart_putc(char c) {
    arch_uart_regs_t *uart = (arch_uart_regs_t *)UART0_BASE;
    while ((uart[5] & 0x20) == 0)
        ;
    uart[0] = (uint8_t)c;
}

static inline int arch_uart_poll_getc(void) {
    arch_uart_regs_t *uart = (arch_uart_regs_t *)UART0_BASE;
    if (uart[5] & 0x01)
        return uart[0];
    return -1;
}

static inline void arch_uart_flush(void) {
    arch_uart_regs_t *uart = (arch_uart_regs_t *)UART0_BASE;
    while ((uart[5] & 0x40) == 0)
        ;
}

static inline void arch_uart_ack_irq(void) {}

#endif /* !CONFIG_BOARD_OX64 */
#endif /* _ARCH_RISCV64_CONSOLE_H */
