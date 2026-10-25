#ifdef CONFIG_LOONGARCH64
#include "core/bootargs.h"
#include "core/panic.h"
#include "core/errno.h"
#include "core/stdio.h"
#include "boot/context.h"
#include "firmware/devicetree.h"
#include "drivers/core/driver_core.h"
#include "mm/frame.h"
#include "platform.h"
static fw_dt_range_t ranges[PFA_MAX_RANGES];
static size_t range_count;
size_t arch_ram_range_count(void) { return range_count ? range_count : 1; }
int arch_ram_range(size_t i, paddr_t *base, paddr_t *end) {
    if (!base || !end || i >= arch_ram_range_count()) return -1;
    if (!range_count) { *base = PHYS_MEMORY_BASE; *end = PHYS_MEMORY_END; }
    else { *base = ranges[i].base; *end = ranges[i].base + ranges[i].size; }
    return 0;
}
const char *arch_bootargs_get(void) {
    const boot_context_t *b = boot_context_get();
    if (b && b->command_line && b->command_line[0]) return b->command_line;
#ifdef CONFIG_UART_CMDLINE
    static char line[1024]; static int ready;
    if (!ready) { (void)uart_cmdline_read(line, sizeof(line)); ready = 1; }
    return line[0] ? line : NULL;
#else
    return NULL;
#endif
}
void loongarch64_memory_init(void) {
    uint64_t base = current_board ? current_board->ram_base : PHYS_MEMORY_BASE;
    uint64_t end = current_board ? current_board->ram_end : PHYS_MEMORY_END;
    ranges[0] = (fw_dt_range_t){base, end - base}; range_count = 1;
    const boot_context_t *b = boot_context_get();
    if (b && b->fdt) {
        fw_dt_t tree; extern char _start[], _end[];
        int rc = fw_dt_init(&tree, b->fdt, b->fdt_available);
        if (!rc) rc = fw_dt_memory_window(&tree, b->fdt_physical, base, end,
            (uintptr_t)_start - PAGE_OFFSET, (uintptr_t)_end - PAGE_OFFSET,
            ranges, PFA_MAX_RANGES, &range_count, FW_DT_MEMORY_STATIC_IF_ABSENT);
        if (rc) panic("LoongArch: invalid or excessive DT RAM/reservations");
    }
    for (size_t i = 0; i < range_count; i++)
        printf("[FDT] RAM range %lu: 0x%lx..0x%lx\n", (unsigned long)i,
            (unsigned long)ranges[i].base, (unsigned long)(ranges[i].base + ranges[i].size));
}
#endif
