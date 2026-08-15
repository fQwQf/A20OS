#ifdef CONFIG_LOONGARCH32

/*
 * NaiLoong Core (LA32R) board support.
 *
 * The core is a Chisel-designed LoongArch32 Reduced out-of-order core that
 * plugs into the 龙芯杯 NSCSCC SoC via a 32-bit AXI master port.  The SoC
 * publishes DRAM at 0x80000000 (512 MiB window, identity mapped through
 * DMW0) and MMIO (UART at 0x1FE001E0) in the 0x1C000000 window (DMW1).
 *
 * The core has no IOCSR, no PCI/virtio, and no hardware page-table walker:
 * the arch layer performs a fully software TLB refill.  This board is
 * BSP-only (.smp = NULL) — secondary CPU launch has no mailbox on the
 * reference SoC.
 */

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"
#include "core/stdio.h"

static void la32_irqchip_init(void) {
    arch_irqchip_init();
}

static void la32_irqchip_enable(uint32_t irq) {
    (void)irq;
    arch_irqchip_enable();
}

static void la32_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t la32_irqchip_ack(void) {
    return 0;
}

static void la32_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

static const irqchip_ops_t la32_irqchip_ops = {
    .init        = la32_irqchip_init,
    .enable_irq  = la32_irqchip_enable,
    .disable_irq = la32_irqchip_disable,
    .ack         = la32_irqchip_ack,
    .eoi         = la32_irqchip_eoi,
};

static uint64_t la32_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t la32_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t la32_timer_ops = {
    .read_ticks    = la32_timer_read_ticks,
    .ticks_per_sec = la32_timer_ticks_per_sec,
};

static void la32_early_init(void) {
    extern void loongarch32_memory_init(void);
    loongarch32_memory_init();
}

static void la32_poweroff(void) {
    arch_halt();
}

static void la32_reboot(void) {
    arch_halt();
}

static void la32_enumerate_devices(void) {
    /* No PCI / virtio on the reference SoC; the UART console is bound by
     * uart_init() through arch_uart_* directly. */
}

static const board_config_t nailoong = {
    .name              = "nailoong",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &la32_irqchip_ops,
    .timer             = &la32_timer_ops,
    .early_init        = la32_early_init,
    .poweroff          = la32_poweroff,
    .reboot            = la32_reboot,
    .enumerate_devices = la32_enumerate_devices,
};

const board_config_t *const current_board = &nailoong;

#endif /* CONFIG_LOONGARCH32 */
