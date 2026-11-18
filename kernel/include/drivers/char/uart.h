#ifndef _UART_H
#define _UART_H

#include "core/types.h"
#include "core/sync.h"

int arch_uart_irq(void);
void uart_init(void);
void uart_putc(char c);
/* Caller holds the console serialization lock, as for uart_putc(). */
void uart_write(const char *data, size_t length);
void uart_receive_char(char c);
int  uart_getc(void);
struct vfile;
int uart_getc_vfile(struct vfile *vf);
void uart_tty_wake(void);
int  uart_try_getc(void);
int  uart_has_input(void);
/* NULL on polling-only boards, so readiness uses its timed fallback. */
wait_queue_t *uart_read_wait_queue(void);
void uart_puts(const char *s);
void uart_flush(void);
void uart_handle_irq(void);
int  uart_get_foreground_pgid(void);

#endif /* _UART_H */
