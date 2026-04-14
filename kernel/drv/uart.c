#include "uart.h"
#include "consts.h"
#include "defs.h"
#include "proc.h"

#define UART0 ((volatile uint8_t *)UART0_BASE)

#define RHR 0
#define THR 0
#define IER 1
#define FCR 2
#define LCR 3
#define MCR 4
#define LSR 5

#define IER_RX_ENABLE (1 << 0)
#define IER_TX_ENABLE (1 << 1)
#define LSR_RX_READY  (1 << 0)
#define LSR_TX_IDLE   (1 << 5)
#define LSR_TX_EMPTY  (1 << 6)

#define RX_BUF_SIZE 256

static volatile char rx_buffer[RX_BUF_SIZE];
static volatile uint32_t rx_head;
static volatile uint32_t rx_tail;

void uart_init(void) {
    rx_head = 0;
    rx_tail = 0;

    UART0[IER] = 0x00;
    UART0[LCR] = 0x80;
    UART0[0] = 0x03;
    UART0[1] = 0x00;
    UART0[LCR] = 0x03;
    UART0[FCR] = 0x07;
    UART0[MCR] = 0x0B;
    UART0[IER] = IER_RX_ENABLE;
    uart_flush();
}

void uart_putc(char c) {
    while (!(UART0[LSR] & LSR_TX_IDLE));
    UART0[THR] = (uint8_t)c;
}

int uart_getc(void) {
    while (rx_head == rx_tail) {
        w_sstatus(r_sstatus() | 0x2); /* SSTATUS_SIE is bit 1 */
        proc_yield();
        w_sstatus(r_sstatus() & ~0x2);
    }
    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return (int)(unsigned char)c;
}

int uart_try_getc(void) {
    if (rx_head == rx_tail)
        return -1;
    char c = rx_buffer[rx_tail];
    rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
    return (int)(unsigned char)c;
}

void uart_puts(const char *s) {
    while (*s) uart_putc(*s++);
}

void uart_flush(void) {
    while (!(UART0[LSR] & LSR_TX_EMPTY));
}

void uart_handle_irq(void) {
    while (UART0[LSR] & LSR_RX_READY) {
        char c = UART0[RHR];
        uint32_t next = (rx_head + 1) % RX_BUF_SIZE;
        if (next != rx_tail) {
            rx_buffer[rx_head] = c;
            rx_head = next;
        }
    }
}
