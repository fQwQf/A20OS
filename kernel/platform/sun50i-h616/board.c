#ifdef CONFIG_BOARD_SUN50I_H616

/*
 * Allwinner H616/H618 board support: Orange Pi Zero 2, Zero 3 and Zero 2W.
 *
 * Boot chain: boot ROM -> Allwinner SPL (built from U-Boot source, no vendor
 * blob) -> TF-A BL31 -> U-Boot -> kernel.  BL31 is TF-A's sun50i_h616
 * platform, which is mainline, so unlike Rockchip and Amlogic there is no
 * vendor binary anywhere in this chain that the build cannot reproduce.  That
 * makes it the cleanest boot path of any ARM board in the tree.
 *
 * Verified: builds clean under -Werror.  NOT run on hardware.  See
 * docs/platforms/sun50i-h616.md.
 *
 * Scope: RAM, the console UART, GICv2, and PSCI power off.  The SD card
 * controller is Allwinner's own sunxi-mmc rather than a DesignWare part, so
 * there is no storage path and enumerate_devices() is empty -- see the board
 * header for why dw_sdio.c does not apply.
 */

#include "core/arch.h"
#include "core/cpu.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/core/driver_core.h"
#include "firmware.h"
#include "platform.h"

static void h616_irqchip_init(void) {
    /* GICv2, already mapped through GICD_BASE/GICC_BASE.  Nothing board-specific
     * to add: the generic trap path programs the banks. */
}

static void h616_irqchip_enable(uint32_t irq) {
    (void)irq;
}

static void h616_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t h616_irqchip_ack(void) {
    return 0;
}

static void h616_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

static const irqchip_ops_t h616_irqchip_ops = {
    .init        = h616_irqchip_init,
    .enable_irq  = h616_irqchip_enable,
    .disable_irq = h616_irqchip_disable,
    .ack         = h616_irqchip_ack,
    .eoi         = h616_irqchip_eoi,
};

static uint64_t h616_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t h616_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t h616_timer_ops = {
    .read_ticks    = h616_timer_read_ticks,
    .ticks_per_sec = h616_timer_ticks_per_sec,
};

static void h616_early_init(void) {
    h616_irqchip_init();
}

static void h616_poweroff(void) {
    firmware_shutdown();
}

static void h616_reboot(void) {
    firmware_reboot();
}

static void h616_enumerate_devices(void) {
}

static const board_config_t sun50i_h616 = {
    .name              = "sun50i-h616",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &h616_irqchip_ops,
    .timer             = &h616_timer_ops,
    /* Four Cortex-A53 exist, but which of them this bootloader publishes has not
     * been checked on hardware, and porting-guide.md requires .smp to stay NULL
     * rather than carry a fabricated ops table. */
    .smp               = NULL,
    .early_init        = h616_early_init,
    .poweroff          = h616_poweroff,
    .reboot            = h616_reboot,
    .enumerate_devices = h616_enumerate_devices,
};

const board_config_t *const current_board = &sun50i_h616;

#endif /* CONFIG_BOARD_SUN50I_H616 */