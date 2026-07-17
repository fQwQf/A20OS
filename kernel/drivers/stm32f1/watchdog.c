/*
 * Independent watchdog (IWDG) driver. See watchdog.h. Register access is
 * compiled out under CONFIG_STM32_QEMU (no IWDG in the model).
 */
#ifdef CONFIG_BOARD_STM32F103

#include "drivers/stm32f1/watchdog.h"

#define IWDG_KR  (*(volatile uint32_t *)0x40003000UL) /* key register    */
#define IWDG_PR  (*(volatile uint32_t *)0x40003004UL) /* prescaler       */
#define IWDG_RLR (*(volatile uint32_t *)0x40003008UL) /* reload          */
#define IWDG_SR  (*(volatile uint32_t *)0x4000300CUL) /* status          */

#define IWDG_KEY_FEED   0x0000AAAAu
#define IWDG_KEY_ACCESS 0x00005555u
#define IWDG_KEY_START  0x0000CCCCu

/* LSI is ~40 kHz. Prescaler /256 -> ~156 Hz (6.4 ms/tick), so a 12-bit reload
 * spans up to ~26 s; plenty for our multi-second budget. */
#define IWDG_PRESCALER_PR 6u   /* /256 */
#define IWDG_HZ 156u

static int watchdog_started;

void stm32_watchdog_init(uint32_t timeout_ms) {
    watchdog_started = 1;
#ifndef CONFIG_STM32_QEMU
    uint32_t ticks = (timeout_ms * IWDG_HZ) / 1000u;
    if (ticks == 0)
        ticks = 1;
    if (ticks > 0x0FFFu)
        ticks = 0x0FFFu;

    IWDG_KR = IWDG_KEY_ACCESS; /* unlock PR/RLR */
    IWDG_PR = IWDG_PRESCALER_PR;
    IWDG_RLR = ticks;
    /* Wait for the prescaler/reload updates to be applied (PVU|RVU). */
    for (volatile uint32_t i = 0; (IWDG_SR & 0x3u) && i < 100000u; i++)
        ;
    IWDG_KR = IWDG_KEY_FEED;  /* load RLR into the counter */
    IWDG_KR = IWDG_KEY_START; /* start the watchdog (irreversible) */
#else
    (void)timeout_ms;
#endif
}

void stm32_watchdog_feed(void) {
    if (!watchdog_started)
        return;
#ifndef CONFIG_STM32_QEMU
    IWDG_KR = IWDG_KEY_FEED;
#endif
}

int stm32_watchdog_active(void) { return watchdog_started; }

#endif /* CONFIG_BOARD_STM32F103 */
