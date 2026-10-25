#include "core/arch.h"
#include "core/fdt_view.h"
#include "core/panic.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "firmware.h"
#include "gicv2_device.h"
#include "smp_boot.h"

/* Initial physical bring-up: UART, GICv2, counter, firmware RAM and PSCI.
 * No sunxi MMC, EMAC, USB, display or secondary-core driver is claimed. */
extern uint64_t aarch64_boot_dtb;
extern char _start[], _end[];
static paddr_t allocator_end;
static char command_line[1024];

static uint64_t read_cells(const uint8_t *data, unsigned count) {
    return count == 2 ? fdt_u64(data) : fdt_u32(data);
}

static void reserve_prefix(uint64_t address, uint64_t size) {
    uint64_t first = (uintptr_t)_start - PAGE_OFFSET;
    uint64_t last = (uintptr_t)_end - PAGE_OFFSET;
    if (!size || address + size < address)
        panic("PINE A64: invalid firmware reservation");
    if (address < last && address + size > first)
        panic("PINE A64: firmware reservation overlaps kernel");
    /* The allocator supports one continuous range. Conservatively discard
     * all RAM above the first reservation rather than free firmware pages. */
    if (address >= last && address < allocator_end)
        allocator_end = address & ~(uint64_t)4095;
}

static void pine_a64_early_init(void) {
    if (arch_smp_boot_hw_id() != 0)
        panic("PINE A64: firmware must boot hardware CPU0");
    uint64_t pa = aarch64_boot_dtb;
    fdt_view_t v;
    if (!pa || (pa & 7) || pa < PHYS_MEMORY_BASE || pa >= PHYS_MEMORY_END ||
        fdt_view_open(&v, (void *)(uintptr_t)(pa + PAGE_OFFSET),
                      PHYS_MEMORY_END - pa < 0x200000 ? PHYS_MEMORY_END - pa
                                                     : 0x200000) != 0)
        panic("PINE A64: missing or invalid firmware DTB");

    unsigned ac = 2, sc = 1;
    int memory = 0, chosen = 0, compatible = 0, memory_seen = 0;
    allocator_end = PHYS_MEMORY_END;
    fdt_cursor_t cursor;
    fdt_event_t e;
    fdt_cursor_init(&cursor, &v);
    while (fdt_cursor_next(&cursor, &e) > 0) {
        if (e.type == FDT_NODE_BEGIN && e.depth == 1) {
            memory = strcmp(e.name, "memory") == 0 ||
                     strncmp(e.name, "memory@", 7) == 0;
            chosen = strcmp(e.name, "chosen") == 0;
        } else if (e.type == FDT_PROPERTY) {
            if (e.depth == 0) {
                if (strcmp(e.name, "compatible") == 0)
                    compatible = fdt_string_list_has(&e, "pine64,pine64");
                if (strcmp(e.name, "#address-cells") == 0 && e.size == 4)
                    ac = fdt_u32(e.data);
                if (strcmp(e.name, "#size-cells") == 0 && e.size == 4)
                    sc = fdt_u32(e.data);
                if ((ac != 1 && ac != 2) || (sc != 1 && sc != 2))
                    panic("PINE A64: unsupported memory cell widths");
            }
            if (memory && e.depth == 1 && strcmp(e.name, "reg") == 0) {
                unsigned step = (ac + sc) * 4;
                if (!e.size || e.size % step)
                    panic("PINE A64: invalid RAM reg");
                for (size_t i = 0; i < e.size; i += step) {
                    uint64_t base = read_cells(e.data + i, ac);
                    uint64_t size = read_cells(e.data + i + 4 * ac, sc);
                    if (!size || base + size < base)
                        panic("PINE A64: invalid RAM size");
                    if (base == PHYS_MEMORY_BASE) {
                        if (memory_seen++)
                            panic("PINE A64: duplicate RAM base");
                        if (base + size < allocator_end)
                            allocator_end = (base + size) & ~(uint64_t)4095;
                    }
                }
            }
            if (chosen && e.depth == 1 && strcmp(e.name, "bootargs") == 0) {
                if (!e.size || e.size > sizeof(command_line) ||
                    e.data[e.size - 1])
                    panic("PINE A64: invalid command line");
                memcpy(command_line, e.data, e.size);
            }
        } else if (e.type == FDT_NODE_END && e.depth == 1) {
            memory = chosen = 0;
        }
    }
    if (!compatible || !memory_seen || pa + v.size > allocator_end)
        panic("PINE A64: DTB must describe PINE A64 RAM");

    /* Apply reservations after finding RAM, independent of DT node order. */
    unsigned res_ac = ac, res_sc = sc;
    int reserved = 0;
    fdt_cursor_init(&cursor, &v);
    while (fdt_cursor_next(&cursor, &e) > 0) {
        if (e.type == FDT_NODE_BEGIN && e.depth == 1)
            reserved = strcmp(e.name, "reserved-memory") == 0;
        else if (e.type == FDT_NODE_END && e.depth == 1)
            reserved = 0;
        else if (reserved && e.type == FDT_PROPERTY) {
            if (e.depth == 1) {
                if (strcmp(e.name, "#address-cells") == 0 && e.size == 4)
                    res_ac = fdt_u32(e.data);
                if (strcmp(e.name, "#size-cells") == 0 && e.size == 4)
                    res_sc = fdt_u32(e.data);
                if ((res_ac != 1 && res_ac != 2) ||
                    (res_sc != 1 && res_sc != 2))
                    panic("PINE A64: unsupported reservation cell widths");
                if (strcmp(e.name, "ranges") == 0 && e.size)
                    panic("PINE A64: translated reservations unsupported");
            } else if (e.depth == 2 && strcmp(e.name, "reg") == 0) {
                unsigned step = 4 * (res_ac + res_sc);
                if (!e.size || e.size % step)
                    panic("PINE A64: invalid reserved-memory reg");
                for (size_t i = 0; i < e.size; i += step)
                    reserve_prefix(read_cells(e.data + i, res_ac),
                                   read_cells(e.data + i + 4 * res_ac, res_sc));
            }
        }
    }
    reserve_prefix(pa, v.size);
    for (size_t i = 0;; i++) {
        uint64_t address, size;
        int rc = fdt_view_reserve(&v, i, &address, &size);
        if (rc <= 0)
            break;
        reserve_prefix(address, size);
    }
    if (allocator_end < (uintptr_t)_end - PAGE_OFFSET + 0x1000000 ||
        !aarch64_timer_freq())
        panic("PINE A64: insufficient free RAM or missing counter frequency");
    printf("[PINE A64] allocator RAM=0x%lx..0x%lx counter=%lu Hz (single CPU)\n",
           (unsigned long)PHYS_MEMORY_BASE, (unsigned long)allocator_end,
           (unsigned long)aarch64_timer_freq());
}

int pine_a64_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx || !base || !end || !allocator_end)
        return -1;
    *base = PHYS_MEMORY_BASE;
    *end = allocator_end;
    return 0;
}

const char *arch_bootargs_get(void) {
    return command_line;
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
static const board_config_t pine_a64_board = {
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
