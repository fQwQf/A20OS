#ifndef _ARCH_LOONGARCH64_CONSOLE_H
#define _ARCH_LOONGARCH64_CONSOLE_H

#include "platform.h"

static inline int arch_uart_poll_getc(void) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    if (uart[5] & 0x01)
        return uart[0];
    return -1;
}

#endif /* _ARCH_LOONGARCH64_CONSOLE_H */
