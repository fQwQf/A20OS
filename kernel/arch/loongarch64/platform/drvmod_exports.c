#include "core/arch.h"
#include "drvmod/export.h"

const struct drv_export arch_drv_export_table[] = {
    {"arch_ram_range_count", (void *)arch_ram_range_count},
    {"arch_ram_range", (void *)arch_ram_range},
};
const unsigned arch_drv_export_count =
    sizeof(arch_drv_export_table) / sizeof(arch_drv_export_table[0]);
