#ifndef _ARCH_ARMV7M_CONSOLE_H
#define _ARCH_ARMV7M_CONSOLE_H

#include "core/types.h"

void arch_uart_init(void);
void arch_uart_putc(char c);
int arch_uart_poll_getc(void);
void arch_uart_flush(void);
void arch_uart_ack_irq(void);
uint32_t arch_uart_clock_hz(void);
uint32_t arch_uart_baud_rate(void);
uint32_t arch_uart_actual_baud_rate(void);
uint32_t arch_uart_divider(void);
uint32_t arch_uart_error_count(void);

#endif
