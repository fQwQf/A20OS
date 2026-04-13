#include "uart.h"
#include "proc.h"
#include "arch_ops.h"

#define RX_BUF_SIZE 256

static volatile char rx_buffer[RX_BUF_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;

void uart_init(void) {
    rx_head = 0;
    rx_tail = 0;
    arch_uart_init_hw();
}

void uart_putc(char c) {
    arch_uart_putc_hw(c);
}

int uart_getc(void) {
    int c;
    for (;;) {
        c = uart_try_getc();
        if (c >= 0) return c;
        proc_yield();
    }
}

int uart_try_getc(void) {
    if (rx_head != rx_tail) {
        char c = rx_buffer[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
        return (int)(unsigned char)c;
    }
    return arch_uart_try_getc_hw();
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

void uart_flush(void) {
    /* Polling transmitter flush is a no-op at this level. */
}

void uart_handle_irq(void) {
    while (1) {
        int c = arch_uart_try_getc_hw();
        if (c < 0) break;

        uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next != rx_tail) {
            rx_buffer[rx_head] = (char)c;
            rx_head = next;
        }
    }
}
