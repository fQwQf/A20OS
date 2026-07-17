#ifndef _STM32F103_BOARD_H
#define _STM32F103_BOARD_H

void stm32_status_led_init(void);
void stm32_status_led_set(int on);
void stm32_status_led_toggle(void);

/* Animation clock published by the 1 kHz SysTick interrupt. */
uint32_t stm32_live2d_frame_clock(void);

#endif
