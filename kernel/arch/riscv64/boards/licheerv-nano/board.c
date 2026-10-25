#ifdef CONFIG_BOARD_LICHEERV_NANO

/*
 * Sipeed LicheeRV Nano board support (SoC: SophGo SG2002).
 *
 * Boot chain: the SG2002 boot ROM loads a vendor FSBL (cv181x.bin) from the
 * TF card, which initialises clocks and DRAM and loads OpenSBI; OpenSBI then
 * hands S-mode to the kernel image.  mainline U-Boot has a board target for
 * this part (configs/sipeed_licheerv_nano_defconfig) and mainline OpenSBI supports it
 * via fw_dynamic, so the SBI interfaces the kernel already uses -- timer,
 * console, SRST -- are the same ones QEMU virt uses.
 *
 * DRAM starts at 0x80000000, which is where the riscv64 arch defaults already
 * point, so this board needs no linker script override.
 *
 * The CPU is a T-Head C906, and upstream describes it as rv64imafdc with
 * mmu-type riscv,sv39.  That is exactly the -march=rv64imafdc_zicsr_zifencei
 * the riscv64 build already uses. The UART still requires 32-bit accesses at
 * four-byte register strides, unlike QEMU's byte-stride NS16550, and U-Boot
 * booti needs the standard RISC-V Image header in arch entry.S.
 *
 * Interrupt controller: the DTS calls it "thead,c900-plic", which mainline
 * binds to its standard SiFive PLIC driver, so the shared PLIC in
 * kernel/drivers/irqchip/plic.c applies unchanged.
 *
 * Verified: builds clean under -Werror.  NOT run on hardware.  See
 * docs/platforms/licheerv-nano.md for what has to happen before that
 * statement can be made.
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

/* SG2002 oscillator, as published by /cpus timebase-frequency in the upstream
 * device tree.  arch_timer_frequency() prefers the firmware DTB and falls back
 * to this. */
#define LRN_TIMER_FREQ 25000000UL

static uint64_t lrn_plic_hart_id(void) {
    return arch_cpu_hart_id(cpu_current_id());
}

static uint64_t lrn_timer_read_ticks(void) {
    return timer_get_ticks();
}

static uint64_t lrn_timer_ticks_per_sec(void) {
    uint64_t freq = riscv64_fdt_timebase_freq();
    return freq ? freq : LRN_TIMER_FREQ;
}

static const timer_ops_t lrn_timer_ops = {
    .read_ticks    = lrn_timer_read_ticks,
    .ticks_per_sec = lrn_timer_ticks_per_sec,
};

static void lrn_early_init(void) {
    plic_configure(PLIC_BASE, lrn_plic_hart_id);
    riscv64_memory_init();
}

static void lrn_poweroff(void) {
    sbi_shutdown();
}

static void lrn_reboot(void) {
    sbi_reboot();
}

/* The SDHCI driver binds the canonical DT node; this hook supplies only board
 * clock/pin/PHY mechanisms. Ethernet remains unbound until a matching MAC
 * driver is implemented. */
void licheerv_nano_configure_storage(void);

static const board_config_t licheerv_nano = {
    .name              = "licheerv-nano",
    .ram_base          = PHYS_MEMORY_BASE,
    .ram_end           = PHYS_MEMORY_END,
    .irqchip           = &plic_irqchip_ops,
    .timer             = &lrn_timer_ops,
    /* SG2002 has a second C906 core, but the upstream device tree declares
     * only cpu0, so no secondary-start contract is claimed here.  See
     * docs/platforms/licheerv-nano.md. */
    .smp               = NULL,
    .early_init        = lrn_early_init,
    .poweroff          = lrn_poweroff,
    .reboot            = lrn_reboot,
    .configure_devices = licheerv_nano_configure_storage,
};

const board_config_t *const current_board = &licheerv_nano;

#endif /* CONFIG_BOARD_LICHEERV_NANO */
