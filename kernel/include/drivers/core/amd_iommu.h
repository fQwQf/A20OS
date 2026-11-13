/* SPDX-License-Identifier: Apache-2.0 */
#ifndef A20_AMD_IOMMU_H
#define A20_AMD_IOMMU_H
#include "firmware/ivrs.h"

/* Read-only transport. Config accesses are aligned dwords in the conventional
 * 256-byte space. MMIO reads are aligned, uncached 64-bit accesses at 18h/30h
 * inside the already enabled unit's first 16 KiB. Callbacks return 0 or a
 * negative errno; positive failures become -EIO. No write callback exists. */
typedef struct {
    void *context;
    int (*pci_read32)(void *, uint16_t segment, uint16_t requester,
                      uint16_t offset, uint32_t *value);
    int (*mmio_read64)(void *, uint64_t base, uint32_t offset, uint64_t *value);
} amd_iommu_probe_ops_t;

typedef struct {
    uint16_t segment, requester, capability;
    uint32_t pci_id, capability_header, range, misc;
    uint64_t base, register_size, control, efr;
    uint8_t physical_bits, virtual_bits;
    bool mmio_enabled, registers_read, range_valid, revision_quirk;
} amd_iommu_snapshot_t;

/* The IVHD must come from a validated immutable IVRS mapping. Verifies PCI
 * identity/class/header, the ENTIRE capability chain, exact capability offset,
 * AMD interface revision 1, base agreement and 16/512-KiB aperture alignment.
 * Revision 0 is accepted only for QEMU's 1022:1419 / 1af4:1100 PCI/subsystem
 * identity and is explicitly marked revision_quirk in the snapshot.
 * A disabled MMIO decoder is reported successfully without touching MMIO.
 * register_size is zero if legacy IVHD plus disabled MMIO leaves PCSup unknown.
 * When enabled, reads control/EFR without changing either. Clears out on any
 * failure. No pointers/references escape through the snapshot.
 *
 * A snapshot is not a resource claim or an activation permit. Range describes
 * integrated devices/root ports on one bus, not downstream requester coverage.
 * Caller supplies serialization/lifetime for transport, mapping and firmware;
 * later ownership/activation must revalidate hardware state. */
int amd_iommu_probe_unit(const acpi_ivrs_hardware_t *,
                         const amd_iommu_probe_ops_t *, amd_iommu_snapshot_t *);
/* Supplemental Range-register scope observation for a validated snapshot.
 * Returns a referenced integrated client/root-port ancestor (device_put by
 * caller). Downstream scope requires a verified PCIe root port; malformed
 * capability chains fail closed. RngValid=0 returns -EOPNOTSUPP: use firmware
 * topology instead, never assume universal coverage. Error clears out.
 * This is not IVRS policy, an alias/ACS isolation group, binding ownership or
 * permission to activate DMA. It neither pins the controller nor freezes its
 * snapshot; callers serialize/revalidate before hardware use. */
struct device;
int amd_iommu_scope_get(const amd_iommu_snapshot_t *, struct device *client,
                          struct device **out);
/* x86 segment-0 boot diagnostic after PCI host initialization. Does not claim
 * the device, enable MMIO, write registers or change DMA routing. */
int amd_iommu_pci_report(const acpi_context_t *);
/* Explicit boot-test only: unregister/re-register the passive driver and
 * verify the first controller's aperture lifetime. Does not change hardware.
 * Caller must be in task context before clients start using controllers. */
int amd_iommu_owner_selftest(void);
#endif
