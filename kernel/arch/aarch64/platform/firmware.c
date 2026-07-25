#ifdef CONFIG_AARCH64

#include "firmware.h"
#include "platform.h"
#include "cpu.h"
#include "console.h"

#define PSCI_SYSTEM_OFF   0x84000008UL
#define PSCI_SYSTEM_RESET 0x84000009UL
#define PSCI_CPU_ON       0xC4000003UL

static inline uint64_t psci_call0(uint64_t fn) {
    register uint64_t x0 __asm__("x0") = fn;
    __asm__ __volatile__(
        "hvc #0"
        : "+r"(x0)
        :
        : "x1", "x2", "x3", "memory");
    return x0;
}

int64_t firmware_cpu_on(uint64_t target_cpu, uint64_t entry, uint64_t context) {
    register uint64_t x0 __asm__("x0") = PSCI_CPU_ON;
    register uint64_t x1 __asm__("x1") = target_cpu;
    register uint64_t x2 __asm__("x2") = entry;
    register uint64_t x3 __asm__("x3") = context;
    __asm__ __volatile__(
        "hvc #0"
        : "+r"(x0)
        : "r"(x1), "r"(x2), "r"(x3)
        : "memory");
    return (int64_t)x0;
}

void firmware_console_putchar(char c) {
    arch_uart_putc(c);
}

void early_uart_init(void) {
    arch_uart_init();
}

int firmware_console_getchar(void) {
    return arch_uart_poll_getc();
}

void firmware_set_timer(uint64_t time) {
    (void)time;
}

void firmware_shutdown(void) {
    arch_local_irq_disable();
    psci_call0(PSCI_SYSTEM_OFF);
    while (1)
        __asm__ __volatile__("wfi");
}

void firmware_reboot(void) {
    arch_local_irq_disable();
    psci_call0(PSCI_SYSTEM_RESET);
    while (1)
        __asm__ __volatile__("wfi");
}

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

#endif /* CONFIG_AARCH64 */
