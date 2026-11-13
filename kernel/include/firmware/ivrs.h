/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_FIRMWARE_IVRS_H
#define A20_FIRMWARE_IVRS_H
#include "firmware/acpi.h"

#define ACPI_IVRS_UNSUPPORTED (-3)
#define ACPI_IVRS_AMBIGUOUS (-4)
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
typedef struct {
    acpi_ivrs_hardware_t hardware;
    uint16_t requester, dma_requester;
    uint8_t settings;
    uint32_t extended;
    bool alias, non_pci_entries;
    size_t memory_count;
} acpi_ivrs_route_t;
/* Read-only firmware routing candidate, not a DMA isolation group or an
 * activation permit. Chooses 40h > 11h > 10h per (segment, IOMMU DeviceID,
 * capability); contradictory bases, duplicate formats, overlapping MMIO
 * windows or overlapping requester owners fail closed. ALL is a fallback
 * within a unit; conflicting explicit matches
 * are ambiguous. Unknown policy in any alternative, undeclared alias targets
 * and alias chains are UNSUPPORTED. ALL plus an alias match is ambiguous.
 * An ALL-only result still needs PCI topology/ownership validation; an invalid
 * hardware Range register cannot supply coverage evidence. All views
 * borrow the immutable table. Limits: 64 units, 1024 blocks. Errors clear out.
 * PCI capability coverage, shared alias groups, HID binding and actual IOMMU
 * capabilities must still be checked by the hardware driver. */
int acpi_ivrs_route(const acpi_table_t *, uint16_t segment, uint16_t requester,
                      acpi_ivrs_route_t *out);
/* Returns raw matching IVMDs in firmware order across all formats of this
 * unit, preserving constraints on the original OR aliased requester. Does not
 * deduplicate, merge or enact permissions/unity/exclusion ranges. */
int acpi_ivrs_route_memory(const acpi_table_t *, uint16_t segment, uint16_t requester,
                             size_t index, acpi_ivrs_memory_t *out);
/* Complete firmware-declared set sharing one DMA requester, under the SAME
 * selected-IVHD routing policy. Includes the original/target and every alias
 * select/range in this segment, resolving each source to detect ambiguity or
 * cross-unit aliases. Sorted unique IDs; excessive sets are UNSUPPORTED.
 * This is a borrowed immutable view, not an isolation group or device claim.
 * Keep both table descriptor and mapping alive; do not modify the view. */
#define ACPI_IVRS_REQUESTER_MAX 128
typedef struct {
    const acpi_table_t *table;
    acpi_ivrs_hardware_t hardware;
    uint16_t requester, dma_requester;
    size_t count, memory_count;
    uint16_t ids[ACPI_IVRS_REQUESTER_MAX];
} acpi_ivrs_requesters_t;
/* Whole-table validation; error clears the ENTIRE view, never a partial set. */
int acpi_ivrs_requesters(const acpi_table_t *, uint16_t segment, uint16_t requester,
                           acpi_ivrs_requesters_t *out);
/* Matching IVMDs for ANY member, once per firmware record, in firmware order
 * across all alternatives of this unit. Requires the unmodified view above.
 * Preserves raw constraints; index outside memory_count -> NOT_FOUND. */
int acpi_ivrs_requester_memory(const acpi_ivrs_requesters_t *, size_t index,
                                 acpi_ivrs_memory_t *out);
/* Enumerate selected, unique unit candidates with the SAME whole-table policy
 * checks as route(). Order follows first appearance of a unit in firmware.
 * These are not initialized units. Errors clear the count/view. */
int acpi_ivrs_unit_count(const acpi_table_t *, size_t *count);
int acpi_ivrs_unit_get(const acpi_table_t *, size_t index, acpi_ivrs_hardware_t *);
/* Read-only boot diagnostic: maps/releases IVRS, logs raw records without
 * deduplicating alternative IVHD formats. No MMIO writes or DMA activation. */
int amd_iommu_firmware_report(const acpi_context_t *);
#endif
