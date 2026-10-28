#include "core/arch.h"
#include "drvmod/export.h"

extern uint64_t arch_timer_frequency(void);
extern void riscv_dma_sync_device(const void *, size_t);
extern void riscv_dma_sync_cpu(const void *, size_t);

const struct drv_export arch_drv_export_table[] = {
    {"arch_timer_frequency", (void *)&arch_timer_frequency},
    {"riscv_dma_sync_device", (void *)&riscv_dma_sync_device},
    {"riscv_dma_sync_cpu", (void *)&riscv_dma_sync_cpu},
};
const unsigned arch_drv_export_count =
    sizeof(arch_drv_export_table) / sizeof(arch_drv_export_table[0]);
