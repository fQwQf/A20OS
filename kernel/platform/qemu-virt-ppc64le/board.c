#ifdef CONFIG_BOARD_QEMU_VIRT_PPC64LE

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"

static void ppc64le_irqchip_init(void) {
}

static void ppc64le_irqchip_enable(uint32_t irq) {
    (void)irq;
}

static void ppc64le_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t ppc64le_irqchip_ack(void) {
    return 0;
}

static void ppc64le_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

static const irqchip_ops_t ppc64le_irqchip_ops = {
    .init = ppc64le_irqchip_init,
    .enable_irq = ppc64le_irqchip_enable,
    .disable_irq = ppc64le_irqchip_disable,
    .ack = ppc64le_irqchip_ack,
    .eoi = ppc64le_irqchip_eoi,
};

static uint64_t ppc64le_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t ppc64le_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t ppc64le_timer_ops = {
    .read_ticks = ppc64le_timer_read_ticks,
    .ticks_per_sec = ppc64le_timer_ticks_per_sec,
};

static void ppc64le_early_init(void) {
    arch_firmware_init();
}

static void ppc64le_poweroff(void) {
    firmware_shutdown();
}

static void ppc64le_reboot(void) {
    firmware_reboot();
}

static void ppc64le_enumerate_devices(void) {
}

static const board_config_t qemu_virt_ppc64le = {
    .name = "qemu-virt-ppc64le",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &ppc64le_irqchip_ops,
    .timer = &ppc64le_timer_ops,
    .early_init = ppc64le_early_init,
    .poweroff = ppc64le_poweroff,
    .reboot = ppc64le_reboot,
    .enumerate_devices = ppc64le_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_ppc64le;

#endif /* CONFIG_BOARD_QEMU_VIRT_PPC64LE */
