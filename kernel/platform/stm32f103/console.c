#ifdef CONFIG_BOARD_STM32F103

#include "console.h"
#include "core/types.h"
#include "platform.h"
#include "drivers/stm32f1/stm32_uart.h"

#define STM32_CONSOLE_BAUD 115200U

void arch_uart_init(void) {
    (void)stm32_uart_init(STM32_UART_USART1, STM32_CONSOLE_BAUD, 1);
}

void arch_uart_putc(char c) {
    if (c == '\n')
        arch_uart_putc('\r');
    (void)stm32_uart_send_byte(
        STM32_UART_USART1, (uint8_t)c, (uint32_t)-1);
}

int arch_uart_poll_getc(void) {
    uint8_t value;
    return stm32_uart_poll_byte(STM32_UART_USART1, &value) > 0
               ? (int)value
               : -1;
}

void arch_uart_flush(void) {
    (void)stm32_uart_wait_tx_complete(
        STM32_UART_USART1, (uint32_t)-1);
}

void arch_uart_ack_irq(void) {}

uint32_t arch_uart_clock_hz(void) {
    return stm32_uart_info(STM32_UART_USART1)->clock_hz;
}
uint32_t arch_uart_baud_rate(void) {
    return stm32_uart_info(STM32_UART_USART1)->requested_baud;
}
uint32_t arch_uart_actual_baud_rate(void) {
    return stm32_uart_info(STM32_UART_USART1)->actual_baud;
}
uint32_t arch_uart_divider(void) {
    return stm32_uart_info(STM32_UART_USART1)->divider;
}
uint32_t arch_uart_error_count(void) {
    return stm32_uart_info(STM32_UART_USART1)->error_count;
}

#endif
