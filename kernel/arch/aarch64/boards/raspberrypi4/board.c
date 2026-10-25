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
static fw_dt_range_t memory_ranges[PFA_MAX_RANGES];
static size_t memory_count;
static void rpi4_early_init(void) {
    if (arch_smp_boot_hw_id() != 0) panic("RPi4: firmware must boot hardware CPU0");
    const boot_context_t *b = boot_context_get();
    fw_dt_t tree; fw_node_t root, cpus, cpu;
    if (!b || !b->fdt || fw_dt_init(&tree, b->fdt, b->fdt_available) ||
        fw_dt_root(&tree, &root) || root.ops->match_id(&root, "brcm,bcm2711") != 1 ||
        fw_dt_find_path(&root, "/cpus", &cpus)) panic("RPi4: invalid BCM2711 firmware tree");
    uint32_t cells = 1;
    fw_property_t property;
    int rc = cpus.ops->property(&cpus, "#address-cells", &property);
    if (!rc) {
        if (property.size != 4) panic("RPi4: invalid CPU cells");
        cells = fdt_u32(property.data);
    } else if (rc != -ENOENT) panic("RPi4: invalid CPU cells");
    if (cells != 1 && cells != 2) panic("RPi4: unsupported CPU cells");
    firmware_cpu_count = 0;
    rc = cpus.ops->next_child(&cpus, NULL, &cpu);
    while (!rc) {
        const char *type = NULL, *method = NULL;
        int enabled = cpu.ops->available(&cpu);
        if (enabled < 0) panic("RPi4: invalid CPU status");
        if (enabled && !fw_node_read_string(&cpu, "device_type", 0, &type) && !strcmp(type, "cpu")) {
            uint64_t id, release;
            if (cpu.ops->property(&cpu, "reg", &property) || property.size != cells * 4)
                panic("RPi4: invalid CPU reg");
            id = cells == 2 ? fdt_u64(property.data) : fdt_u32(property.data);
            if (fw_node_read_string(&cpu, "enable-method", 0, &method) || strcmp(method, "spin-table") ||
                fw_node_read_u64(&cpu, "cpu-release-addr", &release) ||
                id >= 4 || release != 0xd8 + 8 * id || firmware_cpu_count >= 4)
                panic("RPi4: unsupported CPU topology");
            for (unsigned i = 0; i < firmware_cpu_count; i++)
                if (firmware_cpus[i].hw_id == id) panic("RPi4: duplicate CPU ID");
            firmware_cpus[firmware_cpu_count++] = (smp_cpu_desc_t){.hw_id=id,.platform_cookie=release};
        }
        fw_node_t next; rc = cpus.ops->next_child(&cpus, &cpu, &next);
        if (!rc) cpu = next;
    }
    if (rc != -ENOENT || !firmware_cpu_count) panic("RPi4: missing CPU topology");
    if (fw_dt_memory_window(&tree, b->fdt_physical, PHYS_MEMORY_BASE, PHYS_MEMORY_END,
                            (uintptr_t)_start - PAGE_OFFSET, (uintptr_t)_end - PAGE_OFFSET,
                            memory_ranges, PFA_MAX_RANGES, &memory_count, 0))
        panic("RPi4: invalid or excessive DT RAM/reservations");
    uint64_t available = 0, last = (uintptr_t)_end - PAGE_OFFSET;
    for (size_t i = 0; i < memory_count; i++) {
        uint64_t end = memory_ranges[i].base + memory_ranges[i].size;
        uint64_t start = memory_ranges[i].base > last ? memory_ranges[i].base : last;
        if (end > start) available += end - start;
    }
    if (available < 0x1000000 || !aarch64_timer_freq()) panic("RPi4: insufficient usable RAM/counter");
    printf("[RPi4] firmware CPUs=%u allocator RAM=%lu ranges counter=%lu Hz\n",
           firmware_cpu_count, (unsigned long)memory_count, (unsigned long)aarch64_timer_freq());
}
size_t rpi4_ram_range_count(void) { return memory_count; }
int rpi4_ram_range(size_t idx, paddr_t *base, paddr_t *end) {
    if (idx >= memory_count || !base || !end) return -EINVAL;
    *base = memory_ranges[idx].base; *end = memory_ranges[idx].base + memory_ranges[idx].size;
    return 0;
}
const char *arch_bootargs_get(void) {
    const boot_context_t *b = boot_context_get();
    return b ? b->command_line : NULL;
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
    if (!entry_pa || (entry_pa & 3))
        return -1;
    int in_ram = 0;
    for (size_t i = 0; i < memory_count; i++)
        if (entry_pa >= memory_ranges[i].base &&
            entry_pa - memory_ranges[i].base < memory_ranges[i].size)
            in_ram = 1;
    if (!in_ram)
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
