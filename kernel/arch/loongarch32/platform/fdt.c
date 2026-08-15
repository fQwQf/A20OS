#ifdef CONFIG_LOONGARCH32

#include "core/types.h"
#include "core/stdio.h"
#include "platform.h"
#include "drivers/core/driver_core.h"

/*
 * The NaiLoong SoC publishes no device tree.  RAM is the fixed 512 MiB
 * window at PHYS_MEMORY_BASE; bootargs are not supplied by firmware.
 */

#define LA_RAM_RANGES   4

typedef struct {
    paddr_t base;
    paddr_t end;
} la_ram_range_t;

static la_ram_range_t ram_ranges[LA_RAM_RANGES] = {
    { PHYS_MEMORY_BASE, PHYS_MEMORY_END },
};
static size_t ram_range_count = 1;

size_t arch_ram_range_count(void)
{
    return ram_range_count;
}

int arch_ram_range(size_t idx, paddr_t *base, paddr_t *end)
{
    if (!base || !end || idx >= ram_range_count)
        return -1;
    *base = ram_ranges[idx].base;
    *end = ram_ranges[idx].end;
    return 0;
}

const char *arch_bootargs_get(void)
{
    return NULL;
}

void loongarch32_memory_init(void)
{
    if (current_board &&
        current_board->ram_end > current_board->ram_base) {
        ram_ranges[0].base = current_board->ram_base;
        ram_ranges[0].end = current_board->ram_end;
        ram_range_count = 1;
    }

    printf("[FDT] LoongArch32 memory window 0x%lx..0x%lx (%lu MiB)\n",
           (unsigned long)ram_ranges[0].base,
           (unsigned long)ram_ranges[0].end,
           (unsigned long)((ram_ranges[0].end - ram_ranges[0].base) >> 20));
}

#endif
