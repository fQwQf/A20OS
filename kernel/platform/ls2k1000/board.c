#ifdef CONFIG_BOARD_LS2K1000

/*
 * Loongson 2K1000 (龙芯 LS2K1000) board support.
 * Porting reference: RocketOS (MIT), used as a reference for the LoongArch
 * board bring-up and driver development.  See docs/ACKNOWLEDGMENTS.md.
 */

#include "drivers/core/driver_core.h"
#include "core/arch.h"
#include "core/timer.h"

/* Loongson 2K1000 addresses */
#define LS2K_MEMORY_BASE    0x00000000UL
#define LS2K_MEMORY_END     0x20000000UL
#define LS2K_UART0_BASE     0x1FE001E0UL
#define LS2K_PCIE_ECAM_BASE 0x20000000UL
#define LS2K_PCIE_BUS_START 0
#define LS2K_PCIE_BUS_END   16
#define LS2K_TIMER_FREQ     100000000UL

static void ls2k_irqchip_init(void) {
    arch_irqchip_init();
}

static void ls2k_irqchip_enable(uint32_t irq) {
    (void)irq;
    arch_irqchip_enable();
}

static void ls2k_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t ls2k_irqchip_ack(void) {
    return 0;
}

static void ls2k_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

static const irqchip_ops_t ls2k_irqchip_ops = {
    .init       = ls2k_irqchip_init,
    .enable_irq = ls2k_irqchip_enable,
    .disable_irq = ls2k_irqchip_disable,
    .ack        = ls2k_irqchip_ack,
    .eoi        = ls2k_irqchip_eoi,
};

static uint64_t ls2k_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t ls2k_timer_ticks_per_sec(void) {
    return LS2K_TIMER_FREQ;
}

static const timer_ops_t ls2k_timer_ops = {
    .read_ticks    = ls2k_timer_read_ticks,
    .ticks_per_sec = ls2k_timer_ticks_per_sec,
};

static void ls2k_early_init(void) {
}

static void ls2k_poweroff(void) {
    arch_halt();
}

static void ls2k_reboot(void) {
    arch_halt();
}

extern void pci_enumerate(uintptr_t ecam_base, int bus_start, int bus_end);

/* Loongson 2K1000 GMAC base (from PCI BAR0, in uncached region) */
#define LS2K_GMAC_BASE      0x40040000UL
#define LS2K_GMAC_SIZE      0x10000UL

static void ls2k_enumerate_devices(void) {
    extern int device_register(device_t *dev);
    static device_t gmac_dev;
    static resource_t gmac_res[2];

    gmac_res[0].type  = RES_MMIO;
    gmac_res[0].start = LS2K_GMAC_BASE;
    gmac_res[0].end   = LS2K_GMAC_BASE + LS2K_GMAC_SIZE - 1;
    gmac_res[0].flags = IORESOURCE_MMIO_32BIT;

    gmac_dev.name       = "ls2k-gmac";
    gmac_dev.bus        = NULL;
    gmac_dev.res        = gmac_res;
    gmac_dev.res_count  = 1;
    gmac_dev.state      = DEV_STATE_UNINIT;
    device_register(&gmac_dev);

    pci_enumerate(LS2K_PCIE_ECAM_BASE, LS2K_PCIE_BUS_START, LS2K_PCIE_BUS_END);
}

static const board_config_t ls2k1000 = {
    .name              = "ls2k1000",
    .ram_base          = LS2K_MEMORY_BASE,
    .ram_end           = LS2K_MEMORY_END,
    .irqchip           = &ls2k_irqchip_ops,
    .timer             = &ls2k_timer_ops,
    .early_init        = ls2k_early_init,
    .poweroff          = ls2k_poweroff,
    .reboot            = ls2k_reboot,
    .enumerate_devices = ls2k_enumerate_devices,
};

const board_config_t *const current_board = &ls2k1000;

#endif /* CONFIG_BOARD_LS2K1000 */
