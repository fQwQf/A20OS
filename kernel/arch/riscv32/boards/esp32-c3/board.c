#include "core/arch.h"
#include "drivers/core/driver_core.h"
#include "drivers/char/uart.h"
#include "drivers/char/esp_uart.h"
#include "drivers/timer/esp_systimer.h"
#include "mcu.h"

/* ESP-IDF v5.4.2 register definitions, independently implemented here.
 * Keep SoC addresses/routing here and reusable register protocols in drivers. */
#define REG(a) (*(volatile uint32_t *)(uintptr_t)(a))
#define UART_BASE 0x60000000UL
#define SYSTIMER_BASE 0x60023000UL
#define INTC_BASE 0x600c2000UL
#define SYSTEM_BASE 0x600c0000UL
#define RTC_BASE 0x60008000UL
#define TIMER_CPU_IRQ 1U
#define SYSTIMER_HZ 16000000U
static uint64_t timer_origin;

static void disable_watchdogs(void) {
    REG(RTC_BASE + 0xa8) = 0x50d83aa1U;
    REG(RTC_BASE + 0x90) &= ~(1U << 31);
    REG(RTC_BASE + 0xa8) = 0;
    REG(RTC_BASE + 0xb0) = 0x8f1d312aU;
    REG(RTC_BASE + 0xac) |= 1U << 31; /* Super watchdog auto feed. */
    REG(RTC_BASE + 0xb0) = 0;
    for (unsigned i = 0; i < 2; i++) {
        uintptr_t base = 0x6001f000UL + i * 0x1000UL;
        REG(base + 0x64) = 0x50d83aa1U;
        REG(base + 0x48) &= ~(1U << 31);
        REG(base + 0x64) = 0;
    }
}
static void early_init(void) {
    disable_watchdogs();
    /* Disconnect every peripheral source before assigning our timer. */
    REG(INTC_BASE + 0x104) = 0;
    for (unsigned i = 0; i < 62; i++) REG(INTC_BASE + 4U * i) = 0;
    REG(INTC_BASE + 0x108) = 0; /* Level interrupts. */
    REG(INTC_BASE + 0x194) = 0; /* Priority threshold. */
}
void arch_uart_init(void) {
    /* ROM SPI boot configures GPIO21 TX / GPIO20 RX, 115200 8N1.
     * Keep ROM clock/pin configuration; no SDK or FreeRTOS is linked. */
    REG(UART_BASE + 0xc) = 0; /* Poll RX from timer IRQ, no UART IRQ needed. */
}
void arch_uart_putc(char c) { if (c == '\n') esp_uart_putc(UART_BASE, '\r'); esp_uart_putc(UART_BASE, c); }
int arch_uart_poll_getc(void) { return esp_uart_getc(UART_BASE); }
void arch_uart_flush(void) { esp_uart_flush(UART_BASE); }
void arch_uart_ack_irq(void) {}
int arch_uart_irq(void) { return 21; } /* UART0 peripheral source. */
void riscv_mcu_timer_init(void) {
    REG(SYSTEM_BASE + 0x10) |= 1U << 29;
    REG(SYSTEM_BASE + 0x18) &= ~(1U << 29);
    REG(INTC_BASE + 0x94) = TIMER_CPU_IRQ; /* SYSTIMER target0 source 37. */
    REG(INTC_BASE + 0x114 + 4U * TIMER_CPU_IRQ) = 1U;
    esp_systimer_init(SYSTIMER_BASE, SYSTIMER_HZ / ARCH_TIMER_FREQ);
    timer_origin = esp_systimer_read(SYSTIMER_BASE);
    REG(INTC_BASE + 0x104) = 1U << TIMER_CPU_IRQ;
    arch_write_sie(1U << 11); /* Machine external interrupt delivery. */
}
uint64_t riscv_mcu_timer_read(void) {
    return (esp_systimer_read(SYSTIMER_BASE) - timer_origin) / (SYSTIMER_HZ / ARCH_TIMER_FREQ);
}
void riscv_mcu_timer_enable(int en) { esp_systimer_enable(SYSTIMER_BASE, en); }
void riscv_mcu_timer_set_interval(uint64_t ticks) { (void)ticks; }
uint64_t riscv_mcu_timer_interval(void) { return 1; }
void riscv_mcu_timer_rearm(void) {} /* Hardware periodic comparator. */
int riscv_mcu_irq_dispatch(uint32_t cause) {
    if (cause != TIMER_CPU_IRQ) return -1;
    esp_systimer_ack(SYSTIMER_BASE);
    REG(INTC_BASE + 0x10c) = 1U << TIMER_CPU_IRQ;
    REG(INTC_BASE + 0x10c) = 0;
    int c;
    while ((c = arch_uart_poll_getc()) >= 0) uart_receive_char((char)c);
    return 0;
}
void riscv_mcu_reboot(void) {
    arch_local_irq_disable();
    arch_uart_flush();
    /* ESP32-C3 mask ROM software_reset, stable ROM ABI. */
    ((void (*)(void))(uintptr_t)0x40000090UL)();
}
static const board_config_t board = {
    .name = "esp32-c3", .ram_base = PHYS_MEMORY_BASE, .ram_end = PHYS_MEMORY_END,
    .early_init = early_init, .reboot = riscv_mcu_reboot,
};
const board_config_t *const current_board = &board;
