/* Dedicated PC COM2, never the COM1 console. No protocol logic here. */
#ifdef CONFIG_CLUSTER_UART_COM2
#include "core/arch.h"
#include "cluster/uart.h"

#define COM2 0x2f8u
#define COM2_RX_BYTES 1024u
static uint8_t com2_rx[COM2_RX_BYTES];
static unsigned com2_head, com2_tail;
static uint64_t com2_drops;

/* Called only by the serialized UART polling owner, including during TX.
 * Servicing RX between transmitted bytes avoids a full-duplex FIFO stall. */
static void com2_drain(void) {
    for (unsigned i = 0; i < 64; i++) {
        uint8_t status = inb(COM2 + 5);
        if (status & 0x1e)
            com2_drops++;
        if (!(status & 1))
            break;
        uint8_t byte = inb(COM2);
        unsigned next = (com2_head + 1) % COM2_RX_BYTES;
        if ((status & 0x1c) || next == com2_tail)
            com2_drops++;
        else {
            com2_rx[com2_head] = byte;
            com2_head = next;
        }
    }
}
static int com2_get(void *ctx) {
    (void)ctx;
    com2_drain();
    if (com2_tail == com2_head)
        return -1;
    int byte = com2_rx[com2_tail];
    com2_tail = (com2_tail + 1) % COM2_RX_BYTES;
    return byte;
}
static int com2_put(void *ctx, uint8_t byte) {
    (void)ctx;
    for (unsigned i = 0; i < 100000; i++) {
        com2_drain();
        if (inb(COM2 + 5) & 0x20) {
            outb(COM2, byte);
            return 0;
        }
        arch_cpu_relax();
    }
    return -1;
}
int arch_cluster_uart_com2_init(a20_clx_uart_drv_t *driver) {
    /* Probe the scratch register before programming an absent legacy UART. */
    uint8_t saved = inb(COM2 + 7);
    outb(COM2 + 7, 0xa5);
    int present = inb(COM2 + 7) == 0xa5;
    outb(COM2 + 7, saved);
    if (!present)
        return -1;
    outb(COM2 + 1, 0); /* Polling owner; no IRQ handler shares the stream. */
    outb(COM2 + 3, 0x80);
    outb(COM2, 1); /* 115200 baud divisor. */
    outb(COM2 + 1, 0);
    outb(COM2 + 3, 3); /* 8N1. */
    outb(COM2 + 2, 0xc7);
    outb(COM2 + 4, 3); /* DTR/RTS, no loopback. */
    com2_head = com2_tail = 0;
    com2_drops = 0;
    driver->ctx = NULL;
    driver->getc = com2_get;
    driver->putc = com2_put;
    return 0;
}
uint64_t arch_cluster_uart_com2_drops(void) {
    return com2_drops;
}
#endif
