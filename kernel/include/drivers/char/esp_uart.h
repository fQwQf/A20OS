#ifndef A20_ESP_UART_H
#define A20_ESP_UART_H
#include "core/types.h"
/* ESP32-C3 UART register protocol. Clock/pin setup remains board-owned. */
void esp_uart_putc(uintptr_t base, char c);
int esp_uart_getc(uintptr_t base);
void esp_uart_flush(uintptr_t base);
#endif
