#ifdef CONFIG_LOONGARCH64

#include "firmware.h"
#include "platform.h"
#include "cpu.h"

void firmware_console_putchar(char c) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    while ((uart[5] & 0x20) == 0)
        ;
    uart[0] = (uint8_t)c;
}

int firmware_console_getchar(void) {
    volatile uint8_t *uart = (volatile uint8_t *)UART0_BASE;
    if (uart[5] & 0x01)
        return uart[0];
    return -1;
}

void firmware_set_timer(uint64_t time) {
    __asm__ __volatile("csrwr %0, 0x41" :: "r"(time | (1UL << 0)));
}

void firmware_shutdown(void) {
    arch_local_irq_disable();
    while (1)
        __asm__ __volatile__("idle 0");
}

void firmware_reboot(void) {
    firmware_shutdown();
}

/* SBI compatibility shim — called from generic code via sbi.h */
void sbi_set_timer(uint64_t time) {
    firmware_set_timer(time);
}

void sbi_console_putchar(char c) {
    firmware_console_putchar(c);
}

int sbi_console_getchar(void) {
    return firmware_console_getchar();
}

void sbi_shutdown(void) {
    firmware_shutdown();
}

void sbi_reboot(void) {
    firmware_reboot();
}

#endif /* CONFIG_LOONGARCH64 */
