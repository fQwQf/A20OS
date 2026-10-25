#include "core/arch.h"
#include "drvmod/export.h"

extern uint64_t arch_timer_frequency(void);

const struct drv_export arch_drv_export_table[] = {
    {"arch_timer_frequency", (void *)&arch_timer_frequency},
};
const unsigned arch_drv_export_count =
    sizeof(arch_drv_export_table) / sizeof(arch_drv_export_table[0]);
