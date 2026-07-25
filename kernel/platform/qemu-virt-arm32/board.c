#ifdef CONFIG_BOARD_QEMU_VIRT_ARM32

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/timer.h"

static inline volatile uint32_t *arm32_gicd_reg32(uint32_t off) {
    return (volatile uint32_t *)(uintptr_t)(GICD_BASE + off);
}

static inline volatile uint8_t *arm32_gicd_reg8(uint32_t off) {
    return (volatile uint8_t *)(uintptr_t)(GICD_BASE + off);
}

static void arm32_gicd_set_target(uint32_t irq, uint8_t target) {
    uint32_t off = 0x800 + (irq & ~3U);
    uint32_t shift = (irq & 3U) * 8U;
    volatile uint32_t *reg = arm32_gicd_reg32(off);
    uint32_t value = *reg;
    value = (value & ~(0xFFU << shift)) | ((uint32_t)target << shift);
    *reg = value;
}

static void arm32_irqchip_init(void) {
}

static void arm32_irqchip_enable(uint32_t irq) {
    *arm32_gicd_reg32(0x100 + (irq / 32U) * 4U) = 1U << (irq % 32U);
    *arm32_gicd_reg8(0x400 + irq) = 0x40;
    /*
     * QEMU's ARM virt GIC accepts ITARGETSR updates as word accesses.  A byte
     * store leaves the SPI pending but unroutable, so input events never reach
     * the CPU even though the VirtIO device and queue are otherwise healthy.
     */
    arm32_gicd_set_target(irq, 0x01);
    arch_mb();
}

static void arm32_irqchip_disable(uint32_t irq) {
    *arm32_gicd_reg32(0x180 + (irq / 32U) * 4U) = 1U << (irq % 32U);
    arch_mb();
}

static uint32_t arm32_irqchip_ack(void) {
    /* The ARM exception entry already claims the interrupt from the GIC. */
    return 0;
}

static void arm32_irqchip_eoi(uint32_t irq) {
    /* arch_handle_irq() completes the claimed interrupt after dispatch. */
    (void)irq;
}

static const irqchip_ops_t arm32_irqchip_ops = {
    .init = arm32_irqchip_init,
    .enable_irq = arm32_irqchip_enable,
    .disable_irq = arm32_irqchip_disable,
    .ack = arm32_irqchip_ack,
    .eoi = arm32_irqchip_eoi,
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
