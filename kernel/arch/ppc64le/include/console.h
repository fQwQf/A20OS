#ifndef _ARCH_PPC64LE_CONSOLE_H
#define _ARCH_PPC64LE_CONSOLE_H

#include "core/types.h"

void firmware_console_putchar(char c);
int firmware_console_getchar(void);

static inline void arch_uart_init(void) {
}

static inline void arch_uart_putc(char c) {
    firmware_console_putchar(c);
}

static inline int arch_uart_poll_getc(void) {
    return firmware_console_getchar();
}

static inline void arch_uart_flush(void) {
}

static inline void arch_uart_ack_irq(void) {
}

#endif /* _ARCH_PPC64LE_CONSOLE_H */
