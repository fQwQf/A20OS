#include "core/arch.h"
#include "core/fdt_view.h"
#include "core/panic.h"
#include "core/smp.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "gicv2_device.h"
#include "smp_boot.h"

#if CONFIG_NR_CPUS > 4
#error "BCM2711 provides four Cortex-A72 CPUs"
#endif
#ifdef CONFIG_NOMMU
#error "The initial Raspberry Pi 4 port requires MMU"
#endif

extern uint64_t aarch64_boot_dtb;
extern char _start[], _end[], rpi4_spin_entry[];
/* AP firmware reads these contexts with translation off. */
struct rpi4_spin_state {
    uint64_t logical_context;
    uint64_t entry_pa;
};
struct rpi4_spin_state rpi4_spin_context[4] __attribute__((aligned(64)));
static smp_cpu_desc_t firmware_cpus[4];
static unsigned firmware_cpu_count;
static paddr_t ram_end;
static char command_line[1024];

static uint64_t cells(const uint8_t *p, unsigned n) {
    return n == 2 ? fdt_u64(p) : fdt_u32(p);
}

static void reserve_prefix(uint64_t address, uint64_t size) {
    uint64_t first = (uintptr_t)_start - PAGE_OFFSET;
    uint64_t last = (uintptr_t)_end - PAGE_OFFSET;
    if (!size || address + size < address)
        panic("RPi4: invalid firmware reservation");
    if (address < last && address + size > first)
        panic("RPi4: firmware reservation overlaps kernel");
    if (address >= last && address < ram_end)
        ram_end = address & ~(uint64_t)4095;
}

/* Parse firmware facts once before pfa_init. No guessed RAM size or implicit
 * CPU topology fallback; immutable copied descriptors survive DTB reuse. */
static void rpi4_early_init(void) {
    if (arch_smp_boot_hw_id() != 0)
        panic("RPi4: firmware must boot hardware CPU0");
    uint64_t pa = aarch64_boot_dtb;
    fdt_view_t v;
    if (!pa || (pa & 7) || pa >= PHYS_MEMORY_END ||
        fdt_view_open(&v, (void *)(uintptr_t)(pa + PAGE_OFFSET),
                      PHYS_MEMORY_END - pa < 0x200000 ? PHYS_MEMORY_END - pa
                                                      : 0x200000) != 0)
        panic("RPi4: missing or malformed firmware DTB");

    unsigned root_ac = 2, root_sc = 1, cpu_ac = 1, res_ac = 2, res_sc = 1;
    int cpus = 0, chosen = 0, memory = 0, reserved = 0, cpu_node = 0;
    int compatible = 0, memory_seen = 0;
    uint64_t id = 0, release = 0;
    unsigned seen = 0;
    int enabled = 1, type_cpu = 0, spin = 0;
    ram_end = PHYS_MEMORY_END;
    fdt_cursor_t cursor;
    fdt_event_t e;
    fdt_cursor_init(&cursor, &v);
    while (fdt_cursor_next(&cursor, &e) > 0) {
        if (e.type == FDT_NODE_BEGIN) {
            if (e.depth == 1) {
                cpus = strcmp(e.name, "cpus") == 0;
                chosen = strcmp(e.name, "chosen") == 0;
                memory = strcmp(e.name, "memory") == 0 ||
                         strncmp(e.name, "memory@", 7) == 0;
                reserved = strcmp(e.name, "reserved-memory") == 0;
            }
            if (cpus && e.depth == 2) {
                cpu_node = strncmp(e.name, "cpu@", 4) == 0;
                id = release = seen = 0;
                enabled = 1;
                type_cpu = spin = 0;
            }
        } else if (e.type == FDT_PROPERTY) {
            if (e.depth == 0) {
                if (strcmp(e.name, "compatible") == 0)
                    compatible = fdt_string_list_has(&e, "brcm,bcm2711");
                if (strcmp(e.name, "#address-cells") == 0 && e.size == 4)
                    root_ac = fdt_u32(e.data);
                if (strcmp(e.name, "#size-cells") == 0 && e.size == 4)
                    root_sc = fdt_u32(e.data);
                if ((root_ac != 1 && root_ac != 2) ||
                    (root_sc != 1 && root_sc != 2))
                    panic("RPi4: unsupported root cell widths");
            }
            if (cpus && e.depth == 1 && strcmp(e.name, "#address-cells") == 0) {
                if (e.size != 4 || !(cpu_ac = fdt_u32(e.data)) || cpu_ac > 2)
                    panic("RPi4: unsupported CPU cell width");
            }
            if (reserved && e.depth == 1) {
                if (strcmp(e.name, "#address-cells") == 0 && e.size == 4)
                    res_ac = fdt_u32(e.data);
                if (strcmp(e.name, "#size-cells") == 0 && e.size == 4)
                    res_sc = fdt_u32(e.data);
                if ((res_ac != 1 && res_ac != 2) ||
                    (res_sc != 1 && res_sc != 2))
                    panic("RPi4: unsupported reservation cell widths");
                if (strcmp(e.name, "ranges") == 0 && e.size)
                    panic("RPi4: translated reserved-memory unsupported");
            }
            if (chosen && e.depth == 1 && strcmp(e.name, "bootargs") == 0) {
                if (!e.size || e.size > sizeof(command_line) ||
                    e.data[e.size - 1])
                    panic("RPi4: invalid command line");
                memcpy(command_line, e.data, e.size);
            }
            if (((memory && e.depth == 1) || (reserved && e.depth == 2)) &&
                strcmp(e.name, "reg") == 0) {
                unsigned ac = memory ? root_ac : res_ac;
                unsigned sc = memory ? root_sc : res_sc;
                unsigned step = 4 * (ac + sc);
                if (!e.size || e.size % step)
                    panic("RPi4: invalid memory reg");
                for (size_t i = 0; i < e.size; i += step) {
                    uint64_t address = cells(e.data + i, ac);
                    uint64_t size = cells(e.data + i + 4 * ac, sc);
                    if (!size || address + size < address)
                        panic("RPi4: invalid RAM range");
                    if (memory && !address) {
                        memory_seen = 1;
                        if (size < ram_end)
                            ram_end = size & ~(uint64_t)4095;
                    } else if (!memory) {
                        reserve_prefix(address, size);
                    }
                }
            }
            if (cpu_node && cpus && e.depth == 2) {
                if (strcmp(e.name, "device_type") == 0)
                    type_cpu = fdt_string_is(&e, "cpu");
                if (strcmp(e.name, "status") == 0)
                    enabled =
                        fdt_string_is(&e, "okay") || fdt_string_is(&e, "ok");
                if (strcmp(e.name, "enable-method") == 0)
                    spin = fdt_string_is(&e, "spin-table");
                if (strcmp(e.name, "reg") == 0) {
                    if (e.size != cpu_ac * 4 || (seen & 1))
                        panic("RPi4: invalid CPU reg");
                    id = cells(e.data, cpu_ac);
                    seen |= 1;
                }
                if (strcmp(e.name, "cpu-release-addr") == 0) {
                    if (e.size != 8 || (seen & 2))
                        panic("RPi4: invalid CPU release address");
                    release = fdt_u64(e.data);
                    seen |= 2;
                }
            }
        } else if (e.type == FDT_NODE_END) {
            if (cpus && cpu_node && e.depth == 2) {
                if (enabled) {
                    if (seen != 3 || !spin || !type_cpu || id >= 4 ||
                        release != 0xd8 + 8 * id || firmware_cpu_count >= 4)
                        panic("RPi4: unsupported CPU topology");
                    for (unsigned i = 0; i < firmware_cpu_count; i++)
                        if (firmware_cpus[i].hw_id == id)
                            panic("RPi4: duplicate CPU ID");
                    firmware_cpus[firmware_cpu_count++] = (smp_cpu_desc_t){
                        .hw_id = id, .platform_cookie = release};
                }
                cpu_node = 0;
            }
            if (e.depth == 1)
                cpus = chosen = memory = reserved = 0;
        }
    }
    if (!compatible || !memory_seen || !firmware_cpu_count)
        panic("RPi4: DTB must describe BCM2711 RAM and spin-table CPUs");
    reserve_prefix(pa, v.size);
    for (size_t i = 0;; i++) {
        uint64_t address, size;
        int rc = fdt_view_reserve(&v, i, &address, &size);
        if (rc <= 0)
            break;
        reserve_prefix(address, size);
    }
    if (ram_end < ((uintptr_t)_end - PAGE_OFFSET) + 0x1000000 ||
        !aarch64_timer_freq())
        panic("RPi4: insufficient low RAM or missing counter frequency");
    printf("[RPi4] firmware CPUs=%u allocator RAM=0..0x%lx counter=%lu Hz\n",
           firmware_cpu_count, (unsigned long)ram_end,
           (unsigned long)aarch64_timer_freq());
}

int rpi4_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx || !base || !end || !ram_end)
        return -1;
    *base = 0;
    *end = ram_end;
    return 0;
}

const char *arch_bootargs_get(void) {
    return command_line;
}

void platform_early_console_init(void) {
    volatile uint32_t *gpio = (void *)(uintptr_t)(PAGE_OFFSET + 0xfe200000UL);
    uint32_t fsel = gpio[1];
    /* GPIO14/15 ALT0 PL011, BCM2711 two-bit pull configuration: no pulls. */
    gpio[1] = (fsel & ~((7U << 12) | (7U << 15))) | (4U << 12) | (4U << 15);
    gpio[0xe4 / 4] &= ~((3U << 28) | (3U << 30));
    arch_mb();
}

static unsigned rpi4_discover(smp_cpu_desc_t *cpus, unsigned capacity,
                              uint64_t boot_hw_id) {
    unsigned count = 0;
    /* Boot CPU must be retained even if firmware orders it after capacity. */
    for (unsigned i = 0; i < firmware_cpu_count; i++)
        if (firmware_cpus[i].hw_id == boot_hw_id && count < capacity)
            cpus[count++] = firmware_cpus[i];
    if (!count)
        return 0;
    for (unsigned i = 0; i < firmware_cpu_count && count < capacity; i++)
        if (firmware_cpus[i].hw_id != boot_hw_id)
            cpus[count++] = firmware_cpus[i];
    return count;
}

static void clean_to_poc(const void *addr) {
    __asm__ __volatile__("dc cvac, %0\n\tdsb sy" ::"r"(addr) : "memory");
}

static int rpi4_start(const smp_cpu_desc_t *cpu, uintptr_t entry_pa,
                      uintptr_t logical_context) {
    /* Adapt firmware's argument-less spin-table handoff to both parameters
     * of the public SMP start contract. */
    if (!entry_pa || (entry_pa & 3) || entry_pa >= ram_end)
        return -1;
    if (cpu->hw_id >= 4 || !logical_context ||
        logical_context >= CONFIG_NR_CPUS)
        return -1;
    volatile uint64_t *slot = (void *)(cpu->platform_cookie + PAGE_OFFSET);
    if (*slot)
        return -1; /* No isolated core restart after publication. */
    aarch64_boot_clean_page_tables();
    rpi4_spin_context[cpu->hw_id].logical_context = logical_context;
    rpi4_spin_context[cpu->hw_id].entry_pa = entry_pa;
    clean_to_poc(&rpi4_spin_context[cpu->hw_id]);
    *slot = (uintptr_t)rpi4_spin_entry - PAGE_OFFSET;
    clean_to_poc((const void *)slot);
    __asm__ __volatile__("sev" ::: "memory");
    return 0;
}

static void rpi4_send_ipi(const smp_cpu_desc_t *cpu, smp_ipi_reason_t reason) {
    (void)reason;
    arch_wmb();
    *(volatile uint32_t *)(uintptr_t)(GICD_BASE + 0xf00) =
        (1U << (16 + cpu->hw_id)) | IRQ_S_SOFT;
}

static uint64_t rpi4_ticks(void) {
    return timer_get_ticks();
}
static uint64_t rpi4_frequency(void) {
    return aarch64_timer_freq();
}
static const timer_ops_t rpi4_timer = {
    .read_ticks = rpi4_ticks,
    .ticks_per_sec = rpi4_frequency,
};
static const smp_platform_ops_t rpi4_smp = {
    .discover = rpi4_discover,
    .start = rpi4_start,
    .send_ipi = rpi4_send_ipi,
};

/* BCM2711 has no PSCI SYSTEM_OFF. The initial profile halts with IRQs
 * masked; physical power removal and watchdog reboot remain board work. */
void platform_firmware_shutdown(void) {
    arch_halt();
}
void platform_firmware_reboot(void) {
    arch_halt();
}

static const board_config_t rpi4_board = {
    .name = "raspberrypi4",
    .ram_base = PHYS_MEMORY_BASE,
    .ram_end = PHYS_MEMORY_END,
    .irqchip = &aarch64_gicv2_ops,
    .timer = &rpi4_timer,
    .smp = &rpi4_smp,
    .early_init = rpi4_early_init,
    .poweroff = platform_firmware_shutdown,
    .reboot = platform_firmware_reboot,
    .defer_network_init = 1,
};
const board_config_t *const current_board = &rpi4_board;
