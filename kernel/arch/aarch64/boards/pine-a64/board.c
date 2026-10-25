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
#include "gicv2_device.h"
#include "smp_boot.h"

/* Initial physical bring-up: UART, GICv2, counter, firmware RAM and PSCI.
 * SD uses the shared sunxi PIO engine; no EMAC/USB/display driver is claimed. */
extern char _start[], _end[];
static fw_dt_range_t memory_ranges[PFA_MAX_RANGES];
static size_t memory_count;

static void pine_a64_early_init(void) {
    if (arch_smp_boot_hw_id() != 0)
        panic("PINE A64: firmware must boot hardware CPU0");
    const boot_context_t *b = boot_context_get();
    fw_dt_t tree; fw_node_t root;
    if (!b || !b->fdt || fw_dt_init(&tree, b->fdt, b->fdt_available) ||
        fw_dt_root(&tree, &root) || root.ops->match_id(&root, "pine64,pine64") != 1)
        panic("PINE A64: missing or mismatched firmware tree");
    uint64_t first = (uintptr_t)_start - PAGE_OFFSET;
    uint64_t last = (uintptr_t)_end - PAGE_OFFSET;
    if (fw_dt_memory_window(&tree, b->fdt_physical, PHYS_MEMORY_BASE, PHYS_MEMORY_END,
                            first, last, memory_ranges, PFA_MAX_RANGES, &memory_count, 0))
        panic("PINE A64: invalid or excessive DT RAM/reservations");
    uint64_t available = 0;
    for (size_t i = 0; i < memory_count; i++) {
        uint64_t end = memory_ranges[i].base + memory_ranges[i].size;
        uint64_t start = memory_ranges[i].base > last ? memory_ranges[i].base : last;
        if (end > start) available += end - start;
    }
    if (available < 0x1000000 || !aarch64_timer_freq())
        panic("PINE A64: insufficient usable RAM or missing counter frequency");
    printf("[PINE A64] allocator RAM=%lu ranges counter=%lu Hz (single CPU)\n",
           (unsigned long)memory_count, (unsigned long)aarch64_timer_freq());
}
size_t pine_a64_ram_range_count(void) { return memory_count; }
int pine_a64_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx >= memory_count || !base || !end) return -EINVAL;
    *base = memory_ranges[idx].base;
    *end = memory_ranges[idx].base + memory_ranges[idx].size;
    return 0;
}
const char *arch_bootargs_get(void) {
    const boot_context_t *b = boot_context_get();
    return b ? b->command_line : NULL;
}

static uint64_t pine_a64_ticks(void) {
    return timer_get_ticks();
}
static uint64_t pine_a64_frequency(void) {
    return aarch64_timer_freq();
}
static const timer_ops_t pine_a64_timer = {
    .read_ticks = pine_a64_ticks,
    .ticks_per_sec = pine_a64_frequency,
};
static void pine_a64_configure_devices(void) { sunxi_board_prepare_mmc0(1); }
static const board_config_t pine_a64_board = {
    .configure_devices = pine_a64_configure_devices,
    .name = "pine-a64",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &aarch64_gicv2_ops,
    .timer = &pine_a64_timer,
    .smp = NULL,
    .early_init = pine_a64_early_init,
    .poweroff = firmware_shutdown,
    .reboot = firmware_reboot,
    .defer_network_init = 1,
    /* The generic vDSO does not filter A64 counter glitches. */
    .vdso_user_timer_unreliable = 1,
};
const board_config_t *const current_board = &pine_a64_board;
