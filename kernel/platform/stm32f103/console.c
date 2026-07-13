#ifdef CONFIG_BOARD_STM32F103

#include "console.h"
#include "core/types.h"
#include "platform.h"

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)
#define GPIOA_CRH   (*(volatile uint32_t *)0x40010804UL)
#define USART1_SR   (*(volatile uint32_t *)0x40013800UL)
#define USART1_DR   (*(volatile uint32_t *)0x40013804UL)
#define USART1_BRR  (*(volatile uint32_t *)0x40013808UL)
#define USART1_CR1  (*(volatile uint32_t *)0x4001380CUL)

#define USART_SR_RXNE (1U << 5)
#define USART_SR_TC   (1U << 6)
#define USART_SR_TXE  (1U << 7)
#define USART_CR1_RE  (1U << 2)
#define USART_CR1_TE  (1U << 3)
#define USART_CR1_RXNEIE (1U << 5)
#define USART_CR1_UE  (1U << 13)

#define NVIC_ISER1   (*(volatile uint32_t *)0xE000E104UL)

void arch_uart_init(void) {
    RCC_APB2ENR |= (1U << 2) | (1U << 14);

    uint32_t crh = GPIOA_CRH;
    crh &= ~((0xFU << 4) | (0xFU << 8));
    crh |= (0xBU << 4) | (0x4U << 8);
    GPIOA_CRH = crh;

    /* Reset clock is HSI=8 MHz: 8,000,000 / 115,200 ~= 69.44. */
    USART1_BRR = 0x45U;
    USART1_CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE;
    NVIC_ISER1 = 1U << (UART0_IRQ - 32U);
}

void arch_uart_putc(char c) {
    if (c == '\n')
        arch_uart_putc('\r');
    while (!(USART1_SR & USART_SR_TXE))
        ;
    USART1_DR = (uint32_t)(uint8_t)c;
}

int arch_uart_poll_getc(void) {
    if (!(USART1_SR & USART_SR_RXNE))
        return -1;
    return (int)(USART1_DR & 0xFFU);
}

void arch_uart_flush(void) {
    while (!(USART1_SR & USART_SR_TC))
        ;
}

void arch_uart_ack_irq(void) {
    (void)USART1_SR;
}

#endif
