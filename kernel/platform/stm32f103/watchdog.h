#ifndef _STM32F103_WATCHDOG_H
#define _STM32F103_WATCHDOG_H

/*
 * Independent watchdog (IWDG). The production safety net (§4.4 of the manual):
 * if the main loop ever wedges — a stuck SD/network wait, a runaway task — the
 * IWDG (clocked by the LSI, independent of the main clock) resets the MCU.
 *
 * Register-level; a no-op under CONFIG_STM32_QEMU (the model has no IWDG). Once
 * started the IWDG cannot be stopped, so it is started at the end of bring-up
 * (after the slow one-shot probes) and fed from the service loop.
 */

#include "core/types.h"

/* Start the IWDG with the given timeout. Clamped to the achievable range
 * (~ up to a few seconds via the LSI). Cannot be undone once started. */
void stm32_watchdog_init(uint32_t timeout_ms);

/* Reload the counter — call well within the timeout from the main loop. */
void stm32_watchdog_feed(void);

int stm32_watchdog_active(void);

#endif /* _STM32F103_WATCHDOG_H */
