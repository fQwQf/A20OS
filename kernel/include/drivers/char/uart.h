#ifndef _UART_H
#define _UART_H

#include "core/types.h"
#include "core/sync.h"

void uart_init(void);
void uart_putc(char c);
void uart_receive_char(char c);
int  uart_getc(void);
int  uart_try_getc(void);
int  uart_has_input(void);
wait_queue_t *uart_read_wait_queue(void);
void uart_puts(const char *s);
void uart_flush(void);
void uart_handle_irq(void);
int  uart_get_foreground_pgid(void);
void uart_set_foreground_pgid(int pgid);

#if defined(CONFIG_BOARD_LS2K1000) && defined(CONFIG_COOPERATIVE_BOOT) && \
    defined(CONFIG_LS2K_TRAP_TRACE)
static inline void uart_trace_ls2k_syscall(reg_t number)
{
    uart_putc('{');
    for (int shift = 12; shift >= 0; shift -= 4) {
        unsigned int digit = (unsigned int)((number >> shift) & 0xf);
        uart_putc((char)(digit < 10 ? '0' + digit : 'a' + digit - 10));
    }
    uart_putc('}');
}

#define LS2K_SYSCALL_TRACE(number) uart_trace_ls2k_syscall(number)
#define LS2K_SYSCALL_DONE() uart_putc('!')
#else
#define LS2K_SYSCALL_TRACE(number) do { } while (0)
#define LS2K_SYSCALL_DONE() do { } while (0)
#endif

#endif /* _UART_H */
