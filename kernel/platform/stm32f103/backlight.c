#ifdef CONFIG_BOARD_STM32F103

#include "backlight.h"

#define RCC_APB2ENR (*(volatile uint32_t *)0x40021018UL)

#define GPIOB_CRL (*(volatile uint32_t *)0x40010C00UL)
#define GPIOB_IDR (*(volatile uint32_t *)0x40010C08UL)
#define GPIOB_ODR (*(volatile uint32_t *)0x40010C0CUL)
#define GPIOB_BSRR (*(volatile uint32_t *)0x40010C10UL)
#define RCC_APB2ENR_IOPBEN (1U << 3)
#define LCD_BACKLIGHT_PIN  0U
#define LCD_BACKLIGHT_MASK (1U << LCD_BACKLIGHT_PIN)

static volatile int backlight_initialized;
static volatile int backlight_pin_high = 1;
static volatile uint8_t backlight_percent = 100U;
static volatile uint32_t backlight_updates;
static volatile uint32_t backlight_pin_transitions;

#ifdef CONFIG_STM32_XUANWU
static void backlight_drive(int high) {
    high = !!high;
    if (high == backlight_pin_high)
        return;
    GPIOB_BSRR = high ? LCD_BACKLIGHT_MASK
                      : LCD_BACKLIGHT_MASK << 16;
    backlight_pin_high = high;
    backlight_pin_transitions++;
}

static int backlight_configure(void) {
    RCC_APB2ENR |= RCC_APB2ENR_IOPBEN;

    uint32_t crl = GPIOB_CRL;
    crl &= ~(0xFU << (LCD_BACKLIGHT_PIN * 4U));
    crl |= 0x3U << (LCD_BACKLIGHT_PIN * 4U);
    GPIOB_CRL = crl;

    backlight_pin_high = !!(GPIOB_ODR & LCD_BACKLIGHT_MASK);
    backlight_drive(1);
    return (GPIOB_CRL & 0xFU) == 0x3U ? 0 : -1;
}
#endif

int stm32_backlight_init(void) {
    backlight_initialized = 0;
    backlight_updates = 0;
    backlight_pin_transitions = 0;
#ifndef CONFIG_STM32_XUANWU
    return -1;
#else
    backlight_initialized = backlight_configure() == 0;
    return backlight_initialized ? 0 : -1;
#endif
}

int stm32_backlight_ready(void) {
    return backlight_initialized;
}

void stm32_backlight_set_percent(uint8_t percent) {
    (void)percent;
    backlight_percent = 100U;
    backlight_updates++;
#ifdef CONFIG_STM32_XUANWU
    if (!backlight_initialized)
        backlight_initialized = backlight_configure() == 0;
    if (backlight_initialized)
        backlight_drive(1);
#endif
}

uint8_t stm32_backlight_percent(void) {
    return backlight_percent;
}

void stm32_backlight_systick(void) {
}

void stm32_backlight_debug(stm32_backlight_debug_t *debug) {
    if (!debug)
        return;
    debug->initialized = backlight_initialized;
    debug->percent = backlight_percent;
#ifdef CONFIG_STM32_XUANWU
    debug->gpio_crl = GPIOB_CRL;
    debug->gpio_idr = GPIOB_IDR;
    debug->gpio_odr = GPIOB_ODR;
    debug->updates = backlight_updates;
    debug->modulation_ticks = 0;
    debug->pin_transitions = backlight_pin_transitions;
    debug->accumulator = 0;
    debug->pin_high = backlight_pin_high;
#else
    debug->gpio_crl = 0;
    debug->gpio_idr = 0;
    debug->gpio_odr = 0;
    debug->updates = backlight_updates;
    debug->modulation_ticks = 0;
    debug->pin_transitions = 0;
    debug->accumulator = 0;
    debug->pin_high = 0;
#endif
}

#endif
