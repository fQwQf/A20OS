#ifdef CONFIG_ARM32

#include "firmware.h"
#include "cpu.h"
#include "console.h"

#define PSCI_SYSTEM_OFF     0x84000008U
#define PSCI_SYSTEM_RESET   0x84000009U

static void arm32_psci_call(uint32_t fn_id) {
    __asm__ __volatile__(
        ".arch_extension virt\n"
        "mov r0, %0\n"
        "hvc #0\n"
        :
        : "r"(fn_id)
        : "r0", "r1", "r2", "r3", "memory"
    );
}

void firmware_shutdown(void) {
    arch_local_irq_disable();
    arm32_psci_call(PSCI_SYSTEM_OFF);
    arch_halt();
}

void firmware_reboot(void) {
    arch_local_irq_disable();
    arm32_psci_call(PSCI_SYSTEM_RESET);
    arch_halt();
}

void firmware_console_putchar(char c) {
    arch_uart_putc(c);
}

int firmware_console_getchar(void) {
    return arch_uart_poll_getc();
}

void firmware_set_timer(uint64_t time) {
    (void)time;
}

void sbi_set_timer(uint64_t time) { firmware_set_timer(time); }
void sbi_console_putchar(char c) { firmware_console_putchar(c); }
int sbi_console_getchar(void) { return firmware_console_getchar(); }
void sbi_shutdown(void) { firmware_shutdown(); }
void sbi_reboot(void) { firmware_reboot(); }

#endif
