#ifndef _UART_H
#define _UART_H

#include "types.h"

void uart_init(void);
void uart_putc(char c);
int  uart_getc(void);
int  uart_try_getc(void);
void uart_puts(const char *s);
void uart_flush(void);
void uart_handle_irq(void);

#endif /* _UART_H */
