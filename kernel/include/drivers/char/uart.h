#ifndef _UART_H
#define _UART_H

#include "core/types.h"

void uart_init(void);
void uart_putc(char c);
int  uart_getc(void);
int  uart_try_getc(void);
int  uart_has_input(void);
void uart_puts(const char *s);
void uart_flush(void);
void uart_handle_irq(void);
int  uart_get_foreground_pgid(void);
void uart_set_foreground_pgid(int pgid);

#endif /* _UART_H */
