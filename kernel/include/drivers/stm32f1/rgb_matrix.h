#ifndef _STM32F103_RGB_MATRIX_H
#define _STM32F103_RGB_MATRIX_H

#include "core/types.h"

#define STM32_RGB_MATRIX_WIDTH  5U
#define STM32_RGB_MATRIX_HEIGHT 5U
#define STM32_RGB_MATRIX_PIXELS (STM32_RGB_MATRIX_WIDTH * STM32_RGB_MATRIX_HEIGHT)

/* Colors use the conventional 0xRRGGBB representation. */
#define STM32_RGB_COLOR_BLACK   0x000000U
#define STM32_RGB_COLOR_RED     0xFF0000U
#define STM32_RGB_COLOR_GREEN   0x00FF00U
#define STM32_RGB_COLOR_BLUE    0x0000FFU
#define STM32_RGB_COLOR_WHITE   0xFFFFFFU
#define STM32_RGB_COLOR_YELLOW  0xFFFF00U

/*
 * Xuanwu's 5x5 WS2812B matrix is wired to PE5. Pixel changes are buffered;
 * call stm32_rgb_matrix_show() once after composing a frame.
 */
int stm32_rgb_matrix_init(void);
int stm32_rgb_matrix_ready(void);
int stm32_rgb_matrix_set_pixel(uint8_t x, uint8_t y, uint32_t rgb);
uint32_t stm32_rgb_matrix_get_pixel(uint8_t x, uint8_t y);
void stm32_rgb_matrix_fill(uint32_t rgb);
void stm32_rgb_matrix_clear(void);
void stm32_rgb_matrix_set_brightness(uint8_t brightness);
uint8_t stm32_rgb_matrix_brightness(void);
int stm32_rgb_matrix_show(void);

#endif
