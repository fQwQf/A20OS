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
    if (memory_ready)
        return;
    const boot_context_t *b = boot_context_get();
    uint64_t base = current_board ? current_board->ram_base : PHYS_MEMORY_BASE;
    uint64_t end = current_board ? current_board->ram_end : PHYS_MEMORY_END;
    firmware_ranges[0] = (fw_dt_range_t){base, end - base};
    firmware_range_count = 1;
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    if (!b || b->protocol != BOOT_PROTOCOL_UEFI)
        panic("ARM: VBox requires the versioned UEFI memory map");
    extern size_t aarch64_uefi_ram_range_count(void);
    extern int aarch64_uefi_ram_range(size_t, uint64_t *, uint64_t *);
    firmware_range_count = aarch64_uefi_ram_range_count();
    if (!firmware_range_count || firmware_range_count > PFA_MAX_RANGES)
        panic("ARM: invalid UEFI RAM range count");
    for (size_t i = 0; i < firmware_range_count; i++) {
        uint64_t start, limit;
        if (aarch64_uefi_ram_range(i, &start, &limit))
            panic("ARM: invalid UEFI RAM range");
        firmware_ranges[i] = (fw_dt_range_t){start, limit - start};
    }
    memory_ready = 1;
    return;
#endif
    if (b && b->fdt) {
        fw_dt_t tree;
        extern char _start[], _end[];
        int rc = fw_dt_init(&tree, b->fdt, b->fdt_available);
        if (!rc)
            rc = fw_dt_memory_window(
                &tree, b->fdt_physical, base, end,
                (uintptr_t)_start - PAGE_OFFSET, (uintptr_t)_end - PAGE_OFFSET,
                firmware_ranges, PFA_MAX_RANGES, &firmware_range_count, 0);
        if (rc)
            panic("ARM: invalid or excessive DT RAM/reservations");
    }
    memory_ready = 1;
}
size_t arch_firmware_ram_range_count(void) {
    firmware_memory_init();
    return firmware_range_count;
}
int arch_firmware_ram_range(size_t index, paddr_t *base, paddr_t *end) {
    firmware_memory_init();
    if (!base || !end || index >= firmware_range_count)
        return -EINVAL;
    *base = firmware_ranges[index].base;
    *end = firmware_ranges[index].base + firmware_ranges[index].size;
    return 0;
}
