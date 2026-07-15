#ifdef CONFIG_BOARD_STM32F103

#include "core/timer.h"
#include "core/arch.h"
#include "board.h"
#include "stm32_uart.h"

#define SYST_CSR (*(volatile uint32_t *)0xE000E010UL)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014UL)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018UL)

#define STM32_TICK_HZ ARCH_TIMER_FREQ

static volatile uint64_t stm32_ticks;
static uint32_t stm32_systick_reload;

void timer_init(void) {
    uint32_t hclk = stm32_hclk_hz();
    stm32_systick_reload =
        (hclk + STM32_TICK_HZ / 2U) / STM32_TICK_HZ;
    if (stm32_systick_reload == 0U)
        stm32_systick_reload = 1U;
    if (stm32_systick_reload > 0x01000000U)
        stm32_systick_reload = 0x01000000U;

    uint32_t flags = arch_irq_save();
    stm32_ticks = 0;
    SYST_RVR = stm32_systick_reload - 1U;
    SYST_CVR = 0;
    SYST_CSR = 0x7U;
    arch_irq_restore(flags);
}

void timer_set_interval(uint64_t ticks) {
    if (ticks == 0)
        ticks = 1;
    uint64_t cycles = ticks * stm32_systick_reload;
    if (cycles > 0x01000000ULL)
        cycles = 0x01000000ULL;
    SYST_RVR = (uint32_t)cycles - 1U;
    SYST_CVR = 0;
}

uint64_t timer_get_ticks(void) {
    uint32_t flags = arch_irq_save();
    uint64_t ticks = stm32_ticks;
    arch_irq_restore(flags);
    return ticks;
}

void timer_irq_tick(void) {}
void timer_enable(void) { SYST_CSR |= 0x3U; }
void timer_disable(void) { SYST_CSR &= ~0x3U; }

void armv7m_systick_handler(void) {
    stm32_ticks++;
    if ((stm32_ticks % 500U) == 0)
        stm32_status_led_toggle();
}

#endif
