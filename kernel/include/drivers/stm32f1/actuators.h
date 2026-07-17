/*
 * Smart home hub — actuator outputs: fan (PWM), water pump (on/off), buzzer.
 *
 * Hardware driver. No-op under CONFIG_STM32_QEMU (timers/GPIO for these pins
 * are not modelled). See docs/stm32-big-exp.md §5.4.
 *
 * Buzzer = PB8, CONFIRMED from docs/pz/8-蜂鸣器实验: an active buzzer, push-pull,
 * active-high (drive high = sound). The fan/pump pins are PROVISIONAL — verify
 * against the 玄武 board schematic before wiring; they deliberately avoid PB5
 * (status LED) and PB0 (LCD backlight). Defaults: fan = TIM3_CH1/PA6 PWM,
 * pump = PA7 GPIO (to an external MOSFET/H-bridge). The pump/fan MUST go
 * through an external driver stage with a flyback diode and a separate supply
 * — the MCU only provides the control signal.
 */
#ifndef _STM32F103_ACTUATORS_H
#define _STM32F103_ACTUATORS_H

#include "core/types.h"

int stm32_actuators_init(void);

void stm32_fan_set_level(uint8_t level); /* 0..3 -> 0/33/66/100% duty */
void stm32_pump_set(int on);
void stm32_buzzer_set(int on);

#endif /* _STM32F103_ACTUATORS_H */
