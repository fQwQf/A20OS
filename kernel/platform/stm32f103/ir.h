/*
 * Smart home hub — infrared remote receiver (NEC) on PB9 / EXTI9.
 *
 * Ported from docs/pz/28-红外遥控实验. The whole NEC frame is decoded
 * synchronously inside the EXTI9_5 interrupt by measuring high-pulse widths.
 *
 * Hardware only: QEMU's stm32vldiscovery has no IR signal source, so init is a
 * no-op there and no interrupt is enabled. NOT yet verified on real hardware.
 * (A production version should use a timer-capture decoder rather than a
 * busy-loop in the ISR — see docs/stm32-big-exp.md §5.3.)
 */
#ifndef _STM32F103_IR_H
#define _STM32F103_IR_H

#include "core/types.h"

/* Configure PB9 + EXTI9 + NVIC. 0 ok, -1 skipped (QEMU). */
int stm32_ir_init(void);

/* If a new 32-bit code has been decoded since the last poll, store it in
 * *code and return 1; otherwise return 0. */
int stm32_ir_poll(uint32_t *code);

/* EXTI9_5 interrupt body — called from the arch IRQ stub via trap.c. */
void stm32_ir_isr(void);

#endif /* _STM32F103_IR_H */
