#include "core/arch.h"
#include "core/fdt_view.h"
#include "core/panic.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "firmware.h"
#include "drivers/core/driver_core.h"


/* Lichee Pi Zero V3s: UART, GICv2, generic timer, firmware RAM and watchdog.
 * No sunxi MMC, EMAC, USB, display or secondary-core driver is claimed. */
extern uint32_t arm32_boot_dtb;
extern char _start[], _end[];
static paddr_t allocator_end;
static char command_line[1024];

static uint64_t read_cells(const uint8_t *data, unsigned count) {
    return count == 2 ? fdt_u64(data) : fdt_u32(data);
}

static void reserve_prefix(uint64_t address, uint64_t size) {
    uint64_t first = KERNEL_ENTRY; /* ARM32 entry section is linked physically. */
    uint64_t last = (uintptr_t)_end - PAGE_OFFSET;
    if (!size || address + size < address)
        panic("V3s: invalid firmware reservation");
    if (address < last && address + size > first)
        panic("V3s: firmware reservation overlaps kernel");
    /* The allocator supports one continuous range. Conservatively discard
     * all RAM above the first reservation rather than free firmware pages. */
    if (address >= last && address < allocator_end)
        allocator_end = address & ~(uint64_t)4095;
}

static void v3s_early_init(void) {
    if (arch_smp_boot_hw_id() != 0)
        panic("V3s: firmware must boot hardware CPU0");
    uint64_t pa = arm32_boot_dtb;
    fdt_view_t v;
    if (!pa || (pa & 7) || pa < PHYS_MEMORY_BASE || pa >= PHYS_MEMORY_END ||
        fdt_view_open(&v, (void *)(uintptr_t)(pa + PAGE_OFFSET),
                      PHYS_MEMORY_END - pa < 0x200000 ? PHYS_MEMORY_END - pa
                                                     : 0x200000) != 0)
        panic("V3s: missing or invalid firmware DTB");

    unsigned ac = 2, sc = 1;
    int memory = 0, chosen = 0, compatible = 0, memory_seen = 0;
    allocator_end = PHYS_MEMORY_END;
    fdt_cursor_t cursor;
    fdt_event_t e;
    int result;
    fdt_cursor_init(&cursor, &v);
    while ((result = fdt_cursor_next(&cursor, &e)) > 0) {
        if (e.type == FDT_NODE_BEGIN && e.depth == 1) {
            memory = strcmp(e.name, "memory") == 0 ||
                     strncmp(e.name, "memory@", 7) == 0;
            chosen = strcmp(e.name, "chosen") == 0;
        } else if (e.type == FDT_PROPERTY) {
            if (e.depth == 0) {
                if (strcmp(e.name, "compatible") == 0)
                    compatible = fdt_string_list_has(&e, "licheepi,licheepi-zero");
                if (strcmp(e.name, "#address-cells") == 0 && e.size == 4)
                    ac = fdt_u32(e.data);
                if (strcmp(e.name, "#size-cells") == 0 && e.size == 4)
                    sc = fdt_u32(e.data);
                if ((ac != 1 && ac != 2) || (sc != 1 && sc != 2))
                    panic("V3s: unsupported memory cell widths");
            }
            if (memory && e.depth == 1 && strcmp(e.name, "reg") == 0) {
                unsigned step = (ac + sc) * 4;
                if (!e.size || e.size % step)
                    panic("V3s: invalid RAM reg");
                for (size_t i = 0; i < e.size; i += step) {
                    uint64_t base = read_cells(e.data + i, ac);
                    uint64_t size = read_cells(e.data + i + 4 * ac, sc);
                    if (!size || base + size < base)
                        panic("V3s: invalid RAM size");
                    if (base == PHYS_MEMORY_BASE) {
                        if (memory_seen++)
                            panic("V3s: duplicate RAM base");
                        if (base + size < allocator_end)
                            allocator_end = (base + size) & ~(uint64_t)4095;
                    }
                }
            }
            if (chosen && e.depth == 1 && strcmp(e.name, "bootargs") == 0) {
                if (!e.size || e.size > sizeof(command_line) ||
                    e.data[e.size - 1])
                    panic("V3s: invalid command line");
                memcpy(command_line, e.data, e.size);
            }
        } else if (e.type == FDT_NODE_END && e.depth == 1) {
            memory = chosen = 0;
        }
    }
    if (result < 0)
        panic("V3s: malformed DTB structure");
    if (!compatible || !memory_seen || pa + v.size > allocator_end)
        panic("V3s: DTB must describe V3s RAM");

    /* Apply reservations after finding RAM, independent of DT node order. */
    unsigned res_ac = ac, res_sc = sc;
    int reserved = 0, child_reg = 0, child_size = 0;
    fdt_cursor_init(&cursor, &v);
    while ((result = fdt_cursor_next(&cursor, &e)) > 0) {
        if (e.type == FDT_NODE_BEGIN && e.depth == 1)
            reserved = strcmp(e.name, "reserved-memory") == 0;
        else if (reserved && e.type == FDT_NODE_BEGIN && e.depth == 2)
            child_reg = child_size = 0;
        else if (reserved && e.type == FDT_NODE_END && e.depth == 2) {
            if (child_size && !child_reg)
                panic("V3s: dynamic reserved-memory allocation unsupported");
        }
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
                    panic("V3s: unsupported reservation cell widths");
                if (strcmp(e.name, "ranges") == 0 && e.size)
                    panic("V3s: translated reservations unsupported");
            } else if (e.depth == 2 && strcmp(e.name, "reg") == 0) {
                child_reg = 1;
                unsigned step = 4 * (res_ac + res_sc);
                if (!e.size || e.size % step)
                    panic("V3s: invalid reserved-memory reg");
                for (size_t i = 0; i < e.size; i += step)
                    reserve_prefix(read_cells(e.data + i, res_ac),
                                   read_cells(e.data + i + 4 * res_ac, res_sc));
            } else if (e.depth == 2 && strcmp(e.name, "size") == 0) {
                child_size = 1;
            }
        }
    }
    if (result < 0)
        panic("V3s: malformed reservation structure");
    reserve_prefix(pa, v.size);
    for (size_t i = 0;; i++) {
        uint64_t address, size;
        int rc = fdt_view_reserve(&v, i, &address, &size);
        if (rc <= 0)
            break;
        reserve_prefix(address, size);
    }
    if (allocator_end < (uintptr_t)_end - PAGE_OFFSET + 0x400000 ||
        !ARCH_TIMER_FREQ)
        panic("V3s: insufficient free RAM or missing counter frequency");
    printf("[V3s] allocator RAM=0x%lx..0x%lx counter=%lu Hz (single CPU)\n",
           (unsigned long)PHYS_MEMORY_BASE, (unsigned long)allocator_end,
           (unsigned long)ARCH_TIMER_FREQ);
}

int v3s_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx || !base || !end || !allocator_end)
        return -1;
    *base = PHYS_MEMORY_BASE;
    *end = allocator_end;
    return 0;
}

const char *arch_bootargs_get(void) {
    return command_line;
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
static const board_config_t v3s_board = {
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
