#ifndef _ARCH_X86_64_CONSOLE_H
#define _ARCH_X86_64_CONSOLE_H

#include "core/types.h"
#include "cpu.h"

static inline void arch_uart_init(void) {
    outb(0x3F8 + 1, 0x00);  /* Disable interrupts */
    outb(0x3F8 + 3, 0x80);  /* Enable DLAB */
    outb(0x3F8 + 0, 0x03);  /* Divisor = 3 (38400 baud) */
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);  /* 8N1 */
    outb(0x3F8 + 2, 0xC7);  /* Enable FIFO, clear, 14-byte threshold */
    outb(0x3F8 + 4, 0x0B);  /* IRQs enabled, RTS/DTR set */
    while (inb(0x3F8 + 5) & 0x01)
        (void)inb(0x3F8);
    outb(0x3F8 + 1, 0x01);  /* Enable received-data-available interrupt */
}

static inline void arch_uart_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0)
        ;
    outb(0x3F8, (uint8_t)c);
}

static inline int arch_uart_poll_getc(void) {
    if (inb(0x3F8 + 5) & 0x01) {
        return inb(0x3F8);
    }
    return -1;
}

static inline void arch_uart_flush(void) {
    while ((inb(0x3F8 + 5) & 0x40) == 0)
        ;
}

static inline void arch_uart_ack_irq(void) {}

#endif /* _ARCH_X86_64_CONSOLE_H */
