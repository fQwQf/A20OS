#ifndef _STM32F103_LIGHT_SENSOR_H
#define _STM32F103_LIGHT_SENSOR_H

#include "core/types.h"

typedef struct stm32_light_sensor_info {
    int ready;
    int auto_brightness;
    uint16_t raw_adc;
    uint16_t filtered_adc;
    uint8_t intensity_percent;
    uint8_t target_backlight_percent;
    uint8_t backlight_percent;
    uint32_t samples;
    uint32_t brightness_updates;
    uint32_t errors;
} stm32_light_sensor_info_t;

int stm32_light_sensor_init(void);
int stm32_light_sensor_sample(void);
void stm32_light_sensor_set_auto_brightness(int enable);
void stm32_light_sensor_set_manual_brightness(uint8_t percent);
const stm32_light_sensor_info_t *stm32_light_sensor_info(void);

#endif
