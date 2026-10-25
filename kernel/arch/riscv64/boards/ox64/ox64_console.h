#ifndef _OX64_CONSOLE_H
#define _OX64_CONSOLE_H

/* Bouffalo UART3 is not a 16550. Preserve U-Boot's clock/pinmux/2 Mbaud
 * setup. Offsets and FIFO fields: U-Boot drivers/serial/serial_bflb.c;
 * interrupt masks: Apache NuttX hardware/bl808_uart.h. */
static inline volatile uint32_t *ox64_uart(unsigned offset) {
    return (volatile uint32_t *)(UART0_BASE + offset);
}
static inline void arch_uart_init(void) {
    *ox64_uart(0x24) = 0xff; /* Mask all sources during setup. */
    *ox64_uart(0x2c) = 1U << 3; /* RX FIFO interrupt only, level while nonempty. */
    *ox64_uart(0x84) &= ~(0x1fU << 24); /* Trigger at one RX byte. */
    *ox64_uart(0x24) = 0xffU & ~(1U << 3);
}
static inline void arch_uart_putc(char c) {
    while ((*ox64_uart(0x84) & 0x3fU) == 0) {}
    *ox64_uart(0x88) = (uint8_t)c;
}
static inline int arch_uart_poll_getc(void) {
    if ((*ox64_uart(0x84) >> 8) & 0x3fU)
        return *ox64_uart(0x8c) & 0xffU;
    return -1;
}
static inline void arch_uart_flush(void) {
    while ((*ox64_uart(0x84) & 0x3fU) != 32 || (*ox64_uart(0x30) & 1U)) {}
}
/* RX FIFO IRQ is level-triggered and clears when the shared driver drains it. */
static inline void arch_uart_ack_irq(void) {}
#endif
