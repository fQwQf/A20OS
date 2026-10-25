#ifdef CONFIG_ARM32

#include "firmware.h"
#include "cpu.h"
#include "console.h"
#include "core/cpu.h"
extern uint32_t arm32_hyp_boot_cpus[];
static int owns_hyp(void) { return ((uint32_t *)((uintptr_t)arm32_hyp_boot_cpus + PAGE_OFFSET))[cpu_current_id()] == 1; }

#define PSCI_SYSTEM_OFF     0x84000008U
#define PSCI_SYSTEM_RESET   0x84000009U

static int32_t psci(uint32_t fn, uint32_t target, uint32_t entry, uint32_t context) {
    register uint32_t r0 __asm__("r0") = fn;
    register uint32_t r1 __asm__("r1") = target;
    register uint32_t r2 __asm__("r2") = entry;
    register uint32_t r3 __asm__("r3") = context;
    if (owns_hyp())
        __asm__ volatile(".arch_extension sec\nsmc #0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) :: "memory");
    else
        __asm__ volatile(".arch_extension virt\nhvc #0" : "+r"(r0), "+r"(r1), "+r"(r2), "+r"(r3) :: "memory");
    return (int32_t)r0;
}
static void arm32_psci_call(uint32_t fn) { (void)psci(fn, 0, 0, 0); }
int32_t firmware_cpu_on(uint32_t target, uint32_t entry, uint32_t context) {
    return psci(0x84000003, target, entry, context);
}

void platform_firmware_shutdown(void) __attribute__((weak));
void platform_firmware_reboot(void) __attribute__((weak));

void early_uart_init(void) {
    arch_uart_init();
}

void firmware_shutdown(void) {
    arch_local_irq_disable();
    if (platform_firmware_shutdown)
        platform_firmware_shutdown();
    arm32_psci_call(PSCI_SYSTEM_OFF);
    arch_halt();
}

void firmware_reboot(void) {
    arch_local_irq_disable();
    if (platform_firmware_reboot)
        platform_firmware_reboot();
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
