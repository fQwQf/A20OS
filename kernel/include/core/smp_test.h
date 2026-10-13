#ifndef _CORE_SMP_TEST_H
#define _CORE_SMP_TEST_H
/* Bring-up diagnostics only; the opt-in test uses public task APIs. */
#if defined(BRINGUP) && !defined(CONFIG_MCU)
void smp_selftest_boot(void);
void smp_selftest_note_timer_irq(void);
#endif
#endif
