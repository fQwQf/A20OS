#include "firmware/devicetree.h"
#include "boot/context.h"
#include "core/panic.h"
#include "core/errno.h"
#include "drivers/core/driver_core.h"
#include "mm/frame.h"
#include "platform.h"

/* Boards with stricter identity/topology checks may provide their own ranges.
 * All other DT handoffs still preserve the DTB and firmware reservations. */
static fw_dt_range_t firmware_ranges[PFA_MAX_RANGES];
static size_t firmware_range_count;
static int memory_ready;
static void firmware_memory_init(void) {
    if (memory_ready) return;
    const boot_context_t *b = boot_context_get();
    uint64_t base = current_board ? current_board->ram_base : PHYS_MEMORY_BASE;
    uint64_t end = current_board ? current_board->ram_end : PHYS_MEMORY_END;
    firmware_ranges[0] = (fw_dt_range_t){base, end - base};
    firmware_range_count = 1;
    if (b && b->fdt) {
        fw_dt_t tree; extern char _start[], _bss_end[];
        int rc = fw_dt_init(&tree, b->fdt, b->fdt_available);
        if (!rc) rc = fw_dt_memory_window(&tree, b->fdt_physical, base, end,
            (uintptr_t)_start - PAGE_OFFSET, (uintptr_t)_bss_end - PAGE_OFFSET,
            firmware_ranges, PFA_MAX_RANGES, &firmware_range_count, 0);
        if (rc) panic("RISC-V32: invalid or excessive DT RAM/reservations");
    }
    memory_ready = 1;
}
size_t arch_ram_range_count(void) {
    firmware_memory_init();
    return firmware_range_count;
}
int arch_ram_range(size_t index, paddr_t *base, paddr_t *end) {
    firmware_memory_init();
    if (!base || !end || index >= firmware_range_count) return -EINVAL;
    *base = firmware_ranges[index].base;
    *end = firmware_ranges[index].base + firmware_ranges[index].size;
    return 0;
}

#ifdef CONFIG_RISCV32
#include "core/bootargs.h"
#include "../../common/riscv/fdt.h"
int riscv32_fdt_has_isa_extension(const char *extension) {
    return riscv_firmware_extension(extension);
}
const char *arch_bootargs_get(void) {
    const boot_context_t *b = boot_context_get();
    return b ? b->command_line : NULL;
}
#endif
