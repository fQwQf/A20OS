#include "arch/common/sunxi_mmc0.h"
#include "core/arch.h"
#include "firmware/devicetree.h"
#include "boot/context.h"
#include "core/errno.h"
#include "mm/frame.h"
#include "core/panic.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "firmware.h"
#include "drivers/core/driver_core.h"


/* Lichee Pi Zero V3s: UART, GICv2, generic timer, firmware RAM and watchdog.
 * SD uses the shared sunxi PIO engine; no EMAC/USB/display driver is claimed. */
extern char _start[], _end[];
static fw_dt_range_t memory_ranges[PFA_MAX_RANGES];
static size_t memory_count;

static void v3s_early_init(void) {
    if (arch_smp_boot_hw_id() != 0)
        panic("V3s: firmware must boot hardware CPU0");
    const boot_context_t *b = boot_context_get();
    fw_dt_t tree; fw_node_t root;
    if (!b || !b->fdt || fw_dt_init(&tree, b->fdt, b->fdt_available) ||
        fw_dt_root(&tree, &root) || root.ops->match_id(&root, "licheepi,licheepi-zero") != 1)
        panic("V3s: missing or mismatched firmware tree");
    uint64_t first = KERNEL_ENTRY;
    uint64_t last = (uintptr_t)_end - PAGE_OFFSET;
    if (fw_dt_memory_window(&tree, b->fdt_physical, PHYS_MEMORY_BASE, PHYS_MEMORY_END,
                            first, last, memory_ranges, PFA_MAX_RANGES, &memory_count, 0))
        panic("V3s: invalid or excessive DT RAM/reservations");
    uint64_t available = 0;
    for (size_t i = 0; i < memory_count; i++) {
        uint64_t end = memory_ranges[i].base + memory_ranges[i].size;
        uint64_t start = memory_ranges[i].base > last ? memory_ranges[i].base : last;
        if (end > start) available += end - start;
    }
    if (available < 0x400000 || !ARCH_TIMER_FREQ)
        panic("V3s: insufficient usable RAM or missing counter frequency");
    printf("[V3s] allocator RAM=%lu ranges counter=%lu Hz (single CPU)\n",
           (unsigned long)memory_count, (unsigned long)ARCH_TIMER_FREQ);
}
size_t v3s_ram_range_count(void) { return memory_count; }
int v3s_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx >= memory_count || !base || !end) return -EINVAL;
    *base = memory_ranges[idx].base;
    *end = memory_ranges[idx].base + memory_ranges[idx].size;
    return 0;
}
const char *arch_bootargs_get(void) {
    const boot_context_t *b = boot_context_get();
    return b ? b->command_line : NULL;
}

static uint64_t v3s_ticks(void) {
    return timer_get_ticks();
}
static uint64_t v3s_frequency(void) {
    return ARCH_TIMER_FREQ;
}
static const timer_ops_t v3s_timer = {
    .read_ticks = v3s_ticks,
    .ticks_per_sec = v3s_frequency,
};
static void v3s_irq_enable(uint32_t irq) {
    volatile uint32_t *gic = (void *)(uintptr_t)GICD_BASE;
    gic[0x100 / 4 + irq / 32] = 1U << (irq % 32);
    ((volatile uint8_t *)gic)[0x400 + irq] = 0x40;
    if (irq >= 32)
        ((volatile uint8_t *)gic)[0x800 + irq] = 1;
    arch_mb();
}
static void v3s_irq_disable(uint32_t irq) {
    volatile uint32_t *gic = (void *)(uintptr_t)GICD_BASE;
    gic[0x180 / 4 + irq / 32] = 1U << (irq % 32);
    arch_mb();
}
static uint32_t v3s_irq_ack(void) { return 0; }
static void v3s_irq_eoi(uint32_t irq) { (void)irq; }
static const irqchip_ops_t v3s_gic = {
    .enable_irq = v3s_irq_enable, .disable_irq = v3s_irq_disable,
    .ack = v3s_irq_ack, .eoi = v3s_irq_eoi,
};

/* No board power switch/PMIC: shutdown deliberately halts until power removal. */
void platform_firmware_shutdown(void) {
    printf("[V3s] halted; remove external power\n");
    arch_uart_flush();
    arch_halt();
}
void platform_firmware_reboot(void) {
    volatile uint32_t *wdt = (void *)(uintptr_t)(PAGE_OFFSET + 0x01c20ca0UL);
    wdt[0x14 / 4] = (wdt[0x14 / 4] & ~3U) | 1U;
    wdt[0x18 / 4] = 1U; /* shortest watchdog timeout, system reset enabled */
    wdt[0x10 / 4] = (0xa57U << 1) | 1U;
    arch_mb();
    arch_halt();
}
static void v3s_configure_devices(void) { sunxi_board_prepare_mmc0(0); }
static const board_config_t v3s_board = {
    .configure_devices = v3s_configure_devices,
    .name = "lichee-pi-zero",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &v3s_gic,
    .timer = &v3s_timer,
    .smp = NULL,
    .early_init = v3s_early_init,
    .poweroff = firmware_shutdown,
    .reboot = firmware_reboot,
    .defer_network_init = 1,
};
const board_config_t *const current_board = &v3s_board;
