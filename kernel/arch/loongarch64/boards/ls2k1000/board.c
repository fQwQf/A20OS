#ifdef CONFIG_BOARD_LS2K1000

/*
 * Loongson 2K1000 board support.
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
#include "core/stdio.h"
#include "core/timer.h"
#include "platform.h"
#include "../../../common/module_devices.h"

/* Loongson 2K1000 physical layout */
#define LS2K_MEMORY_BASE    PHYS_MEMORY_BASE
#define LS2K_MEMORY_END     PHYS_MEMORY_END
#define LS2K_UART0_BASE     UART0_BASE
#define LS2K_GMAC_BASE      0x40040000UL
#define LS2K_GMAC_SIZE      0x10000UL
#define LS2K_AHCI_BASE      0x400e0000UL
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

/* Known on-chip controllers supplement firmware that omits their binding.
 * The canonical physical resource is reused; no duplicate device is created. */
#ifndef CONFIG_COOPERATIVE_BOOT
static const char gmac_identity[] = "ls2k1000-gmac";
static const fw_static_property_t gmac_properties[] = {
    {"hardware-id", {gmac_identity, sizeof(gmac_identity), FW_ORDER_NATIVE}},
};
static const fw_resource_t gmac_resources[] = {
    {FW_RESOURCE_MEMORY, LS2K_GMAC_BASE, LS2K_GMAC_SIZE, IORESOURCE_MMIO_32BIT, "registers"},
};
static const fw_static_node_t gmac_nodes[] = {
    {.name="ls2k1000-static", .parent=SIZE_MAX},
    {.name="ls2k-gmac", .parent=0, .properties=gmac_properties, .property_count=1,
     .resources=gmac_resources, .resource_count=1},
};
static const fw_static_tree_t gmac_tree = {gmac_nodes, 2};
#ifdef CONFIG_STORAGE_READ_ONLY
static const char ahci_identity[] = "ls2k1000-ahci";
static const uint32_t ahci_ports = 1;
static const fw_static_property_t ahci_properties[] = {
    {"hardware-id", {ahci_identity, sizeof(ahci_identity), FW_ORDER_NATIVE}},
    {"ports-implemented", {&ahci_ports, sizeof(ahci_ports), FW_ORDER_NATIVE}},
};
static const fw_resource_t ahci_resources[] = {
    {FW_RESOURCE_MEMORY, LS2K_AHCI_BASE, LS2K_AHCI_SIZE, IORESOURCE_MMIO_32BIT, "registers"},
};
static const fw_static_node_t ahci_nodes[] = {
    {.name="ls2k1000-static", .parent=SIZE_MAX},
    {.name="ls2k-ahci-ro", .parent=0, .properties=ahci_properties, .property_count=2,
     .resources=ahci_resources, .resource_count=1},
};
static const fw_static_tree_t ahci_tree = {ahci_nodes, 2};
#endif
#endif /* !CONFIG_COOPERATIVE_BOOT */
static void ls2k_configure_devices(void) {
    device_t *device = device_find_get_by_mmio(LS2K_GMAC_BASE);
#ifdef CONFIG_COOPERATIVE_BOOT
    /* Recovery policy keeps the hardware described but prevents ownership. */
    if (device) { device->user_owned = 1; device_put(device); }
#else
    if (!device) board_firmware_static_enumerate(&gmac_tree);
    else { device->software_id = gmac_identity; device_put(device); }
#ifdef CONFIG_STORAGE_READ_ONLY
    device = device_find_get_by_mmio(LS2K_AHCI_BASE);
    if (!device) board_firmware_static_enumerate(&ahci_tree);
    else { device->software_id = ahci_identity; device_put(device); }
#endif
#endif
    /* This platform does not provide a PCI host frontend. QEMU's ECAM is not
     * a substitute for the physical Loongson configuration mechanism. */
}

static int ls2k_format_interrupts(char *buf, size_t bufsz)
{
    return snprintf(buf, bufsz,
            "           CPU0\n"
            "  0: %12lu   LS2K-LIOINTC   edge   ttyS0\n"
            "CAS: %12lu   LS2K HWI1 cascades\n"
            "SPU: %12lu   LS2K spurious cascades\n"
            "STM: %12lu   LS2K masked UART storms\n",
            (unsigned long)ls2k1000_irq_source_count(UART0_IRQ),
            (unsigned long)ls2k1000_irq_cascade_count(),
            (unsigned long)ls2k1000_irq_spurious_count(),
            (unsigned long)ls2k1000_irq_storm_count());
}

static void ls2k_prepare_scheduler(void)
{
    /* Keep the bootstrap stack non-preemptible until the first runnable task
     * exists and all one-time boot work is complete. */
    uint32_t cpucfg1 = arch_read_cpu_config(1);
    uint32_t cpucfg2 = arch_read_cpu_config(2);
    printf("[INIT] LS2K1000 CPUCFG.1=0x%x CPUCFG.2=0x%x\n",
           cpucfg1, cpucfg2);
#ifdef CONFIG_COOPERATIVE_BOOT
    printf("[INIT] LS2K1000 polling mode; timer interrupts remain disabled\n");
#else
    timer_enable();
    printf("[INIT] LS2K1000 timer interrupts enabled\n");
#endif
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
    .configure_devices = ls2k_configure_devices,
    .prepare_scheduler = ls2k_prepare_scheduler,
    .format_interrupts = ls2k_format_interrupts,
#ifdef CONFIG_COOPERATIVE_BOOT
    /* The cooperative recovery profile exists because this board's firmware
     * hands over with the timer unable to preempt, so a sleeping idle task would
     * never be woken.  idle_loop() reads this instead of testing the board
     * name.  The normal profile leaves it 0 and sleeps as usual.
     *
     * The same profile has no working UART IRQ route, so the console driver
     * polls the 16550 directly.  Both flags are read by name from the code that
     * needs them rather than tested for this board. */
    .defer_network_init = 1,
    .idle_cannot_sleep = 1,
    .uart_rx_is_polled = 1,
#endif
};

const board_config_t *const current_board = &ls2k1000;

#endif /* CONFIG_BOARD_LS2K1000 */
