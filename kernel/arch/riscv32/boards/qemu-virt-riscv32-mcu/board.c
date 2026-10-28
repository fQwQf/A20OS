#include "core/arch.h"
#include "drivers/core/driver_core.h"
#include "drivers/char/uart.h"
#include "mcu.h"
#define MTIME ((volatile uint32_t *)0x0200bff8UL)
#define MTIMECMP ((volatile uint32_t *)0x02004000UL)
#define UART ((volatile uint8_t *)0x10000000UL)
static uint64_t read_time(void) {
    uint32_t a, b, lo;
    do { a = MTIME[1]; lo = MTIME[0]; b = MTIME[1]; } while (a != b);
    return ((uint64_t)a << 32) | lo;
}
void riscv_mcu_timer_rearm(void) {
    uint64_t t = read_time() + 10000;
    MTIMECMP[0] = ~0U; MTIMECMP[1] = (uint32_t)(t >> 32); MTIMECMP[0] = (uint32_t)t;
}
void riscv_mcu_timer_enable(int en) { arch_write_sie(en ? 1U << 7 : 0); }
void riscv_mcu_timer_init(void) { riscv_mcu_timer_rearm(); riscv_mcu_timer_enable(1); }
uint64_t riscv_mcu_timer_read(void) { return read_time() / 10000U; }
int riscv_mcu_irq_dispatch(uint32_t cause) {
    if (cause != 7) return -1;
    riscv_mcu_timer_rearm();
    int c;
    while ((c = arch_uart_poll_getc()) >= 0) uart_receive_char((char)c);
    return 0;
}
void arch_uart_init(void) { UART[1] = 0; UART[3] = 3; }
void arch_uart_putc(char c) { if (c == '\n') arch_uart_putc('\r'); while (!(UART[5] & 0x20)) {} UART[0] = (uint8_t)c; }
int arch_uart_poll_getc(void) { return UART[5] & 1 ? UART[0] : -1; }
void arch_uart_flush(void) { while (!(UART[5] & 0x40)) {} }
void arch_uart_ack_irq(void) {}
int arch_uart_irq(void) { return 10; }
void riscv_mcu_reboot(void) { *(volatile uint32_t *)0x100000UL = 0x7777; }
static const board_config_t board = {
    .name = "qemu-virt-riscv32-mcu", .ram_base = PHYS_MEMORY_BASE, .ram_end = PHYS_MEMORY_END,
    .reboot = riscv_mcu_reboot,
};
const board_config_t *const current_board = &board;
