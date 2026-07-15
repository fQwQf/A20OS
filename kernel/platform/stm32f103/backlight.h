#ifndef _STM32F103_BACKLIGHT_H
#define _STM32F103_BACKLIGHT_H

#include "core/types.h"

typedef struct stm32_backlight_debug {
    uint32_t gpio_crl;
    uint32_t gpio_idr;
    uint32_t gpio_odr;
    uint32_t updates;
    uint32_t modulation_ticks;
    uint32_t pin_transitions;
    uint8_t accumulator;
    uint8_t percent;
    int pin_high;
    int initialized;
} stm32_backlight_debug_t;

int stm32_backlight_init(void);
int stm32_backlight_ready(void);
void stm32_backlight_set_percent(uint8_t percent);
uint8_t stm32_backlight_percent(void);
void stm32_backlight_systick(void);
void stm32_backlight_debug(stm32_backlight_debug_t *debug);

#endif
