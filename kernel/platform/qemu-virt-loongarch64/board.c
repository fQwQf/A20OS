#ifdef CONFIG_LOONGARCH64

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"

static void la64_irqchip_init(void) {
    arch_irqchip_init();
}

static void la64_irqchip_enable(uint32_t irq) {
    (void)irq;
    arch_irqchip_enable();
}

static void la64_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t la64_irqchip_ack(void) {
    return 0;
}

static void la64_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

static void la64_irqchip_send_ipi(uint64_t target_mask) {
    (void)target_mask;
}

static const irqchip_ops_t la64_irqchip_ops = {
    .init       = la64_irqchip_init,
    .enable_irq = la64_irqchip_enable,
    .disable_irq = la64_irqchip_disable,
    .ack        = la64_irqchip_ack,
    .eoi        = la64_irqchip_eoi,
    .send_ipi   = la64_irqchip_send_ipi,
};

static uint64_t la64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t la64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t la64_timer_ops = {
    .read_ticks    = la64_timer_read_ticks,
    .ticks_per_sec = la64_timer_ticks_per_sec,
};

static void la64_early_init(void) {
}

static void la64_poweroff(void) {
    *(volatile uint16_t *)(uintptr_t)VIRT_GED_SLEEP_CTL =
        (uint16_t)((VIRT_GED_SLP_TYP_S5 << VIRT_GED_SLP_TYP_POS) | VIRT_GED_SLP_EN);
    arch_halt();
}

static void la64_reboot(void) {
    *(volatile uint16_t *)(uintptr_t)VIRT_GED_RESET_REG = VIRT_GED_RESET_VAL;
    arch_halt();
}

extern void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);

static void la64_enumerate_devices(void) {
    pci_enumerate(PCIE_ECAM_BASE, PCIE_BUS_START, PCIE_BUS_END);
}

static const board_config_t qemu_virt_la64 = {
    .name              = "qemu-virt-la64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &la64_irqchip_ops,
    .timer             = &la64_timer_ops,
    .early_init        = la64_early_init,
    .poweroff          = la64_poweroff,
    .reboot            = la64_reboot,
    .enumerate_devices = la64_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_la64;

#endif /* CONFIG_LOONGARCH64 */
