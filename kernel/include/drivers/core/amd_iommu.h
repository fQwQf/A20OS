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
/* Native retained route observation, with optional explicit reservation below.
 * Acquiring an observation alone does not claim a requester or DMA domain.
 * The value contains no borrowed firmware pointers. settings/extended and
 * IVMD constraints are raw policy which an eventual provider must implement. */
typedef struct amd_iommu_route amd_iommu_route_t;
typedef struct {
    amd_iommu_snapshot_t unit;
    uint16_t requester, dma_requester;
    uint8_t settings;
    uint32_t extended;
    size_t memory_count;
    bool alias, non_pci_entries, hardware_range_checked;
} amd_iommu_route_info_t;
/* Task context, caller holds client reference. Combines retained IVRS policy,
 * a live passive controller binding and published PCI paths for both original
 * and alias requesters. Valid hardware Range is an additional constraint;
 * RngValid=0 uses firmware routing plus PCI topology. Failure clears out.
 * Success retains device/anchor identities, firmware and aperture claims.
 * No writes, implicit dependency links, DMA enable or alias exclusivity. */
int amd_iommu_route_acquire(struct device *client, amd_iommu_route_t **out);
/* Revalidates binding, PCI identities/paths and the unit's read-only snapshot.
 * Clears info on failure. Detached owner -> -ESHUTDOWN; changed unit/anchor
 * -> -ESTALE. Does not freeze hardware: serialize validation AND subsequent
 * use with an outer discovery transaction. A route never grants activation. */
int amd_iommu_route_validate(amd_iommu_route_t *, amd_iommu_route_info_t *info);
/* Copy the complete firmware-declared requester set, including sibling aliases.
 * Metadata remains readable after unbind; no PCI references/pointers escape.
 * Success copies all IDs and sets count. Insufficient capacity -> -ENOSPC with
 * required count (NULL IDs/capacity zero is a size query). Other errors clear
 * count; the buffer is unchanged on all errors. This is not live admission. */
int amd_iommu_route_requesters(amd_iommu_route_t *, uint16_t *ids,
                                size_t capacity, size_t *count);
/* Reserve ALL firmware-declared requesters sharing this DMA requester.
 * Task context; caller has stopped every peer's DMA via its owning driver.
 * Revalidates the route, requires every peer published, kernel-owned and BME
 * disabled, then atomically reserves all IDs and retains each identity/scope.
 * Missing peer -> -ENODEV; no partial reservation or automatic peer unbind.
 * Duplicate on the same live handle -> -EALREADY; any overlapping reservation
 * -> -EBUSY, including aliases, old device identities and detached bindings.
 * Success changes software ownership only; it neither drains DMA nor proves
 * ACS/peer isolation, in-flight DMA drain, IVMD hardware enforcement or activation.
 * The route owns this reservation until release; retain it on uncertain stop.
 * A failed reservation leaves the original observation handle owned by caller. */
int amd_iommu_route_reserve(amd_iommu_route_t *);
/* Copies raw matching IVMDs, including original/alias constraints, in firmware
 * order. Index outside memory_count -> -ENODEV; error clears out. Retained
 * immutable policy remains readable after controller unbind. */
int amd_iommu_route_memory(amd_iommu_route_t *, size_t index,
                            acpi_ivrs_memory_t *out);
/* Build an independent immutable DMA policy from this live route's COMPLETE
 * firmware requester set and all retained IVHD alternatives. Each matching
 * IVMD record is included once, even when it covers several peers.
 * IR/IW become device READ/WRITE restrictions; Unity requires identity.
 * Reserved flags or related ExclusionRange -> -EOPNOTSUPP; Unity with neither
 * IR nor IW -> -EINVAL. Never silently ignore unsupported hardware policy.
 * Clears out on failure. No reservation, hardware access writes, auto unity
 * mappings or activation; a policy reference does not retain the route or
 * controller. Provider still coordinates ALL alias peers and hardware state. */
struct driver_dma_policy;
int amd_iommu_route_policy_create(amd_iommu_route_t *, struct driver_dma_policy **out);
/* Exclusive release: no concurrent use of this handle. Controller unbind
 * invalidates live validation immediately, but firmware/MMIO reservations
 * survive until the final route is released. A reserved route also releases
 * its requester set: confirm hardware detach before release. No hardware stop
 * or invalidation is performed by this function. */
void amd_iommu_route_release(amd_iommu_route_t *);
/* x86 segment-0 boot diagnostic after PCI host initialization. Does not claim
 * the device, enable MMIO, write registers or change DMA routing. */
int amd_iommu_pci_report(const acpi_context_t *);
/* Explicit boot-test only: unregister/re-register the passive driver and
 * verify the first controller's aperture lifetime. Does not change hardware.
 * Caller must be in task context before clients start using controllers. */
int amd_iommu_owner_selftest(void);
#endif
