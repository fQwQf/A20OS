#ifdef CONFIG_BOARD_OX64
#include "core/arch.h"
#include "core/cpu.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/core/driver_core.h"
#include "drivers/irqchip/plic.h"
#include "firmware.h"
#include "platform.h"

static uint64_t ox64_hart(void) { return arch_cpu_hart_id(cpu_current_id()); }
static uint64_t ox64_ticks(void) { return timer_get_ticks(); }
static uint64_t ox64_frequency(void) {
    uint64_t f = riscv64_fdt_timebase_freq();
    return f ? f : ARCH_TIMER_FREQ;
}
static const timer_ops_t ox64_timer = {
    .read_ticks = ox64_ticks,
    .ticks_per_sec = ox64_frequency,
};
static void ox64_init(void) {
    plic_configure(PLIC_BASE, ox64_hart);
    riscv64_memory_init();
    printf("[Ox64] D0 C906, 64 MiB PSRAM, BL808 UART3, SBI timer\n");
}
void ox64_configure_storage(void);
static void ox64_poweroff(void) { sbi_shutdown(); }
static void ox64_reboot(void) { sbi_reboot(); }
static const board_config_t ox64 = {
    .name = "ox64",
    .ram_base = PHYS_MEMORY_BASE, .ram_end = PHYS_MEMORY_END,
    .irqchip = &plic_irqchip_ops, .timer = &ox64_timer, .smp = NULL,
    .early_init = ox64_init, .poweroff = ox64_poweroff,
    .reboot = ox64_reboot,
    .configure_devices = ox64_configure_storage,
};
const board_config_t *const current_board = &ox64;
#endif
