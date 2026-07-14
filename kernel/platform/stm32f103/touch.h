#ifndef _STM32F103_TOUCH_H
#define _STM32F103_TOUCH_H

#include "core/types.h"

typedef struct stm32_touch_calibration {
    uint16_t x_min;
    uint16_t x_max;
    uint16_t y_min;
    uint16_t y_max;
    int swap_xy;
    int invert_x;
    int invert_y;
} stm32_touch_calibration_t;

int stm32_touch_init(void);
int stm32_touch_poll(uint16_t *x, uint16_t *y);
int stm32_touch_ready(void);
void stm32_touch_set_calibration(const stm32_touch_calibration_t *calibration);
const stm32_touch_calibration_t *stm32_touch_get_calibration(void);

#endif
