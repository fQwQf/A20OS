#include "core/arch.h"
#include "../../common/boot_context.h"
#ifdef CONFIG_ACPI
#include "firmware/acpi.h"
#endif
extern uint64_t aarch64_boot_dtb;
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
extern uint64_t aarch64_boot_acpi_rsdp;
extern uint64_t aarch64_boot_cmdline, aarch64_boot_uefi_handoff;
#include "uefi/handoff.h"
#include "mm/frame.h"
static a64_uefi_range_t uefi_ranges[PFA_MAX_RANGES];
static size_t uefi_range_count;
size_t aarch64_uefi_ram_range_count(void) {
    return uefi_range_count;
}
int aarch64_uefi_ram_range(size_t index, uint64_t *base, uint64_t *end) {
    if (index >= uefi_range_count || !base || !end)
        return -EINVAL;
    *base = uefi_ranges[index].base;
    *end = *base + uefi_ranges[index].size;
    return 0;
}
#endif
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
static int reserve_acpi(void *opaque, uint64_t physical, size_t bytes) {
    (void)opaque;
    extern char _start[], _end[];
    uint64_t start = (uintptr_t)_start - PAGE_OFFSET,
             end = (uintptr_t)_end - PAGE_OFFSET;
    if (physical < end && start < physical + bytes)
        return -EINVAL;
    return a64_uefi_reserve_range(uefi_ranges, &uefi_range_count,
                                  PFA_MAX_RANGES, physical, bytes);
}
#endif
void arch_boot_context_init(void) {
    boot_context_t context = {.protocol = BOOT_PROTOCOL_STATIC};
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    context.protocol = BOOT_PROTOCOL_UEFI;
    context.acpi_rsdp = aarch64_boot_acpi_rsdp;
    uint64_t physical = aarch64_boot_uefi_handoff;
    if (!physical || physical < A64_UEFI_RAM_BASE ||
        physical > A64_UEFI_RAM_END - A64_UEFI_HANDOFF_BYTES)
        panic("boot: incompatible AArch64 UEFI loader; rebuild BOOTAA64.EFI");
    const a64_uefi_handoff_t *handoff =
        (const void *)(uintptr_t)(physical + PAGE_OFFSET);
    extern char _start[], _end[];
    if (a64_uefi_memory_ranges(handoff, A64_UEFI_HANDOFF_BYTES, physical,
                               A64_UEFI_RAM_BASE, A64_UEFI_RAM_END,
                               (uintptr_t)_start - PAGE_OFFSET,
                               (uintptr_t)_end - PAGE_OFFSET, uefi_ranges,
                               PFA_MAX_RANGES, &uefi_range_count))
        panic("boot: invalid AArch64 UEFI memory handoff");
    if (handoff->cmdline_physical != aarch64_boot_cmdline)
        panic("boot: inconsistent AArch64 UEFI command line");
    context.command_line =
        handoff->cmdline_physical
            ? (const char *)(uintptr_t)(handoff->cmdline_physical + PAGE_OFFSET)
            : NULL;
#else
    uint64_t pa = aarch64_boot_dtb;
    if (pa) {
        context.protocol = BOOT_PROTOCOL_FDT;
        context.fdt_available =
            arch_boot_mapped_span(pa, PHYS_MEMORY_BASE, PHYS_MEMORY_END);
        context.fdt_physical = pa;
        context.fdt = (const void *)(uintptr_t)(pa + PAGE_OFFSET);
    }
#endif
    arch_boot_publish(&context);
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    const acpi_context_t *acpi = arch_acpi_context();
    if (!acpi || acpi_reserve_tables(acpi, reserve_acpi, NULL))
        panic("boot: cannot preserve complete UEFI ACPI tables");
#endif
}

#ifdef CONFIG_ACPI
#include "firmware/acpi.h"
static const void *boot_acpi_map(void *opaque, uint64_t pa, size_t length) {
    (void)opaque;
    if (!length || pa + length < pa)
        return NULL;
#ifdef CONFIG_BOARD_VIRTUALBOX_AARCH64
    /* Only ordinary RAM mappings may back ACPI table bytes. Device windows
     * cannot safely host unaligned byte accesses used by AML/table readers. */
    if (pa < 0x48000000ULL && pa + length <= 0x48000000ULL)
        return (const void *)(uintptr_t)(PAGE_OFFSET + pa);
#else
    (void)pa;
#endif
    return NULL;
}
extern void *arch_device_map_mmio(uint64_t, size_t, uint32_t);
static const void *boot_acpi_map_region(void *opaque, uint64_t pa,
                                        size_t length) {
    (void)opaque;
    return arch_device_map_mmio(pa, length, 0);
}
static void boot_acpi_unmap(void *opaque, const void *mapped, size_t length) {
    (void)opaque;
    (void)mapped;
    (void)length;
}
const acpi_context_t *arch_acpi_context(void) {
    static acpi_context_t acpi;
    static int initialized;
    if (!boot_context_get()->acpi_rsdp)
        return NULL;
    if (!initialized) {
        const acpi_mapper_t mapper = {.map = boot_acpi_map,
                                      .unmap = boot_acpi_unmap,
                                      .map_region = boot_acpi_map_region};
        if (acpi_context_open(&acpi, &mapper, boot_context_get()->acpi_rsdp) !=
            ACPI_OK)
            panic("boot: malformed or unmapped ACPI root");
        initialized = 1;
    }
    return &acpi;
}
#endif
