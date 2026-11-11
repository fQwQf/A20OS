/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_FIRMWARE_IVRS_H
#define A20_FIRMWARE_IVRS_H
#include "firmware/acpi.h"

#define ACPI_IVRS_UNSUPPORTED (-3)
typedef struct { const acpi_table_t *table; size_t offset; } acpi_ivrs_iterator_t;
typedef struct { const uint8_t *data; size_t length; uint8_t type, flags; } acpi_ivrs_block_t;
typedef struct {
    uint8_t type, flags;
    uint16_t device_id, capability, segment, info;
    uint32_t attributes;
    uint64_t base, efr, efr2;
    const uint8_t *entries;
    size_t entries_length;
} acpi_ivrs_hardware_t;
typedef struct {
    uint8_t type, flags;
    uint16_t device_id, auxiliary, segment;
    uint64_t start, length;
} acpi_ivrs_memory_t;
typedef struct {
    const uint8_t *data;
    size_t length;
    uint8_t type, settings;
    uint16_t device_id;
} acpi_ivrs_device_t;

/* Validates the ENTIRE immutable table before exposing any record. Checks
 * checksum, revision 1/2, block/device lengths, known range pairs and IVMD
 * address overflow. Unknown bounded blocks/fixed entries remain visible;
 * unknown variable device formats return UNSUPPORTED. Views borrow the table
 * mapping; release it only after all views/iterators are no longer used.
 * Validation is not requester policy or permission to enable an IOMMU. */
int acpi_ivrs_begin(const acpi_table_t *, acpi_ivrs_iterator_t *);
int acpi_ivrs_next(acpi_ivrs_iterator_t *, acpi_ivrs_block_t *); /* 1/0/error */
int acpi_ivrs_hardware(const acpi_ivrs_block_t *, acpi_ivrs_hardware_t *);
int acpi_ivrs_memory(const acpi_ivrs_block_t *, acpi_ivrs_memory_t *);
/* Start offset at zero. Preserves raw alias/extended/HID bytes; no scope
 * resolution or alias expansion is implied. Advances offset only on success. */
int acpi_ivrs_device_next(const acpi_ivrs_hardware_t *, size_t *offset,
                           acpi_ivrs_device_t *); /* 1/0/error */
/* Read-only boot diagnostic: maps/releases IVRS, logs raw records without
 * deduplicating alternative IVHD formats. No MMIO writes or DMA activation. */
int amd_iommu_firmware_report(const acpi_context_t *);
#endif
