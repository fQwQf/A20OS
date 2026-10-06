/*
 * A20OS platform-bus IRQ resource channel self-check.
 *
 * Compiled only when CONFIG_PLATFORM_IRQ_TEST=y; see
 * kernel/drivers/core/platform_irq_test.c for what it asserts and why it is
 * not a hardware test.
 */
#ifndef _DRIVERS_CORE_PLATFORM_IRQ_TEST_H
#define _DRIVERS_CORE_PLATFORM_IRQ_TEST_H

int platform_irq_test_run(void);

#endif