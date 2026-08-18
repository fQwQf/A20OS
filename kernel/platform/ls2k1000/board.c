#ifdef CONFIG_BOARD_LS2K1000

/*
 * Loongson 2K1000 (龙芯 LS2K1000) board support.
 *
 * The LS2K1000 SoC boots under PMON/UEFI and exposes its peripherals at fixed
 * physical addresses through LoongArch DMW aliases.  The GMAC
 * (DesignWare stmmac-class) is found at physical 0x40040000,
 * which is the BAR0 value the on-chip PCI config window (0xfe00001800)
 * returns.  The kernel is linked at cached VA 0x9000_0000_0200_0000,
 * the DMW alias of physical 0x0200_0000 used by the U-Boot RAM loader.
 *
 * This board remains single-core: device-IRQ bring-up starts with UART0
 * source 0 routed only to CPU0.  See docs/platforms/physical-boards.md for
 * the exact bring-up checklist.
 *
 * Reference: RocketOS (MIT), used as reference for LoongArch board bring-up
 * and the la2000 GMAC driver.  See docs/ACKNOWLEDGMENTS.md.
 */

#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/bus/platform_bus.h"
#include "drivers/block/ahci.h"
#include "drivers/net/ls2k_gmac.h"
#include "core/arch.h"
#include "core/klog.h"
#include "core/timer.h"
#include "platform.h"

/* Loongson 2K1000 physical layout */
#define LS2K_MEMORY_BASE    PHYS_MEMORY_BASE
#define LS2K_MEMORY_END     PHYS_MEMORY_END
#define LS2K_UART0_BASE     UART0_BASE
#define LS2K_GMAC_BASE      (LS2K_UNCACHED_BASE + 0x40040000UL)
#define LS2K_GMAC_SIZE      0x10000UL
#define LS2K_AHCI_BASE      (LS2K_UNCACHED_BASE + 0x400e0000UL)
#define LS2K_AHCI_SIZE      0x10000UL
#define LS2K_TIMER_FREQ     100000000UL

#define LS2K_LIO_ROUTE(source)   (LS2K_LIOINTC_BASE + (source))
#define LS2K_LIO_ENABLE_STATUS   (LS2K_LIOINTC_BASE + 0x24UL)
#define LS2K_LIO_ENABLE_SET      (LS2K_LIOINTC_BASE + 0x28UL)
#define LS2K_LIO_ENABLE_CLEAR    (LS2K_LIOINTC_BASE + 0x2cUL)
#define LS2K_LIO_POLARITY        (LS2K_LIOINTC_BASE + 0x30UL)
#define LS2K_LIO_EDGE            (LS2K_LIOINTC_BASE + 0x34UL)
#define LS2K_LIO_BANK1_OFFSET    0x40UL
#define LS2K_UART0_MASK          (1U << UART0_IRQ)
#define LS2K_UART0_ROUTE_CPU0_HWI1 0x21U
#define LS2K_UART_IRQ_BUDGET     8U

static uint64_t ls2k_irq_cascades;
static uint64_t ls2k_irq_sources[64];
static uint64_t ls2k_irq_spurious;
static uint64_t ls2k_irq_storms;

static inline volatile void *ls2k_lio_reg(uint64_t addr) {
    return (volatile void *)(uintptr_t)addr;
}

#ifndef CONFIG_COOPERATIVE_BOOT
static void ls2k_lio_ack_source(uint32_t source) {
    uint32_t bank_offset = source >= 32U ? LS2K_LIO_BANK1_OFFSET : 0U;
    uint32_t bit = 1U << (source & 31U);
    uint64_t status_addr = LS2K_LIO_ENABLE_STATUS + bank_offset;
    uint64_t clear_addr = LS2K_LIO_ENABLE_CLEAR + bank_offset;
    uint64_t set_addr = LS2K_LIO_ENABLE_SET + bank_offset;
    uint32_t was_enabled = readl(ls2k_lio_reg(status_addr)) & bit;

    /* The matching vendor driver acknowledges an edge by momentarily
     * disabling the source, then restoring it if it was enabled. */
    writel(bit, ls2k_lio_reg(clear_addr));
    if (was_enabled)
        writel(bit, ls2k_lio_reg(set_addr));
}
#endif

static void ls2k_irqchip_init(void) {
#ifndef CONFIG_COOPERATIVE_BOOT
    /* Inherit no device enables from U-Boot/vendor Linux.  Phase 2 starts
     * with UART0 only; every other LIOINTC source remains masked. */
    writel(0xffffffffU, ls2k_lio_reg(LS2K_LIO_ENABLE_CLEAR));
    writel(0xffffffffU, ls2k_lio_reg(LS2K_LIO_ENABLE_CLEAR +
                                    LS2K_LIO_BANK1_OFFSET));

    writeb(LS2K_UART0_ROUTE_CPU0_HWI1,
           ls2k_lio_reg(LS2K_LIO_ROUTE(UART0_IRQ)));
    uint32_t polarity = readl(ls2k_lio_reg(LS2K_LIO_POLARITY));
    uint32_t edge = readl(ls2k_lio_reg(LS2K_LIO_EDGE));
    writel(polarity & ~LS2K_UART0_MASK,
           ls2k_lio_reg(LS2K_LIO_POLARITY));
    writel(edge & ~LS2K_UART0_MASK, ls2k_lio_reg(LS2K_LIO_EDGE));
#endif
}

static void ls2k_irqchip_enable(uint32_t irq) {
#ifndef CONFIG_COOPERATIVE_BOOT
    if (irq == UART0_IRQ)
        writel(LS2K_UART0_MASK, ls2k_lio_reg(LS2K_LIO_ENABLE_SET));
#else
    (void)irq;
#endif
}

static void ls2k_irqchip_disable(uint32_t irq) {
#ifndef CONFIG_COOPERATIVE_BOOT
    if (irq == UART0_IRQ)
        writel(LS2K_UART0_MASK, ls2k_lio_reg(LS2K_LIO_ENABLE_CLEAR));
#else
    (void)irq;
#endif
}

static uint32_t ls2k_irqchip_ack(void) {
    return 0;
}

static void ls2k_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

void ls2k1000_handle_device_irq(void) {
#ifndef CONFIG_COOPERATIVE_BOOT
    __atomic_add_fetch(&ls2k_irq_cascades, 1, __ATOMIC_RELAXED);

    for (unsigned pass = 0; pass < LS2K_UART_IRQ_BUDGET; pass++) {
        uint32_t pending = readl(ls2k_lio_reg(LS2K_LIOINTC_CORE0_ISR));
        if (!(pending & LS2K_UART0_MASK)) {
            if (pass == 0)
                __atomic_add_fetch(&ls2k_irq_spurious, 1,
                                   __ATOMIC_RELAXED);
            return;
        }

        __atomic_add_fetch(&ls2k_irq_sources[UART0_IRQ], 1,
                           __ATOMIC_RELAXED);
        ls2k_lio_ack_source(UART0_IRQ);
        driver_irq_dispatch(UART0_IRQ);
    }

    /* A stuck source must not monopolize the CPU.  Polling remains available
     * after masking UART0, and /proc/interrupts exposes this event. */
    writel(LS2K_UART0_MASK, ls2k_lio_reg(LS2K_LIO_ENABLE_CLEAR));
    __atomic_add_fetch(&ls2k_irq_storms, 1, __ATOMIC_RELAXED);
#endif
}

uint64_t ls2k1000_irq_cascade_count(void) {
    return __atomic_load_n(&ls2k_irq_cascades, __ATOMIC_RELAXED);
}

uint64_t ls2k1000_irq_source_count(uint32_t source) {
    if (source >= 64U)
        return 0;
    return __atomic_load_n(&ls2k_irq_sources[source], __ATOMIC_RELAXED);
}

uint64_t ls2k1000_irq_spurious_count(void) {
    return __atomic_load_n(&ls2k_irq_spurious, __ATOMIC_RELAXED);
}

uint64_t ls2k1000_irq_storm_count(void) {
    return __atomic_load_n(&ls2k_irq_storms, __ATOMIC_RELAXED);
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
    /* The vendor DTB has no memory node, so the verified low bank is used. */
    loongarch64_memory_init();
    ls2k_irqchip_init();
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
#ifndef CONFIG_COOPERATIVE_BOOT
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

#ifdef CONFIG_STORAGE_READ_ONLY
    static const ahci_platform_data_t ahci_data = {
        .flags = AHCI_PLATFORM_F_READ_ONLY |
                 AHCI_PLATFORM_F_PRESERVE_FIRMWARE_LINK,
        .port_map = 1U,
    };
    static platform_device_t ahci_dev;
    static resource_t ahci_res[1];

    ahci_res[0].type  = RES_MMIO;
    ahci_res[0].start = LS2K_AHCI_BASE;
    ahci_res[0].end   = LS2K_AHCI_BASE + LS2K_AHCI_SIZE - 1;
    ahci_res[0].flags = IORESOURCE_MMIO_32BIT;

    ahci_dev.dev.name       = "ls2k-ahci-ro";
    ahci_dev.dev.plat_data  = (void *)&ahci_data;
    ahci_dev.dev.res        = ahci_res;
    ahci_dev.dev.res_count  = 1;
    ahci_dev.dev.state      = DEV_STATE_UNINIT;
    ahci_dev.id.vendor      = AHCI_PLATFORM_VENDOR;
    ahci_dev.id.device      = AHCI_PLATFORM_DEVICE;
    platform_device_register(&ahci_dev);
    kinfo("[LS2K1000] AHCI read-only polling experiment enabled\n");
#endif
#else
    /* The recovery profile deliberately leaves GMAC untouched and keeps only
     * the RAM shell plus polling console active. */
    kinfo("[LS2K1000] cooperative profile skips GMAC enumeration\n");
#endif

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
