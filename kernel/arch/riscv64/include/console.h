#ifndef _ARCH_RISCV64_CONSOLE_H
#define _ARCH_RISCV64_CONSOLE_H

#include "core/types.h"
#include "platform.h"
#ifdef CONFIG_BOARD_VISIONFIVE2
#include "firmware.h"
#endif

static inline void arch_uart_init(void) {
#ifdef CONFIG_BOARD_VISIONFIVE2
    /*
     * U-Boot/OpenSBI already owns the JH7110 UART setup.  During hardware
     * bring-up, retain that console through SBI instead of applying the QEMU
     * virt 16550 divisor sequence to the device.
     */
    return;
#else
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
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
#ifdef CONFIG_BOARD_VISIONFIVE2
    /* OpenSBI advertises the legacy console extension on this board. */
    firmware_console_putchar(c);
#else
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    while ((uart[5] & 0x20) == 0)
        ;
    uart[0] = (uint8_t)c;
#endif
}

static inline int arch_uart_poll_getc(void) {
#ifdef CONFIG_BOARD_VISIONFIVE2
    return firmware_console_getchar();
#else
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    if (uart[5] & 0x01)
        return uart[0];
    return -1;
#endif
}

static inline void arch_uart_flush(void) {
#ifdef CONFIG_BOARD_VISIONFIVE2
    return;
#else
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    while ((uart[5] & 0x40) == 0)
        ;
#endif
}

static inline void arch_uart_ack_irq(void) {}

#endif /* _ARCH_RISCV64_CONSOLE_H */
