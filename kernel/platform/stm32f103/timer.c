#ifdef CONFIG_BOARD_STM32F103

#include "core/timer.h"
#include "core/arch.h"
#include "board.h"

#define SYST_CSR (*(volatile uint32_t *)0xE000E010UL)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014UL)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018UL)

#define STM32_HSI_HZ 8000000UL
#define STM32_TICK_HZ ARCH_TIMER_FREQ
#define STM32_SYSTICK_RELOAD (STM32_HSI_HZ / STM32_TICK_HZ)

static volatile uint64_t stm32_ticks;

void timer_init(void) {
    stm32_ticks = 0;
    SYST_RVR = STM32_SYSTICK_RELOAD - 1U;
    SYST_CVR = 0;
    SYST_CSR = 0x7U;
}

void timer_set_interval(uint64_t ticks) {
    if (ticks == 0)
        ticks = 1;
    uint64_t cycles = ticks * STM32_SYSTICK_RELOAD;
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
