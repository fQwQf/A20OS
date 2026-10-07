#include "drivers/char/uart.h"
#include "console.h"
#include "core/arch.h"

#define MCU_UART_RX_SIZE 128U

static volatile unsigned uart_rx_head;
static volatile unsigned uart_rx_tail;
static volatile char uart_rx_buffer[MCU_UART_RX_SIZE];

void uart_init(void) {
    uart_rx_head = 0;
    uart_rx_tail = 0;
    arch_uart_init();
}

void uart_putc(char c) { arch_uart_putc(c); }

void uart_receive_char(char c) {
    unsigned next = (uart_rx_head + 1U) % MCU_UART_RX_SIZE;
    if (next == uart_rx_tail)
        return;
    uart_rx_buffer[uart_rx_head] = c;
    uart_rx_head = next;
}

int uart_getc(void) {
    int c;
    while ((c = uart_try_getc()) < 0)
        arch_wfi();
    return c;
}

int uart_try_getc(void) {
    uint32_t flags = arch_irq_save();
    if (uart_rx_head == uart_rx_tail) {
        arch_irq_restore(flags);
        return -1;
    }
    int c = (unsigned char)uart_rx_buffer[uart_rx_tail];
    uart_rx_tail = (uart_rx_tail + 1U) % MCU_UART_RX_SIZE;
    arch_irq_restore(flags);
    return c;
}

int uart_has_input(void) {
    uint32_t flags = arch_irq_save();
    int has_input = uart_rx_head != uart_rx_tail;
    arch_irq_restore(flags);
    return has_input;
}
void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
void uart_flush(void) { arch_uart_flush(); }
void uart_handle_irq(void) {}
int uart_get_foreground_pgid(void) { return 0; }
void uart_set_foreground_pgid(int pgid) { (void)pgid; }
