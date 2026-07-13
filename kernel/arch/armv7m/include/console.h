#ifndef _ARCH_ARMV7M_CONSOLE_H
#define _ARCH_ARMV7M_CONSOLE_H

void arch_uart_init(void);
void arch_uart_putc(char c);
int arch_uart_poll_getc(void);
void arch_uart_flush(void);
void arch_uart_ack_irq(void);

#endif
