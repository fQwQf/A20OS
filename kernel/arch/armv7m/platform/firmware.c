#ifdef CONFIG_ARMV7M

#include "firmware.h"
#include "console.h"
#include "cpu.h"

void firmware_shutdown(void) {
    arch_halt();
}

void firmware_reboot(void) {
    volatile uint32_t *aircr = (volatile uint32_t *)0xE000ED0CUL;
    arch_local_irq_disable();
    *aircr = 0x05FA0004UL;
    arch_fence_i();
    arch_halt();
}

void firmware_console_putchar(char c) { arch_uart_putc(c); }
int firmware_console_getchar(void) { return arch_uart_poll_getc(); }
void firmware_set_timer(uint64_t time) { (void)time; }
void sbi_set_timer(uint64_t time) { firmware_set_timer(time); }
void sbi_console_putchar(char c) { firmware_console_putchar(c); }
int sbi_console_getchar(void) { return firmware_console_getchar(); }
void sbi_shutdown(void) { firmware_shutdown(); }
void sbi_reboot(void) { firmware_reboot(); }

#endif
