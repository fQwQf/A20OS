#include "core/arch.h"
#include "drvmod/export.h"

const struct drv_export arch_drv_export_table[] = {
    {"x86_64_apic_to_cpu", (void *)x86_64_apic_to_cpu},
};
const unsigned arch_drv_export_count =
    sizeof(arch_drv_export_table) / sizeof(arch_drv_export_table[0]);
