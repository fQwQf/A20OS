#include "drivers/char/uart.h"
#include "console.h"
#include "core/arch.h"

void uart_init(void) { arch_uart_init(); }
void uart_putc(char c) { arch_uart_putc(c); }
void uart_receive_char(char c) { (void)c; }

int uart_getc(void) {
    int c;
    while ((c = arch_uart_poll_getc()) < 0)
        arch_wfi();
    return c;
}

int uart_try_getc(void) { return arch_uart_poll_getc(); }
int uart_has_input(void) { return arch_uart_poll_getc() >= 0; }
void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
void uart_flush(void) { arch_uart_flush(); }
void uart_handle_irq(void) {}
int uart_get_foreground_pgid(void) { return 0; }
void uart_set_foreground_pgid(int pgid) { (void)pgid; }
