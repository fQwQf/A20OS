#ifdef CONFIG_AARCH64

#include "firmware.h"
#include "platform.h"
#include "cpu.h"
#include "console.h"
#include "core/cpu.h"
extern uint64_t aarch64_hyp_boot_cpus[CONFIG_NR_CPUS];

static int firmware_psci_uses_smc(void) {
#ifdef AARCH64_PSCI_USE_SMC
    /* Physical boards use their resident EL3 firmware even from EL1. */
    return 1;
#else
    return aarch64_hyp_boot_cpus[cpu_current_id()] == 1;
#endif
}

#define PSCI_SYSTEM_OFF 0x84000008UL
#define PSCI_SYSTEM_RESET 0x84000009UL
#define PSCI_CPU_ON 0xC4000003UL

static inline uint64_t psci_call0(uint64_t fn) {
    register uint64_t x0 __asm__("x0") = fn;
    if (firmware_psci_uses_smc())
        __asm__ __volatile__("smc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
    else
        __asm__ __volatile__("hvc #0" : "+r"(x0) : : "x1", "x2", "x3", "memory");
    return x0;
}

int64_t firmware_cpu_on(uint64_t target_cpu, uint64_t entry, uint64_t context) {
    register uint64_t x0 __asm__("x0") = PSCI_CPU_ON;
    register uint64_t x1 __asm__("x1") = target_cpu;
    register uint64_t x2 __asm__("x2") = entry;
    register uint64_t x3 __asm__("x3") = context;
    if (firmware_psci_uses_smc())
        __asm__ __volatile__("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    else
        __asm__ __volatile__("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory");
    return (int64_t)x0;
}

void firmware_console_putchar(char c) {
    arch_uart_putc(c);
}

void platform_early_console_init(void) __attribute__((weak));
void platform_firmware_shutdown(void) __attribute__((weak));
void platform_firmware_reboot(void) __attribute__((weak));

void early_uart_init(void) {
    if (platform_early_console_init)
        platform_early_console_init();
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
    if (platform_firmware_shutdown)
        platform_firmware_shutdown();
    psci_call0(PSCI_SYSTEM_OFF);
    while (1)
        __asm__ __volatile__("wfi");
}

void firmware_reboot(void) {
    arch_local_irq_disable();
    if (platform_firmware_reboot)
        platform_firmware_reboot();
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
