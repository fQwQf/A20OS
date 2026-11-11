/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "firmware/ivrs.h"
#include "core/klog.h"
#include "drivers/core/amd_iommu.h"
#include "core/errno.h"
#ifdef CONFIG_X86_64
#include "drivers/bus/pci_hal.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_hwapi.h"
#endif

int amd_iommu_firmware_report(const acpi_context_t *context) {
    if (!context)
        return ACPI_NOT_FOUND;
    acpi_table_t table;
    int ret = acpi_table_find(context, "IVRS", 0, &table);
    if (ret == ACPI_NOT_FOUND) {
        kinfo("AMDVI: IVRS absent; driver_enabled=0\n");
        return ret;
    }
    if (ret)
        return ret;
    acpi_ivrs_iterator_t it;
    ret = acpi_ivrs_begin(&table, &it);
    if (ret) {
        kerr("AMDVI: IVRS rejected status=%d; driver_enabled=0\n", ret);
        acpi_table_release(context, &table);
        return ret;
    }
    unsigned hardware = 0, memory = 0, unknown = 0;
    acpi_ivrs_block_t block;
    while (acpi_ivrs_next(&it, &block) > 0) {
        acpi_ivrs_hardware_t h;
        if (!acpi_ivrs_hardware(&block, &h)) {
            hardware++;
            kinfo("AMDVI: IVHD type=0x%x segment=%u requester=0x%x base=0x%lx "
                  "cap=0x%x\n",
                  h.type, h.segment, h.device_id, (unsigned long)h.base,
                  h.capability);
        } else if (block.type >= 0x20 && block.type <= 0x22)
            memory++;
        else
            unknown++;
    }
    kinfo("AMDVI: IVRS revision=%u hardware_records=%u memory_records=%u "
          "unknown_blocks=%u driver_enabled=0\n",
          table.data[8], hardware, memory, unknown);
    acpi_table_release(context, &table);
    return 0;
}

#ifdef CONFIG_X86_64
struct probe_mapping {
    uint64_t base;
    void *address;
};

static int probe_pci_read(void *context, uint16_t segment, uint16_t requester,
                          uint16_t offset, uint32_t *value) {
    (void)context;
    /* The current architecture HAL has no segment argument. Never silently
     * use segment zero for a firmware record from another segment. */
    if (segment)
        return -EOPNOTSUPP;
    *value = arch_pci_config_read32(requester >> 8, (requester >> 3) & 31,
                                    requester & 7, offset);
    return 0;
}

static int probe_mmio_read(void *context, uint64_t base, uint32_t offset,
                           uint64_t *value) {
    struct probe_mapping *mapping = context;
    if (!mapping->address) {
        mapping->address = arch_device_map_mmio(base, 0x4000, 0);
        mapping->base = base;
    }
    if (!mapping->address)
        return -ENOMEM;
    if (mapping->base != base || (offset != 0x18 && offset != 0x30))
        return -EINVAL;
    *value = readq((const volatile uint8_t *)mapping->address + offset);
    return 0;
}
#endif

int amd_iommu_pci_report(const acpi_context_t *context) {
#ifndef CONFIG_X86_64
    (void)context;
    return -EOPNOTSUPP;
#else
    if (!context)
        return ACPI_NOT_FOUND;
    acpi_table_t table;
    int ret = acpi_table_find(context, "IVRS", 0, &table);
    if (ret)
        return ret;
    size_t count = 0;
    ret = acpi_ivrs_unit_count(&table, &count);
    if (ret) {
        kerr("AMDVI: unit policy rejected status=%d; driver_enabled=0\n", ret);
        acpi_table_release(context, &table);
        return ret;
    }
    size_t verified = 0, failed = 0;
    for (size_t i = 0; i < count; i++) {
        acpi_ivrs_hardware_t hardware;
        ret = acpi_ivrs_unit_get(&table, i, &hardware);
        if (ret)
            break;
        struct probe_mapping mapping = {0};
        amd_iommu_probe_ops_t ops = {&mapping, probe_pci_read, probe_mmio_read};
        amd_iommu_snapshot_t snapshot;
        int status = amd_iommu_probe_unit(&hardware, &ops, &snapshot);
        if (mapping.address)
            arch_device_unmap_mmio(mapping.address, 0x4000);
        if (status) {
            failed++;
            kerr("AMDVI: PCI probe segment=%u requester=0x%x type=0x%x "
                 "status=%d; driver_enabled=0\n",
                 hardware.segment, hardware.device_id, hardware.type, status);
            continue;
        }
        verified++;
        kinfo("AMDVI: PCI unit segment=%u requester=0x%x type=0x%x "
              "mmio_enabled=%u registers_read=%u revision_quirk=%u pa_bits=%u "
              "va_bits=%u "
              "aperture=0x%lx control=0x%lx efr=0x%lx\n",
              snapshot.segment, snapshot.requester, hardware.type,
              snapshot.mmio_enabled, snapshot.registers_read,
              snapshot.revision_quirk, snapshot.physical_bits,
              snapshot.virtual_bits, (unsigned long)snapshot.register_size,
              (unsigned long)snapshot.control, (unsigned long)snapshot.efr);
    }
    kinfo("AMDVI: PCI discovery units=%lu verified=%lu failed=%lu "
          "driver_enabled=0\n",
          (unsigned long)count, (unsigned long)verified, (unsigned long)failed);
    acpi_table_release(context, &table);
    return ret ? ret : failed ? -EIO : 0;
#endif
}
#endif
