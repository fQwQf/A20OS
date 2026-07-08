#ifdef CONFIG_BOARD_QEMU_VIRT_ARM32

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/timer.h"

static void arm32_irqchip_init(void) {
}

static void arm32_irqchip_enable(uint32_t irq) {
    (void)irq;
}

static void arm32_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t arm32_irqchip_ack(void) {
    return arm32_gic_ack();
}

static void arm32_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

static void arm32_irqchip_send_ipi(uint64_t target_mask) {
    (void)target_mask;
}

static const irqchip_ops_t arm32_irqchip_ops = {
    .init = arm32_irqchip_init,
    .enable_irq = arm32_irqchip_enable,
    .disable_irq = arm32_irqchip_disable,
    .ack = arm32_irqchip_ack,
    .eoi = arm32_irqchip_eoi,
    .send_ipi = arm32_irqchip_send_ipi,
};

static uint64_t arm32_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t arm32_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t arm32_timer_ops = {
    .read_ticks = arm32_timer_read_ticks,
    .ticks_per_sec = arm32_timer_ticks_per_sec,
};

static void arm32_early_init(void) {
}

static void arm32_poweroff(void) {
    firmware_shutdown();
}

static void arm32_reboot(void) {
    firmware_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);

static void arm32_enumerate_devices(void) {
    virtio_mmio_enumerate(VIRTIO_BASE, 8, 48);
}

static const board_config_t qemu_virt_arm32 = {
    .name = "qemu-virt-arm32",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &arm32_irqchip_ops,
    .timer = &arm32_timer_ops,
    .early_init = arm32_early_init,
    .poweroff = arm32_poweroff,
    .reboot = arm32_reboot,
    .enumerate_devices = arm32_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_arm32;

#endif
