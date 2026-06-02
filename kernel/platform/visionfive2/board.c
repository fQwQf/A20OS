#ifdef CONFIG_BOARD_VISIONFIVE2

#include "drivers/core/driver_core.h"

/* VisionFive2 addresses */
#define VF2_MEMORY_BASE   0x40000000UL
#define VF2_MEMORY_END    0x60000000UL
#define VF2_UART0_BASE    0x10000000UL
#define VF2_SDIO_BASE     0x16020000UL
#define VF2_SDIO_SIZE     0x10000UL
#define VF2_GMAC_BASE     0x16040000UL
#define VF2_GMAC_SIZE     0x10000UL
#define VF2_PLIC_BASE     0x0C000000UL
#define VF2_CLOCK_FREQ    12500000UL

static volatile uint32_t *vf2_plic_pri(uint32_t irq) {
    return (volatile uint32_t *)(uintptr_t)(VF2_PLIC_BASE + irq * 4);
}

static volatile uint32_t *vf2_plic_senable(int hart) {
    return (volatile uint32_t *)(uintptr_t)(VF2_PLIC_BASE + 0x2080 + hart * 0x100);
}

static volatile uint32_t *vf2_plic_spriority(int hart) {
    return (volatile uint32_t *)(uintptr_t)(VF2_PLIC_BASE + 0x201000 + hart * 0x2000);
}

static volatile uint32_t *vf2_plic_sclaim(int hart) {
    return (volatile uint32_t *)(uintptr_t)(VF2_PLIC_BASE + 0x201004 + hart * 0x2000);
}

static void vf2_plic_init(void) {
    int hart = 0;
    *vf2_plic_senable(hart) = 0;
    *vf2_plic_spriority(hart) = 0;
}

static void vf2_plic_enable(uint32_t irq) {
    int hart = 0;
    *vf2_plic_senable(hart) |= (1U << irq);
    *vf2_plic_pri(irq) = 1;
}

static void vf2_plic_disable(uint32_t irq) {
    int hart = 0;
    *vf2_plic_senable(hart) &= ~(1U << irq);
}

static uint32_t vf2_plic_ack(void) {
    int hart = 0;
    return *vf2_plic_sclaim(hart);
}

static void vf2_plic_eoi(uint32_t irq) {
    int hart = 0;
    *vf2_plic_sclaim(hart) = irq;
}

static void vf2_plic_send_ipi(uint64_t target_mask) {
    (void)target_mask;
}

static const irqchip_ops_t vf2_plic_ops = {
    .init       = vf2_plic_init,
    .enable_irq = vf2_plic_enable,
    .disable_irq = vf2_plic_disable,
    .ack        = vf2_plic_ack,
    .eoi        = vf2_plic_eoi,
    .send_ipi   = vf2_plic_send_ipi,
};

static void vf2_timer_init(void) {
}

static void vf2_timer_set_interval(uint64_t ticks) {
    uint64_t now;
    __asm__ volatile("csrr %0, time" : "=r"(now));
    extern void sbi_set_timer(uint64_t);
    sbi_set_timer(now + ticks);
}

static uint64_t vf2_timer_read_ticks(void) {
    uint64_t val;
    __asm__ volatile("csrr %0, time" : "=r"(val));
    return val;
}

static uint64_t vf2_timer_ticks_per_sec(void) {
    return VF2_CLOCK_FREQ;
}

static const timer_ops_t vf2_timer_ops = {
    .init          = vf2_timer_init,
    .set_interval  = vf2_timer_set_interval,
    .read_ticks    = vf2_timer_read_ticks,
    .ticks_per_sec = vf2_timer_ticks_per_sec,
};

static void vf2_early_init(void) {
}

static void vf2_poweroff(void) {
    extern void sbi_shutdown(void);
    sbi_shutdown();
}

static void vf2_reboot(void) {
    extern void sbi_reboot(void);
    sbi_reboot();
}

static void vf2_enumerate_devices(void) {
    extern int device_register(device_t *dev);
    static device_t sdio_dev;
    static resource_t sdio_res[2];

    sdio_res[0].type  = RES_MMIO;
    sdio_res[0].start = VF2_SDIO_BASE;
    sdio_res[0].end   = VF2_SDIO_BASE + VF2_SDIO_SIZE - 1;
    sdio_res[0].flags = IORESOURCE_MMIO_32BIT;

    sdio_dev.name       = "dw-sdio0";
    sdio_dev.bus        = NULL;
    sdio_dev.res        = sdio_res;
    sdio_dev.res_count  = 1;
    sdio_dev.state      = DEV_STATE_UNINIT;
    device_register(&sdio_dev);

    static device_t gmac_dev;
    static resource_t gmac_res[2];

    gmac_res[0].type  = RES_MMIO;
    gmac_res[0].start = VF2_GMAC_BASE;
    gmac_res[0].end   = VF2_GMAC_BASE + VF2_GMAC_SIZE - 1;
    gmac_res[0].flags = IORESOURCE_MMIO_32BIT;

    gmac_dev.name       = "starfive-gmac";
    gmac_dev.bus        = NULL;
    gmac_dev.res        = gmac_res;
    gmac_dev.res_count  = 1;
    gmac_dev.state      = DEV_STATE_UNINIT;
    device_register(&gmac_dev);
}

static const board_config_t visionfive2 = {
    .name              = "visionfive2",
    .ram_base          = VF2_MEMORY_BASE,
    .ram_end           = VF2_MEMORY_END,
    .irqchip           = &vf2_plic_ops,
    .timer             = &vf2_timer_ops,
    .early_init        = vf2_early_init,
    .poweroff          = vf2_poweroff,
    .reboot            = vf2_reboot,
    .enumerate_devices = vf2_enumerate_devices,
};

const board_config_t *const current_board = &visionfive2;

#endif /* CONFIG_BOARD_VISIONFIVE2 */
