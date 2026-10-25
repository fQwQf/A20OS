#ifdef CONFIG_RISCV64
#include "core/types.h"
#include "core/stdio.h"
#include "core/panic.h"
#include "core/bootargs.h"
#include "drivers/core/driver_core.h"
#include "mm/frame.h"
#include "platform.h"
#include "../../common/riscv/fdt.h"
static fw_dt_range_t ranges[PFA_MAX_RANGES];
static size_t range_count;
static int facts_ready, has_h, has_sstc;
static uint64_t frequency;
int riscv64_fdt_has_isa_extension(const char *name) {
    if (!name) return 0;
    if (!facts_ready) return riscv_firmware_extension(name);
    return !strcmp(name, "h") ? has_h : !strcmp(name, "sstc") ? has_sstc : 0;
}
uint64_t riscv64_fdt_timebase_freq(void) {
    return facts_ready ? frequency : riscv_firmware_timebase();
}
size_t arch_ram_range_count(void) { return range_count ? range_count : 1; }
int arch_ram_range(size_t i, paddr_t *base, paddr_t *end) {
    if (!base || !end || i >= arch_ram_range_count()) return -1;
    if (!range_count) { *base = PHYS_MEMORY_BASE; *end = PHYS_MEMORY_END; }
    else { *base = ranges[i].base; *end = ranges[i].base + ranges[i].size; }
    return 0;
}
void riscv64_memory_init(void) {
    const boot_context_t *b = boot_context_get();
    uint64_t base = current_board ? current_board->ram_base : PHYS_MEMORY_BASE;
    uint64_t end = current_board ? current_board->ram_end : PHYS_MEMORY_END;
    ranges[0] = (fw_dt_range_t){base, end - base}; range_count = 1;
    if (b && b->fdt) {
        fw_dt_t tree; extern char _start[], _end[];
        int rc = fw_dt_init(&tree, b->fdt, b->fdt_available);
        if (!rc) rc = fw_dt_memory_window(&tree, b->fdt_physical, base, end,
            (uintptr_t)_start - PAGE_OFFSET, (uintptr_t)_end - PAGE_OFFSET,
            ranges, PFA_MAX_RANGES, &range_count, 0);
        if (rc) panic("RISC-V: invalid or excessive DT RAM/reservations");
    }
    for (size_t i = 0; i < range_count; i++)
        printf("[FDT] RAM range 0x%lx..0x%lx (%lu MiB, firmware reservations excluded)\n",
               (unsigned long)ranges[i].base, (unsigned long)(ranges[i].base + ranges[i].size),
               (unsigned long)(ranges[i].size >> 20));
    has_h = riscv_firmware_extension("h"); has_sstc = riscv_firmware_extension("sstc");
    frequency = riscv_firmware_timebase(); facts_ready = 1;
}
const char *arch_bootargs_get(void) {
    const boot_context_t *b = boot_context_get();
    return b ? b->command_line : NULL;
}
#endif
