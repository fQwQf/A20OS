#ifndef _STM32F103_BOARD_H
#define _STM32F103_BOARD_H
#include <stdint.h>

int stm32_cluster_boot_identity(uint8_t node_id[16], uint64_t *epoch);

#ifdef CONFIG_STM32_CLUSTER_PORT
int stm32_cluster_uart_init(void);
void stm32_cluster_uart_irq(void);
uint32_t stm32_cluster_uart_rx_drops(void);
#endif

void stm32_status_led_init(void);
void stm32_status_led_set(int on);
void stm32_status_led_toggle(void);

#endif
