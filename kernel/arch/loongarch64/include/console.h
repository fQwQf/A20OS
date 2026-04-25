#ifndef _ARCH_LOONGARCH64_CONSOLE_H
#define _ARCH_LOONGARCH64_CONSOLE_H

#include "core/types.h"
#include "platform.h"

static inline void arch_uart_init(void) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    uart[1] = 0x00;
    uart[3] = 0x80;
    uart[0] = 0x03;
    uart[1] = 0x00;
    uart[3] = 0x03;
    uart[2] = 0x07;
    uart[4] = 0x0B;
    uart[1] = 0x01;
}

static inline void arch_uart_putc(char c) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
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
