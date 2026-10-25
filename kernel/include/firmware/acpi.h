#ifndef _FIRMWARE_ACPI_H
#define _FIRMWARE_ACPI_H
#include "core/types.h"

/* Allocation-free, little-endian ACPI table reader. Mapping policy belongs to
 * the caller; map must cover exactly the requested byte range or return NULL.
 * Successful maps are paired with unmap, including temporary header mappings.
 * Tables must remain immutable while a context/view is in use. */
#define ACPI_SDT_HEADER_SIZE 36U
#define ACPI_TABLE_MAX_SIZE (1024U * 1024U)
#define ACPI_RSDP_MAX_SIZE 4096U
#define ACPI_MAX_ROOT_ENTRIES 1024U
#define ACPI_OK 0
#define ACPI_NOT_FOUND 1
#define ACPI_INVALID (-1)
#define ACPI_UNMAPPED (-2)

typedef struct {
    void *opaque;
    const void *(*map)(void *opaque, uint64_t physical, size_t length);
    void (*unmap)(void *opaque, const void *mapped, size_t length);
    /* Optional SystemMemory OpRegion mapping; never used by the table reader.
     * The same unmap callback must handle both mapping kinds. */
    const void *(*map_region)(void *opaque, uint64_t physical, size_t length);
} acpi_mapper_t;

typedef struct {
    acpi_mapper_t mapper;
    uint64_t rsdp, root;
    unsigned entry_width;
} acpi_context_t;

typedef struct {
    const uint8_t *data;
    size_t length;
    uint64_t physical;
} acpi_table_t;

/* A bad advertised XSDT is an error, never silently replaced by an RSDT. */
int acpi_context_open(acpi_context_t *, const acpi_mapper_t *, uint64_t rsdp);
int acpi_table_map(const acpi_context_t *, uint64_t physical,
                   const char signature[4], acpi_table_t *);
void acpi_table_release(const acpi_context_t *, acpi_table_t *);
int acpi_table_find(const acpi_context_t *, const char signature[4],
                    size_t occurrence, acpi_table_t *);
/* DSDT is referenced by FADT, not by an RSDT/XSDT entry. */
int acpi_dsdt_find(const acpi_context_t *, acpi_table_t *);
/* Allocation-free early reservation of RSDP/root/all SDTs and FADT-selected
 * DSDT/FACS. Callback receives exact physical byte extents; it must round to
 * pages when feeding a PFA. Any nonzero callback error propagates. Partial
 * reservations must be discarded if validation later fails. */
typedef int (*acpi_reserve_callback_t)(void *opaque, uint64_t physical,
                                       size_t length);
int acpi_reserve_tables(const acpi_context_t *, acpi_reserve_callback_t,
                        void *opaque);
int acpi_table_read(const acpi_table_t *, size_t offset, unsigned width,
                    uint64_t *value);
uint16_t acpi_le16(const void *);
uint32_t acpi_le32(const void *);
uint64_t acpi_le64(const void *);

typedef struct {
    uint64_t base; /* ECAM base corresponding to bus 0, not start_bus. */
    uint16_t segment;
    uint8_t start_bus, end_bus;
} acpi_mcfg_entry_t;
/* Validate the complete table before exposing any allocation. */
int acpi_mcfg_count(const acpi_table_t *, size_t *count);
int acpi_mcfg_get(const acpi_table_t *, size_t index, acpi_mcfg_entry_t *);

typedef struct {
    const acpi_table_t *table;
    size_t offset;
} acpi_madt_iterator_t;
typedef struct {
    const uint8_t *data;
    unsigned type, length;
} acpi_madt_entry_t;
int acpi_madt_begin(const acpi_table_t *, acpi_madt_iterator_t *);
/* 1 = entry, 0 = end, negative = invalid. */
int acpi_madt_next(acpi_madt_iterator_t *, acpi_madt_entry_t *);
/* Typed accessors: NOT_FOUND for a different entry type, INVALID if too short. */
int acpi_madt_cpu(const acpi_madt_entry_t *, uint32_t *id, uint32_t *flags);
int acpi_madt_ioapic(const acpi_madt_entry_t *, uint32_t *id,
                     uint32_t *physical, uint32_t *gsi_base);
/* Full namespace/AML service, initialized after heap and IRQ infrastructure.
 * The early table reader above remains independently usable before allocation. */
#include "firmware/node.h"
unsigned arch_acpi_interrupt_model(void);
const acpi_context_t *
arch_acpi_context(void); /* Architecture mapping/discovery only. */
int acpi_namespace_initialize(const acpi_context_t *, fw_node_t *root);
int acpi_namespace_root(fw_node_t *root);
int acpi_pci_root(unsigned segment, unsigned bus, fw_node_t *out);
int acpi_pci_child(const fw_node_t *parent, unsigned dev, unsigned function,
                   fw_node_t *out);
int acpi_pci_route(const fw_node_t *bus, unsigned dev, unsigned function,
                   unsigned pin, uint32_t *gsi);
int acpi_namespace_poweroff(void);
#endif
