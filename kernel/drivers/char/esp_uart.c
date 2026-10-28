#include "drivers/char/esp_uart.h"
#define REG(base, off) (*(volatile uint32_t *)((base) + (off)))
void esp_uart_putc(uintptr_t base, char c) {
    while (((REG(base, 0x1c) >> 16) & 0x3ffU) >= 127U) {}
    REG(base, 0) = (uint8_t)c;
}
int esp_uart_getc(uintptr_t base) {
    return (REG(base, 0x1c) & 0x3ffU) ? (int)(REG(base, 0) & 255U) : -1;
}
void esp_uart_flush(uintptr_t base) {
    while ((REG(base, 0x1c) >> 16) & 0x3ffU) {}
}
