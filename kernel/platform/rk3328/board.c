#ifdef CONFIG_BOARD_RK3328

/*
 * Rockchip RK3328 board support: Rock64, NanoPi R2S and siblings.
 *
 * Boot chain: Rockchip boot ROM -> SPL (DDR init from rkbin) -> TF-A BL31 ->
 * U-Boot -> kernel.  BL31 is TF-A's rk3328 platform, which is open source, so
 * the only blob is the SPL's DDR training binary.  mainline U-Boot has
 * board targets for these parts and TF-A hands S-mode to the image, so the
 * PSCI interfaces the aarch64 port already uses are the ones available here.
 *
 * Verified: builds clean under -Werror.  NOT run on hardware.  See
 * docs/platforms/rk3328.md.
 *
 * Scope of this port: RAM, the console UART, the GIC, and SBI-style power off.
 * The two devices this SoC is actually useful for -- its DesignWare eMMC
 * controller and its DWMAC Ethernet -- are declared in rk3328_platform.h but
 * not driven, and enumerate_devices() is empty because a device table entry
 * for IP nobody drives would enumerate hardware the kernel cannot talk to.
 */

#include "core/arch.h"
#include "core/cpu.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/core/driver_core.h"
#include "firmware.h"
#include "platform.h"

/* Rock64 with 2 GiB hands the kernel four cores; the NanoPi R2S has two.  The
 * board does not claim secondary_start here: which CPUs exist is a bootloader
 * and DT question that has not been checked on hardware, and
 * docs/platforms/porting-guide.md is explicit that a board which cannot start
 * secondaries must leave .smp NULL rather than supply a fake ops table. */
static const smp_platform_ops_t *const rk3328_no_smp = NULL;

static void rk3328_irqchip_init(void) {
    /* GICv2 distributor and CPU interface: both already mapped by the boot
     * page tables through GICD_BASE/GICC_BASE.  The generic trap path programs
     * the banks; there is nothing board-specific to add. */
}

static void rk3328_irqchip_enable(uint32_t irq) {
    (void)irq;
}

static void rk3328_irqchip_disable(uint32_t irq) {
    (void)irq;
}

static uint32_t rk3328_irqchip_ack(void) {
    return 0;
}

static void rk3328_irqchip_eoi(uint32_t irq) {
    (void)irq;
}

static const irqchip_ops_t rk3328_irqchip_ops = {
    .init        = rk3328_irqchip_init,
    .enable_irq  = rk3328_irqchip_enable,
    .disable_irq = rk3328_irqchip_disable,
    .ack         = rk3328_irqchip_ack,
    .eoi         = rk3328_irqchip_eoi,
};

static uint64_t rk3328_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t rk3328_timer_ticks_per_sec(void) {
    return ARCH_TIMER_FREQ;
}

static const timer_ops_t rk3328_timer_ops = {
    .read_ticks    = rk3328_timer_read_ticks,
    .ticks_per_sec = rk3328_timer_ticks_per_sec,
};

static void rk3328_early_init(void) {
    /* The CRU is deliberately not programmed here.  U-Boot enabled the console
     * UART's clocks before handing over, so the serial port works; anything
     * else on this SoC -- eMMC, GMAC -- needs CRU gate and reset writes that
     * have not been derived and cannot be checked without the board. */
    rk3328_irqchip_init();
}

static void rk3328_poweroff(void) {
    firmware_shutdown();
}

static void rk3328_reboot(void) {
    firmware_reboot();
}

static void rk3328_enumerate_devices(void) {
}

static const board_config_t rk3328 = {
    .name              = "rk3328",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &rk3328_irqchip_ops,
    .timer             = &rk3328_timer_ops,
    .smp               = rk3328_no_smp,
    .early_init        = rk3328_early_init,
    .poweroff          = rk3328_poweroff,
    .reboot            = rk3328_reboot,
    .enumerate_devices = rk3328_enumerate_devices,
};

const board_config_t *const current_board = &rk3328;

#endif /* CONFIG_BOARD_RK3328 */
