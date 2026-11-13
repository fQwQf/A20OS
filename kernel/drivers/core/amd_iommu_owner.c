/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/amd_iommu.h"
#include "core/errno.h"

#if defined(CONFIG_X86_64) && defined(CONFIG_ACPI)
#include "drivers/bus/pci_bus.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_register.h"
#include "core/klog.h"
#include "mm/slab.h"

#define AMDVI_MIN_REGISTERS 0x4000ULL
#define AMDVI_COUNTER_REGISTERS 0x80000ULL
#define AMDVI_EFR_COUNTERS (1ULL << 9)

/* Passive binding only. No PCI/MMIO writes, DMA queues, IRQs or producers
 * exist yet. A future activation path needs its own revalidation and a
 * fallible hardware-stop protocol before these objects may be reclaimed. */
struct amdvi_unit {
    acpi_ivrs_hardware_t hardware; /* Borrows the retained table. */
    amd_iommu_snapshot_t snapshot;
    resource_t aperture;
    driver_resource_claim_t *claim;
    void *registers;
};

struct amdvi_owner {
    device_t *device; /* Registry/binding and claims protect this reference. */
    acpi_context_t acpi;
    acpi_table_t table;
    size_t count;
    struct amdvi_unit units[];
};

struct amdvi_transport {
    struct amdvi_owner *owner;
    struct amdvi_unit *unit;
};

static int firmware_error(int status) {
    if (status == ACPI_NOT_FOUND)
        return -ENODEV;
    if (status == ACPI_IVRS_UNSUPPORTED)
        return -EOPNOTSUPP;
    if (status == ACPI_IVRS_AMBIGUOUS)
        return -EBUSY;
    return status == ACPI_UNMAPPED ? -EIO : -EINVAL;
}

static void owner_destroy(struct amdvi_owner *owner) {
    for (size_t i = owner->count; i; i--) {
        struct amdvi_unit *unit = &owner->units[i - 1];
        if (unit->registers)
            arch_device_unmap_mmio(unit->registers, AMDVI_MIN_REGISTERS);
        driver_resource_release(unit->claim);
    }
    acpi_table_release(&owner->acpi, &owner->table);
    kfree(owner);
}

static int owner_pci_read(void *context, uint16_t segment, uint16_t requester,
                          uint16_t offset, uint32_t *out) {
    struct amdvi_transport *transport = context;
    const acpi_ivrs_hardware_t *hardware = &transport->unit->hardware;
    if (segment != hardware->segment || requester != hardware->device_id ||
        segment || (offset & 3) || offset > 252)
        return -EINVAL;
    *out = pci_cfg_read32(transport->owner->device, offset);
    return 0;
}

static int owner_mmio_read(void *context, uint64_t base, uint32_t offset,
                           uint64_t *out) {
    struct amdvi_transport *transport = context;
    struct amdvi_unit *unit = transport->unit;
    if (!unit->claim || base != unit->hardware.base ||
        (offset != 0x18 && offset != 0x30))
        return -EINVAL;
    if (!unit->registers)
        unit->registers = arch_device_map_mmio(base, AMDVI_MIN_REGISTERS, 0);
    if (!unit->registers)
        return -ENOMEM;
    *out = readq((const volatile uint8_t *)unit->registers + offset);
    return 0;
}

/* Generic claims intentionally permit overlapping leases by one owner.
 * Different capabilities in the SAME function must still be disjoint. */
static int unit_claim(struct amdvi_owner *owner, size_t index, uint64_t size) {
    struct amdvi_unit *unit = &owner->units[index];
    uint64_t base = unit->hardware.base;
    if ((base & (size - 1)) || base > ~(uint64_t)0 - (size - 1))
        return -EINVAL;
    resource_t range = {.type = RES_MMIO,
                        .start = base,
                        .end = base + size - 1,
                        .name = "amdvi-registers"};
    for (size_t i = 0; i < index; i++) {
        const resource_t *other = &owner->units[i].aperture;
        if (range.start <= other->end && other->start <= range.end)
            return -EBUSY;
    }
    driver_resource_claim_t *claim;
    int ret = driver_resource_request_checked(owner->device, &range, &claim);
    if (ret)
        return ret;
    /* Acquire the full extent before dropping the minimum-window lease. */
    driver_resource_release(unit->claim);
    unit->claim = claim;
    unit->aperture = range;
    return 0;
}

static int amdvi_probe(device_t *device) {
    unsigned bus, slot, function;
    int ret = pci_device_location(device, &bus, &slot, &function);
    if (ret)
        return ret;
    uint16_t requester = bus << 8 | slot << 3 | function;
    const acpi_context_t *acpi = arch_acpi_context();
    if (!acpi)
        return -ENODEV;
    acpi_table_t table;
    ret = acpi_table_find(acpi, "IVRS", 0, &table);
    if (ret)
        return firmware_error(ret);
    size_t total, count = 0;
    ret = acpi_ivrs_unit_count(&table, &total);
    if (ret)
        goto table_error;
    for (size_t i = 0; i < total; i++) {
        acpi_ivrs_hardware_t hardware;
        ret = acpi_ivrs_unit_get(&table, i, &hardware);
        if (ret)
            goto table_error;
        if (!hardware.segment && hardware.device_id == requester)
            count++;
    }
    if (!count) {
        acpi_table_release(acpi, &table);
        return -ENODEV;
    }
    /* Parser bounds total to 64 units; still validate the allocation math. */
    if (count >
        (~(size_t)0 - sizeof(struct amdvi_owner)) / sizeof(struct amdvi_unit)) {
        acpi_table_release(acpi, &table);
        return -EOVERFLOW;
    }
    struct amdvi_owner *owner =
        kcalloc(1, sizeof(*owner) + count * sizeof(owner->units[0]));
    if (!owner) {
        acpi_table_release(acpi, &table);
        return -ENOMEM;
    }
    owner->device = device;
    owner->acpi = *acpi;
    owner->table = table;
    for (size_t i = 0; i < total; i++) {
        acpi_ivrs_hardware_t hardware;
        ret = acpi_ivrs_unit_get(&owner->table, i, &hardware);
        if (ret) {
            ret = firmware_error(ret);
            goto fail;
        }
        if (hardware.segment || hardware.device_id != requester)
            continue;
        size_t index = owner->count++;
        struct amdvi_unit *unit = &owner->units[index];
        unit->hardware = hardware;
        uint64_t size =
            hardware.type != 0x10 && (hardware.efr & AMDVI_EFR_COUNTERS)
                ? AMDVI_COUNTER_REGISTERS
                : AMDVI_MIN_REGISTERS;
        ret = unit_claim(owner, index, size);
        if (ret)
            goto fail;
        struct amdvi_transport transport = {owner, unit};
        amd_iommu_probe_ops_t ops = {&transport, owner_pci_read,
                                     owner_mmio_read};
        ret = amd_iommu_probe_unit(&hardware, &ops, &unit->snapshot);
        if (ret)
            goto fail;
        if (!unit->snapshot.register_size) {
            /* Disabled legacy IVHD + EFR support: final span is unknown. */
            ret = -EOPNOTSUPP;
            goto fail;
        }
        if (unit->snapshot.register_size != size) {
            ret = unit_claim(owner, index, unit->snapshot.register_size);
            if (ret)
                goto fail;
        }
    }
    device->drv_priv = owner;
    kinfo(
        "AMDVI_OWNER: bound requester=0x%x units=%lu writes=0 translation=0\n",
        requester, (unsigned long)owner->count);
    return 0;
fail:
    owner_destroy(owner);
    return ret;
table_error:
    acpi_table_release(acpi, &table);
    return firmware_error(ret);
}

static int amdvi_remove(device_t *device) {
    struct amdvi_owner *owner = device->drv_priv;
    device->drv_priv = NULL;
    if (owner)
        owner_destroy(owner);
    return 0;
}

static const device_id_t amdvi_ids[] = {
    {.vendor = 0x1022,
     .device = DEVICE_ANY,
     .subvendor = VENDOR_ANY,
     .subdevice = DEVICE_ANY},
    {0},
};
static driver_t amdvi_driver = {
    .name = "amdvi-owner",
    .id_table = amdvi_ids,
    .bus = &pci_bus,
    .probe = amdvi_probe,
    .remove = amdvi_remove,
    .class_type = DEV_CLASS_NONE,
};
DRIVER_REGISTER(amdvi_driver);

int amd_iommu_owner_selftest(void) {
    const acpi_context_t *acpi = arch_acpi_context();
    if (!acpi)
        return -ENODEV;
    acpi_table_t table;
    int ret = acpi_table_find(acpi, "IVRS", 0, &table);
    if (ret)
        return firmware_error(ret);
    /* The IVRS policy reader bounds units to 64. Pin the controllers which
     * were actually bound, so the test never probes unrelated failed devices
     * (a global probe_all could reinitialize storage/network hardware). */
    device_t *devices[64] = {0};
    size_t total = 0, count = 0;
    driver_discovery_begin();
    ret = acpi_ivrs_unit_count(&table, &total);
    if (ret || total > ARRAY_SIZE(devices)) {
        ret = ret ? firmware_error(ret) : -EOVERFLOW;
        goto release_table;
    }
    for (size_t i = 0; i < total; i++) {
        acpi_ivrs_hardware_t hardware;
        ret = acpi_ivrs_unit_get(&table, i, &hardware);
        if (ret) {
            ret = firmware_error(ret);
            goto release_table;
        }
        device_t *found = NULL;
        ret = pci_device_find_get(hardware.segment, hardware.device_id, &found);
        if (ret == -ENODEV || ret == -EOPNOTSUPP)
            continue;
        if (ret)
            goto release_table;
        bool duplicate = found->drv != &amdvi_driver;
        for (size_t j = 0; j < count; j++)
            duplicate |= devices[j] == found;
        if (duplicate)
            device_put(found);
        else
            devices[count++] = found;
    }
    ret = count ? 0 : -ENODEV;
release_table:
    acpi_table_release(acpi, &table);
    if (ret)
        goto done;
    device_t *device = devices[0];
    struct amdvi_owner *owner = device->drv_priv;
    if (!owner || !owner->count) {
        ret = -ENODEV;
        goto done;
    }
    resource_t range = owner->units[0].aperture;
    device_t peer = {.name = "amdvi-claim-test", .references = 1};
    driver_resource_claim_t *claim = NULL;
    ret = driver_resource_request(&peer, &range, &claim);
    driver_resource_release(claim);
    if (ret != -EBUSY) {
        ret = -EIO;
        goto done;
    }
    ret = driver_unregister(&amdvi_driver);
    if (ret)
        goto done;
    ret = driver_resource_request(&peer, &range, &claim);
    driver_resource_release(claim);
    int rebound = driver_register(&amdvi_driver);
    /* Restore only previous AMD-Vi owners. The outer transaction suppresses
     * register()'s automatic probing of every unbound device. */
    if (!rebound) {
        for (size_t i = 0; i < count; i++)
            if (bus_probe_device(devices[i]) ||
                devices[i]->drv != &amdvi_driver)
                rebound = -EIO;
    }
    if (ret || rebound || device->drv != &amdvi_driver)
        ret = -EIO;
    else
        ret = driver_resource_check_claims(&peer, &range) == -EBUSY ? 0 : -EIO;
done:
    for (size_t i = 0; i < count; i++)
        device_put(devices[i]);
    driver_discovery_end();
    return ret;
}
#else
int amd_iommu_owner_selftest(void) {
    return -EOPNOTSUPP;
}
#endif
#endif
