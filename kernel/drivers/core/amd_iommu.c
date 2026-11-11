/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "firmware/ivrs.h"
#include "core/klog.h"

int amd_iommu_firmware_report(const acpi_context_t *context)
{
    if (!context) return ACPI_NOT_FOUND;
    acpi_table_t table;
    int ret = acpi_table_find(context, "IVRS", 0, &table);
    if (ret == ACPI_NOT_FOUND) {
        kinfo("AMDVI: IVRS absent; driver_enabled=0\n");
        return ret;
    }
    if (ret) return ret;
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
            kinfo("AMDVI: IVHD type=0x%x segment=%u requester=0x%x base=0x%lx cap=0x%x\n",
                  h.type, h.segment, h.device_id, (unsigned long)h.base, h.capability);
        } else if (block.type >= 0x20 && block.type <= 0x22) memory++;
        else unknown++;
    }
    kinfo("AMDVI: IVRS revision=%u hardware_records=%u memory_records=%u unknown_blocks=%u driver_enabled=0\n",
          table.data[8], hardware, memory, unknown);
    acpi_table_release(context, &table);
    return 0;
}
#endif
