#ifdef CONFIG_RISCV32

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/timer.h"

static void rv32_plic_init(void) {
    int hart = (int)cpu_current_id();
    *(volatile uint32_t *)PLIC_SENABLE(hart) = 0;
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

static void rv32_plic_enable(uint32_t irq) {
    int hart = (int)cpu_current_id();
    *(volatile uint32_t *)PLIC_SENABLE(hart) |= (1U << irq);
    *(volatile uint32_t *)(PLIC_PRIORITY + (uint32_t)irq * 4U) = 1;
}

static void rv32_plic_disable(uint32_t irq) {
    int hart = (int)cpu_current_id();
    *(volatile uint32_t *)PLIC_SENABLE(hart) &= ~(1U << irq);
}

static uint32_t rv32_plic_ack(void) {
    return 0;
}

static void rv32_plic_eoi(uint32_t irq) {
    (void)irq;
}

static void rv32_plic_send_ipi(uint64_t target_mask) {
    (void)target_mask;
}

static const irqchip_ops_t rv32_plic_ops = {
    .init = rv32_plic_init,
    .enable_irq = rv32_plic_enable,
    .disable_irq = rv32_plic_disable,
    .ack = rv32_plic_ack,
    .eoi = rv32_plic_eoi,
    .send_ipi = rv32_plic_send_ipi,
};

static uint64_t rv32_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t rv32_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t rv32_timer_ops = {
    .read_ticks = rv32_timer_read_ticks,
    .ticks_per_sec = rv32_timer_ticks_per_sec,
};

static void rv32_early_init(void) {
}

static void rv32_poweroff(void) {
    sbi_shutdown();
}

static void rv32_reboot(void) {
    sbi_reboot();
}

extern void virtio_mmio_enumerate(uintptr_t base, int max_slots, int irq_base);

static void rv32_enumerate_devices(void) {
    virtio_mmio_enumerate(VIRTIO_BASE, 8, 1);
}

static const board_config_t qemu_virt_rv32 = {
    .name = "qemu-virt-rv32",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &rv32_plic_ops,
    .timer = &rv32_timer_ops,
    .early_init = rv32_early_init,
    .poweroff = rv32_poweroff,
    .reboot = rv32_reboot,
    .enumerate_devices = rv32_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_rv32;

#endif /* CONFIG_RISCV32 */
