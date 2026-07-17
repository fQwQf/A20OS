#ifdef CONFIG_BOARD_STM32F103

#include "core/timer.h"
#include "core/arch.h"
#include "board.h"
#include "backlight.h"
#include "live2d.h"
#include "stm32_uart.h"

#define SYST_CSR (*(volatile uint32_t *)0xE000E010UL)
#define SYST_RVR (*(volatile uint32_t *)0xE000E014UL)
#define SYST_CVR (*(volatile uint32_t *)0xE000E018UL)

#define STM32_TICK_HZ ARCH_TIMER_FREQ
#define STM32_PREEMPT_SLICE_MS 10U
#define SCB_ICSR (*(volatile uint32_t *)0xE000ED04UL)
#define SCB_ICSR_PENDSVSET (1UL << 28)
#define SCB_SHPR3 (*(volatile uint32_t *)0xE000ED20UL)

static volatile uint64_t stm32_ticks;
static uint32_t stm32_systick_reload;
static volatile uint32_t stm32_live2d_clock;
static uint32_t stm32_live2d_countdown;

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
    stm32_live2d_clock = 0;
    stm32_live2d_countdown = LIVE2D_FRAME_MS;
    SYST_RVR = stm32_systick_reload - 1U;
    SYST_CVR = 0;
    /* PendSV must run below SysTick and all device IRQs. It only redirects the
     * eventual exception return; the scheduler itself runs in thread mode. */
    SCB_SHPR3 = (SCB_SHPR3 & 0x0000FFFFU) | (0x80U << 24) | (0xFFU << 16);
    SYST_CSR = 0x7U;
    arch_irq_restore(flags);
}

void timer_set_interval(uint64_t ticks) {
    /* STM32 peripherals use timer_get_ticks() as milliseconds. Keep SysTick at
     * 1 kHz instead of applying the generic scheduler's tickless rearm request. */
    (void)ticks;
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

uint32_t stm32_live2d_frame_clock(void) {
    return stm32_live2d_clock;
}

void armv7m_systick_handler(void) {
    stm32_ticks++;
    if (--stm32_live2d_countdown == 0U) {
        stm32_live2d_countdown = LIVE2D_FRAME_MS;
        stm32_live2d_clock++;
    }
    stm32_backlight_systick();
    if ((stm32_ticks % 500U) == 0)
        stm32_status_led_toggle();
    if ((stm32_ticks % STM32_PREEMPT_SLICE_MS) == 0U)
        SCB_ICSR = SCB_ICSR_PENDSVSET;
}

#endif
