#include "core/arch.h"
#include "drvmod/export.h"

const struct drv_export arch_drv_export_table[] = {
    {"firmware_acpi_tpm2", (void *)firmware_acpi_tpm2},
    {"x86_64_apic_to_cpu", (void *)x86_64_apic_to_cpu},
};
const unsigned arch_drv_export_count =
    sizeof(arch_drv_export_table) / sizeof(arch_drv_export_table[0]);
