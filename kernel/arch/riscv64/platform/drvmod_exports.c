#include "core/arch.h"
#include "drvmod/export.h"

extern uint64_t riscv64_timer_freq(void);

const struct drv_export arch_drv_export_table[] = {
    {"riscv64_timer_freq", (void *)&riscv64_timer_freq},
};
const unsigned arch_drv_export_count =
    sizeof(arch_drv_export_table) / sizeof(arch_drv_export_table[0]);
