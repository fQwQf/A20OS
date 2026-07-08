#ifdef CONFIG_BOARD_QEMU_VIRT_X86_64

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"

static void x86_64_irqchip_init(void) {}

static void x86_64_irqchip_enable(uint32_t irq) {
    (void)irq;
}

static void x86_64_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t x86_64_irqchip_ack(void) {
    return 0;
}

static void x86_64_irqchip_eoi(uint32_t irq) {
    (void)irq;
    lapic_write(LAPIC_EOI, 0);
}

static void x86_64_irqchip_send_ipi(uint64_t target_mask) {
    (void)target_mask;
}

static const irqchip_ops_t x86_64_irqchip_ops = {
    .init       = x86_64_irqchip_init,
    .enable_irq = x86_64_irqchip_enable,
    .disable_irq = x86_64_irqchip_disable,
    .ack        = x86_64_irqchip_ack,
    .eoi        = x86_64_irqchip_eoi,
    .send_ipi   = x86_64_irqchip_send_ipi,
};

static uint64_t x86_64_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t x86_64_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t x86_64_timer_ops = {
    .read_ticks    = x86_64_timer_read_ticks,
    .ticks_per_sec = x86_64_timer_ticks_per_sec,
};

static void x86_64_early_init(void) {
    x86_64_enable_fpu_sse();
}

static void x86_64_poweroff(void) {
    outw(0x604, 0x2000);
    arch_halt();
}

static void x86_64_reboot(void) {
    uint8_t val;
    do {
        val = inb(0x64);
    } while (val & 0x02);
    outb(0x64, 0xFE);
    arch_halt();
}

extern void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);

static void x86_64_enumerate_devices(void) {
    pci_enumerate(PCI_ECAM_BASE, 0, 255);
}

static const board_config_t qemu_virt_x86_64 = {
    .name              = "qemu-virt-x86_64",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &x86_64_irqchip_ops,
    .timer             = &x86_64_timer_ops,
    .early_init        = x86_64_early_init,
    .poweroff          = x86_64_poweroff,
    .reboot            = x86_64_reboot,
    .enumerate_devices = x86_64_enumerate_devices,
};

const board_config_t *const current_board = &qemu_virt_x86_64;

#endif
