#ifdef CONFIG_BOARD_LS2K1000

/*
 * Loongson 2K1000 (龙芯 LS2K1000) board support.
 *
 * The LS2K1000 SoC boots under PMON/UEFI and exposes its peripherals at fixed
 * physical addresses in the identity-mapped (cached DMW) segment.  DDR lives
 * at physical 0x0; the GMAC (DesignWare stmmac-class) is found at 0x40040000,
 * which is the BAR0 value the on-chip PCI config window (0xfe00001800)
 * returns.  The kernel image is linked in the cached window 0x9000_0000_0000_0000,
 * i.e. physical 0x0, which matches the 2K1000 firmware load convention.
 *
 * This board is BSP-only for now (.smp = NULL): the CPU local interrupt
 * controller (CSR ECFG) is enabled so the timer and software IPIs work, but a
 * device-IRQ path through the LS2K1000 internal PIC (LioIntc/PCH-PIC) is not
 * yet implemented, so all board drivers are polled.  See
 * docs/platforms/physical-boards.md for the exact bring-up checklist.
 *
 * Reference: RocketOS (MIT), used as reference for LoongArch board bring-up
 * and the la2000 GMAC driver.  See docs/ACKNOWLEDGMENTS.md.
 */

#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/net/ls2k_gmac.h"
#include "core/arch.h"
#include "core/timer.h"
#include "platform.h"

/* Loongson 2K1000 physical layout */
#define LS2K_MEMORY_BASE    0x00000000UL
#define LS2K_MEMORY_END     0x40000000UL   /* 1 GiB DDR; narrows via FDT */
#define LS2K_UART0_BASE     0x1FE001E0UL
#define LS2K_GMAC_BASE      0x40040000UL   /* stmmac-class GMAC0 (BAR0) */
#define LS2K_GMAC_SIZE      0x10000UL
#define LS2K_TIMER_FREQ     100000000UL

static void ls2k_irqchip_init(void) {
    arch_irqchip_init();
}

static void ls2k_irqchip_enable(uint32_t irq) {
    (void)irq;
    /* Unmask the CPU-local HWI0 line only.  Device IRQs additionally need the
     * LS2K1000 internal PIC routing, which is not wired yet (see header). */
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
    .init        = ls2k_irqchip_init,
    .enable_irq  = ls2k_irqchip_enable,
    .disable_irq = ls2k_irqchip_disable,
    .ack         = ls2k_irqchip_ack,
    .eoi         = ls2k_irqchip_eoi,
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
    extern void loongarch64_memory_init(void);
    /* Without a firmware DTB the board window (0x0..0x40000000) is used. */
    loongarch64_memory_init();
}

static void ls2k_poweroff(void) {
    /* Real hardware power-off needs the 2K1000 PWRCTRL/PMON path; halt the
     * CPU as the safe fallback. */
    arch_halt();
}

static void ls2k_reboot(void) {
    arch_halt();
}

static void ls2k_enumerate_devices(void) {
    extern int platform_device_register(platform_device_t *pdev);
    static platform_device_t gmac_dev;
    static resource_t gmac_res[1];

    gmac_res[0].type  = RES_MMIO;
    gmac_res[0].start = LS2K_GMAC_BASE;
    gmac_res[0].end   = LS2K_GMAC_BASE + LS2K_GMAC_SIZE - 1;
    gmac_res[0].flags = IORESOURCE_MMIO_32BIT;

    gmac_dev.dev.name       = "ls2k-gmac";
    gmac_dev.dev.res        = gmac_res;
    gmac_dev.dev.res_count  = 1;
    gmac_dev.dev.state      = DEV_STATE_UNINIT;
    gmac_dev.id.vendor      = LS2K_GMAC_PLATFORM_VENDOR;
    gmac_dev.id.device      = LS2K_GMAC_PLATFORM_DEVICE;
    platform_device_register(&gmac_dev);

    /* NOTE: no PCI enumeration here.  The QEMU-virt ECAM at 0x20000000 does
     * not exist on the physical 2K1000; its PCI config space is accessed
     * through the Loongson config window (e.g. GMAC0 at 0xfe00001800).  A
     * real PCI host shim must be added before pci_enumerate() is usable. */
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
