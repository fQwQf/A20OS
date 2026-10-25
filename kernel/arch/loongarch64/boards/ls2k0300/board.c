#ifdef CONFIG_BOARD_LS2K0300

#include "core/arch.h"
#include "core/stdio.h"
#include "core/timer.h"
#include "drivers/core/driver_core.h"
#include "platform.h"
#include "liointc.h"

/* The vendor bootloader owns DDR, UART clocks, pinmux and the baud divisor. */

uint64_t ls2k0300_timer_freq(void)
{
    uint32_t base = arch_read_cpu_config(4);
    uint32_t ratio = arch_read_cpu_config(5);
    uint32_t mult = ratio & 0xffffU;
    uint32_t div = ratio >> 16;
    /* Bad CPUCFG is a hard stop; inventing a frequency corrupts deadlines. */
    if (!base || !mult || !div) {
        arch_local_irq_disable();
        for (;;) arch_cpu_relax();
    }
    return (uint64_t)base * mult / div;
}

static uint64_t read_ticks(void) { return timer_get_ticks(); }
static const timer_ops_t stable_timer = {
    .read_ticks = read_ticks, .ticks_per_sec = ls2k0300_timer_freq,
};

static void early_init(void)
{
    timer_disable();
    ls2k0300_irq_init();
    loongarch64_memory_init();
    printf("[LS2K0300] stage=irq cpu=LA264 timer_hz=%lu CPUCFG.2=0x%x\n",
           (unsigned long)ls2k0300_timer_freq(), arch_read_cpu_config(2));
}

static void halt_board(void) { arch_local_irq_disable(); arch_halt(); }
static void prepare_scheduler(void)
{
#ifdef CONFIG_COOPERATIVE_BOOT
    timer_disable();
    printf("[LS2K0300] cooperative polling selected\n");
#else
    timer_enable();
    printf("[LS2K0300] LIOINTC UART RX and core timer enabled\n");
#endif
}

static const board_config_t ls2k0300 = {
    .name = "ls2k0300",
    .ram_base = PHYS_MEMORY_BASE, .ram_end = PHYS_MEMORY_END,
    .irqchip = &ls2k0300_irqchip_ops, .timer = &stable_timer,
    .early_init = early_init, .poweroff = halt_board, .reboot = halt_board,
    .prepare_scheduler = prepare_scheduler,
    .defer_network_init = 1,
#ifdef CONFIG_COOPERATIVE_BOOT
    .idle_cannot_sleep = 1, .uart_rx_is_polled = 1,
#endif
};
const board_config_t *const current_board = &ls2k0300;
#endif
