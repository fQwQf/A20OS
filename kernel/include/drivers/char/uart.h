#ifndef _UART_H
#define _UART_H

#include "core/types.h"
#include "core/sync.h"

void uart_init(void);
void uart_putc(char c);
void uart_receive_char(char c);
/* Prints the console RX integrity counters (adjacent-character transpositions
 * and ring-buffer drops) once they are non-zero.  Safe from task context only:
 * it writes to the same UART, so calling it from the top half would re-enter
 * the console under its own IRQ.  See the RX_FIDELITY note in uart.c. */
void uart_rx_fidelity_report(void);
int  uart_getc(void);
int  uart_try_getc(void);
int  uart_has_input(void);
wait_queue_t *uart_read_wait_queue(void);
void uart_puts(const char *s);
void uart_flush(void);
void uart_handle_irq(void);
int  uart_get_foreground_pgid(void);
void uart_set_foreground_pgid(int pgid);

#endif /* _UART_H */
