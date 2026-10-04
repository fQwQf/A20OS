#ifndef _ARCH_LOONGARCH64_CONSOLE_H
#define _ARCH_LOONGARCH64_CONSOLE_H

#include "core/types.h"
#include "platform.h"

static inline void arch_uart_init(void) {
#ifdef CONFIG_BOARD_LS2K1000
    /* Keep U-Boot's 115200 8N1 divisor.  The cooperative fallback polls;
     * the Phase 2 image enables RX only after LIOINTC was initialized. */
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
#ifdef CONFIG_COOPERATIVE_BOOT
    uart[1] = 0x00;
#else
    uart[1] = 0x01;
#endif
    return;
#else
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    uart[1] = 0x00;
    uart[3] = 0x80;
    uart[0] = 0x03;
    uart[1] = 0x00;
    uart[3] = 0x03;
    /* FCR bit 0 enables the 16-byte receive FIFO.  Without it the device keeps
     * only a one-byte holding register, so a burst that arrives while nobody is
     * spinning on LSR.DR overwrites itself and the earliest characters are
     * lost.  That is invisible on output and fatal for the polled receive path
     * (uart_cmdline_read): the operator's keystrokes are a burst.  Bit 1 clears
     * both FIFOs, bits 6-7 set a 14-byte receive trigger; the receiver interrupt
     * level stays masked so this board still has no IRQ route. */
    uart[2] = 0xC7;
    uart[4] = 0x0B;
    uart[1] = 0x01;
#endif
}

static inline void arch_uart_putc(char c) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    if (c == '\n') {
        while ((uart[5] & 0x20) == 0)
            ;
        uart[0] = '\r';
    }
    while ((uart[5] & 0x20) == 0)
        ;
    uart[0] = (uint8_t)c;
}

static inline int arch_uart_poll_getc(void) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    if (uart[5] & 0x01)
        return uart[0];
    return -1;
}

static inline void arch_uart_flush(void) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    while ((uart[5] & 0x40) == 0)
        ;
}

static inline void arch_uart_ack_irq(void) {}

#endif /* _ARCH_LOONGARCH64_CONSOLE_H */
