#ifndef _ARCH_X86_64_CONSOLE_H
#define _ARCH_X86_64_CONSOLE_H

#include "core/types.h"
#include "cpu.h"

static inline void arch_uart_init(void) {
    outb(0x3F8 + 1, 0x00);  /* Disable interrupts */
    outb(0x3F8 + 3, 0x80);  /* Enable DLAB */
#ifdef CONFIG_BOARD_QEMU_VIRT_X86_64
    outb(0x3F8 + 0, 0x01);  /* QEMU: divisor = 1 (115200 baud) */
#else
    outb(0x3F8 + 0, 0x03);  /* Physical boards retain 38400 baud. */
#endif
    outb(0x3F8 + 1, 0x00);
    outb(0x3F8 + 3, 0x03);  /* 8N1 */
    outb(0x3F8 + 2, 0xC7);  /* Enable FIFO, clear, 14-byte threshold */
    outb(0x3F8 + 4, 0x0B);  /* IRQs enabled, RTS/DTR set */
    while (inb(0x3F8 + 5) & 0x01)
        (void)inb(0x3F8);
    outb(0x3F8 + 1, 0x01);  /* Enable received-data-available interrupt */
}

#ifdef CONFIG_QEMU_SHARED_SMP
void x86_shared_console_putc(char c);
int x86_shared_console_getc(void);
void x86_shared_console_flush(void);
static inline void arch_uart_putc(char c) { x86_shared_console_putc(c); }
static inline int arch_uart_poll_getc(void) { return x86_shared_console_getc(); }
static inline void arch_uart_flush(void) { x86_shared_console_flush(); }
#else
#define ARCH_UART_HAS_WRITE 1
/* THRE means the transmit FIFO is empty. Fill at most its 16-byte depth;
 * string I/O also lets KVM submit the burst in one port-I/O transaction.
 * A UART without enabled FIFOs keeps the single-byte path. Callers serialize
 * this with every other console writer, including interrupt-time printk. */
static inline void arch_uart_write(const char *data, size_t length) {
    size_t depth = (inb(0x3F8 + 2) & 0xc0) == 0xc0 ? 16 : 1;
    while (length) {
        while (!(inb(0x3F8 + 5) & 0x20))
            ;
        size_t n = length < depth ? length : depth;
        length -= n;
        __asm__ __volatile__("rep outsb" : "+S"(data), "+c"(n)
                             : "d"((uint16_t)0x3F8) : "memory");
    }
}

static inline void arch_uart_putc(char c) {
    while ((inb(0x3F8 + 5) & 0x20) == 0)
        ;
    outb(0x3F8, (uint8_t)c);
}

static inline int arch_uart_poll_getc(void) {
    if (inb(0x3F8 + 5) & 0x01) {
        return inb(0x3F8);
    }
    return -1;
}

static inline void arch_uart_flush(void) {
    while ((inb(0x3F8 + 5) & 0x40) == 0)
        ;
}

#endif

static inline void arch_uart_ack_irq(void) {}

#endif /* _ARCH_X86_64_CONSOLE_H */
