#ifndef _ARCH_AARCH64_CONSOLE_H
#define _ARCH_AARCH64_CONSOLE_H

#include "core/types.h"
#include "platform.h"

#define PL011_DR     0x00
#define PL011_FR     0x18
#define PL011_IBRD   0x24
#define PL011_FBRD   0x28
#define PL011_LCRH   0x2C
#define PL011_CR     0x30
#define PL011_IMSC   0x38
#define PL011_MIS    0x40
#define PL011_ICR    0x44

#define PL011_FR_RXFE   (1U << 4)
#define PL011_FR_TXFF   (1U << 5)
#define PL011_FR_BUSY   (1U << 3)

#define PL011_LCRH_FEN  (1U << 4)
#define PL011_LCRH_WLEN8 (3U << 5)

#define PL011_CR_UARTEN (1U << 0)
#define PL011_CR_TXE    (1U << 8)
#define PL011_CR_RXE    (1U << 9)

#define PL011_IMSC_RXIM (1U << 4)
#define PL011_IMSC_RTIM (1U << 6)

static inline volatile uint32_t *arch_uart_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(UART0_BASE + off);
}

static inline void arch_uart_init(void) {
    *arch_uart_reg32(PL011_CR) = 0;
    *arch_uart_reg32(PL011_ICR) = 0x7FF;
    *arch_uart_reg32(PL011_IBRD) = 13;
    *arch_uart_reg32(PL011_FBRD) = 1;
    *arch_uart_reg32(PL011_LCRH) = PL011_LCRH_FEN | PL011_LCRH_WLEN8;
    *arch_uart_reg32(PL011_IMSC) = PL011_IMSC_RXIM | PL011_IMSC_RTIM;
    *arch_uart_reg32(PL011_CR) = PL011_CR_UARTEN | PL011_CR_TXE | PL011_CR_RXE;
}

static inline void arch_uart_putc(char c) {
    while (*arch_uart_reg32(PL011_FR) & PL011_FR_TXFF)
        ;
    *arch_uart_reg32(PL011_DR) = (uint32_t)(uint8_t)c;
}

static inline int arch_uart_poll_getc(void) {
    if (*arch_uart_reg32(PL011_FR) & PL011_FR_RXFE)
        return -1;
    return (int)(*arch_uart_reg32(PL011_DR) & 0xFF);
}

static inline void arch_uart_flush(void) {
    while (*arch_uart_reg32(PL011_FR) & PL011_FR_BUSY)
        ;
}

static inline void arch_uart_ack_irq(void) {
    *arch_uart_reg32(PL011_ICR) = PL011_IMSC_RXIM | PL011_IMSC_RTIM;
}

#endif /* _ARCH_AARCH64_CONSOLE_H */
