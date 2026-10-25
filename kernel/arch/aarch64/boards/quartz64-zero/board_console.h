#ifndef _QUARTZ64_ZERO_CONSOLE_H
#define _QUARTZ64_ZERO_CONSOLE_H
/* DW APB UART, 32-bit accesses, register shift 2. U-Boot owns pinmux,
 * clock/reset and the 1500000 baud divisor; never apply PL011 writes here. */
static inline volatile uint32_t *quartz_uart_reg(unsigned index) {
    return (volatile uint32_t *)(uintptr_t)(UART0_BASE + 4U * index);
}
static inline void arch_uart_flush(void) {
    while (!(*quartz_uart_reg(5) & 0x40U))
        ;
}
static inline void arch_uart_init(void) {
    arch_uart_flush();
    *quartz_uart_reg(3) = 3; /* 8N1, DLAB clear; preserve firmware divisor. */
    *quartz_uart_reg(1) = 0;
    *quartz_uart_reg(2) = 1; /* FIFO enabled, one-byte RX threshold. */
    *quartz_uart_reg(1) = 1; /* Received data/character-timeout interrupt. */
}
static inline void arch_uart_putc(char c) {
    while (!(*quartz_uart_reg(5) & 0x20U))
        ;
    *quartz_uart_reg(0) = (uint8_t)c;
}
static inline int arch_uart_poll_getc(void) {
    if (!(*quartz_uart_reg(5) & 1U))
        return -1;
    return *quartz_uart_reg(0) & 0xffU;
}
static inline void arch_uart_ack_irq(void) {
    /* Draining RX deasserts RDA/timeout; read USR for DW busy-detect. */
    (void)*quartz_uart_reg(2);
    (void)*quartz_uart_reg(31);
}
#endif
