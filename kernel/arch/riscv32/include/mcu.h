#ifndef A20_RISCV_MCU_H
#define A20_RISCV_MCU_H
#include "core/types.h"
/* The board owns all SoC registers. Architecture trap code sees only this. */
int riscv_mcu_irq_dispatch(uint32_t cause);
void riscv_mcu_timer_init(void);
uint64_t riscv_mcu_timer_read(void);
void riscv_mcu_timer_enable(int enabled);
void riscv_mcu_timer_rearm(void);
void riscv_mcu_reboot(void);
#endif
