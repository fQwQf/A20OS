#ifdef CONFIG_RISCV32

#include "drivers/core/driver_core.h"
#include "drivers/irqchip/plic.h"
#include "core/arch.h"
#include "core/cpu.h"
#include "core/timer.h"

/* Shared PLIC body: see kernel/drivers/irqchip/plic.c.  riscv32 has no
 * arch_cpu_hart_id(), so the hart id is the logical cpu id. */
static uint64_t rv32_plic_hart_id(void) {
    return (uint64_t)cpu_current_id();
}

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
    plic_configure(PLIC_BASE, rv32_plic_hart_id);
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
    .irqchip = &plic_irqchip_ops,
    .timer = &rv32_timer_ops,
    .early_init = rv32_early_init,
    .poweroff = rv32_poweroff,
    .reboot = rv32_reboot,
    .enumerate_devices = rv32_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_rv32;

#endif /* CONFIG_RISCV32 */
