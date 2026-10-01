#ifdef CONFIG_BOARD_MILK_V_DUO

/*
 * Milk-V Duo board support (SoC: SophGo CV1800B).
 *
 * Boot chain: the CV1800B boot ROM loads a vendor FSBL from the TF card,
 * which initialises clocks and DRAM and loads OpenSBI; OpenSBI then hands
 * S-mode to the kernel.  mainline U-Boot has a board target
 * (configs/milkv_duo_defconfig) and upstream Linux carries
 * arch/riscv/boot/dts/sophgo/cv1800b-milkv-duo.dts, so the SBI interfaces the
 * kernel already uses are the same ones QEMU virt uses.
 *
 * DRAM starts at 0x80000000, which is where the riscv64 arch defaults already
 * point, so this board needs no linker script override.
 *
 * The core is a T-Head C906: upstream describes it as rv64imafdc with
 * mmu-type riscv,sv39, matching the riscv64 build's
 * -march=rv64imafdc_zicsr_zifencei exactly.
 *
 * Interrupt controller: the DTS calls it "thead,c900-plic", which mainline
 * binds to its standard SiFive PLIC driver, so the shared PLIC in
 * kernel/drivers/irqchip/plic.c applies unchanged.
 *
 * RAM is the constraint on this board, not the kernel: the Duo ships 64 MiB
 * of SIP DDR2 and U-Boot reports "DRAM: 63.3 MiB".  Build it BRINGUP=1 with
 * RAMFS_USER=1 and swap off; a full apk world does not fit.  The 256 MiB
 * LicheeRV Nano (same IP, same boot chain) is the better first target.
 *
 * Verified: builds clean under -Werror.  NOT run on hardware.  See
 * docs/platforms/milk-v-duo.md.
 */

#include "core/arch.h"
#include "core/cpu.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/core/driver_core.h"
#include "drivers/irqchip/plic.h"
#include "firmware.h"
#include "platform.h"

/* CV1800B oscillator, as published by /cpus timebase-frequency upstream. */
#define MVD_TIMER_FREQ 25000000UL

static uint64_t mvd_plic_hart_id(void) {
    return arch_cpu_hart_id(cpu_current_id());
}

static uint64_t mvd_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t mvd_timer_ticks_per_sec(void) {
    uint64_t freq = riscv64_fdt_timebase_freq();
    return freq ? freq : MVD_TIMER_FREQ;
}

static const timer_ops_t mvd_timer_ops = {
    .read_ticks    = mvd_timer_read_ticks,
    .ticks_per_sec = mvd_timer_ticks_per_sec,
};

static void mvd_early_init(void) {
    plic_configure(PLIC_BASE, mvd_plic_hart_id);
    riscv64_memory_init();
}

static void mvd_poweroff(void) {
    sbi_shutdown();
}

static void mvd_reboot(void) {
    sbi_reboot();
}

/* See the LicheeRV Nano board file: the DWCMSH controller at 0x04310000 and
 * the "snps,dwmac-3.70a" MAC at 0x04070000 both need drivers that do not
 * exist yet.  The Milk-V Duo's Ethernet PHY also needs an external transformer
 * and RJ45, which the board does not populate. */
static void mvd_enumerate_devices(void) {
}

static const board_config_t milk_v_duo = {
    .name              = "milk-v-duo",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &plic_irqchip_ops,
    .timer             = &mvd_timer_ops,
    /* The CV1800B's second C906 is not described in the upstream device tree,
     * so no secondary-start contract is claimed. */
    .smp               = NULL,
    .early_init        = mvd_early_init,
    .poweroff          = mvd_poweroff,
    .reboot            = mvd_reboot,
    .enumerate_devices = mvd_enumerate_devices,
};

const board_config_t *const current_board = &milk_v_duo;

#endif /* CONFIG_BOARD_MILK_V_DUO */
