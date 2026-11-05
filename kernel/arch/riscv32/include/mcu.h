#ifndef A20_RISCV_MCU_H
#define A20_RISCV_MCU_H
#include "core/types.h"
/* The board owns all SoC registers. Architecture trap code sees only this. */
int riscv_mcu_irq_dispatch(uint32_t cause);
/* Rebuild the U-mode PMP allow-list for the current flat program. */
void riscv_mcu_pmp_update(void);
void riscv_mcu_timer_init(void);
uint64_t riscv_mcu_timer_read(void);
void riscv_mcu_timer_enable(int enabled);
void riscv_mcu_timer_rearm(void);
void riscv_mcu_timer_set_interval(uint64_t ticks);
uint64_t riscv_mcu_timer_interval(void);
void riscv_mcu_reboot(void);
#endif
