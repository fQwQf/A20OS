#ifndef _STM32F103_UART_H
#define _STM32F103_UART_H

#include "core/types.h"

typedef enum stm32_uart_port {
    STM32_UART_USART1 = 0,
    STM32_UART_USART3,
    STM32_UART_PORT_COUNT,
} stm32_uart_port_t;

typedef struct stm32_uart_info {
    uint32_t clock_hz;
    uint32_t requested_baud;
    uint32_t actual_baud;
    uint32_t divider;
    uint32_t error_count;
    uint32_t rx_bytes;
    uint32_t tx_bytes;
    uint32_t rx_transitions;
    uint8_t last_rx_byte;
    int initialized;
    int rx_irq_enabled;
} stm32_uart_info_t;

int stm32_uart_init(stm32_uart_port_t port, uint32_t baud_rate,
                    int enable_rx_irq);
int stm32_uart_set_baud(stm32_uart_port_t port, uint32_t baud_rate);
void stm32_uart_set_rx_irq(stm32_uart_port_t port, int enable);
int stm32_uart_rx_irq_enabled(stm32_uart_port_t port);
int stm32_uart_send_byte(stm32_uart_port_t port, uint8_t value,
                         uint32_t timeout);
int stm32_uart_wait_tx_complete(stm32_uart_port_t port, uint32_t timeout);
int stm32_uart_poll_byte(stm32_uart_port_t port, uint8_t *value);
void stm32_uart_drain_rx(stm32_uart_port_t port);
int stm32_uart_rx_pin_level(stm32_uart_port_t port);
const stm32_uart_info_t *stm32_uart_info(stm32_uart_port_t port);

uint32_t stm32_hclk_hz(void);
uint32_t stm32_pclk1_hz(void);
uint32_t stm32_pclk2_hz(void);

#endif
