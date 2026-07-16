/*
 * Smart home hub — self-test entry (runs on QEMU, where the peripheral
 * bring-up is compiled out). Exercises the hardware-independent core so
 * `make run-stm32f103-qemu` demonstrates the rule engine + frame protocol.
 */
#ifndef _STM32F103_SMARTHUB_H
#define _STM32F103_SMARTHUB_H

/* Runs the environment-rule and protocol self-tests, printing to USART1.
 * Returns 0 if all checks passed, non-zero = number of failures. */
int smarthub_selftest(void);

#endif /* _STM32F103_SMARTHUB_H */
