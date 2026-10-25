#ifndef A20_BOOT_CONTEXT_H
#define A20_BOOT_CONTEXT_H
#include "core/types.h"
#include "firmware/node.h"
#include "boot/framebuffer.h"

enum boot_phase {
    BOOT_PHASE_RESET,
    BOOT_PHASE_HANDOFF,
    BOOT_PHASE_EARLY,
    BOOT_PHASE_MEMORY,
    BOOT_PHASE_DEVICES,
    BOOT_PHASE_READY
};
enum boot_protocol {
    BOOT_PROTOCOL_STATIC,
    BOOT_PROTOCOL_FDT,
    BOOT_PROTOCOL_MULTIBOOT,
    BOOT_PROTOCOL_UEFI
};
/* Architecture adapters publish a mapped FDT with an independently established
 * accessible size, plus the physical ACPI root if present. These sources may
 * coexist: UEFI is a handoff protocol, not a device bus. */
typedef struct boot_context {
    enum boot_phase phase;
    enum boot_protocol protocol;
    const void *fdt;
    size_t fdt_available;
    uint64_t fdt_physical;
    uint64_t acpi_rsdp;
    const char *command_line;
    fw_node_t hardware_root;
    boot_framebuffer_t framebuffer;
    enum boot_framebuffer_memory framebuffer_memory;
    fw_node_t framebuffer_node;
} boot_context_t;

const boot_context_t *boot_context_get(void);
int boot_context_publish(const boot_context_t *handoff);
int boot_context_advance(enum boot_phase expected, enum boot_phase next);
int boot_context_set_hardware_root(const fw_node_t *root);
/* Implemented solely by the selected architecture; no generic code reads
 * boot registers, subtracts PAGE_OFFSET, or accesses firmware pointers itself. */
void arch_boot_context_init(void);
#endif
