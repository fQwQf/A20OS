/*
 * Smart home hub — actuator outputs: fan (PWM), water pump (on/off), buzzer.
 *
 * Hardware driver. No-op under CONFIG_STM32_QEMU (timers/GPIO for these pins
 * are not modelled).
 *
 * Buzzer = PB8. On Xuanwu the fan uses the board's ULN2003 motor input at
 * PB5/TIM3_CH2 (partial remap). PB5 is therefore reserved for the fan
 * instead of the former status heartbeat. The pump remains provisional at PA7
 * GPIO. The pump/fan MUST go
 * through an external driver stage with a flyback diode and a separate supply
 * — the MCU only provides the control signal.
 */
#ifndef _STM32F103_ACTUATORS_H
#define _STM32F103_ACTUATORS_H

#include "core/types.h"

int stm32_actuators_init(void);

void stm32_fan_set_level(uint8_t level); /* 0..3 -> 0/40/50/60% duty */

typedef struct stm32_fan_debug {
    uint32_t afio_mapr;
    uint32_t gpio_config;
    uint32_t gpio_idr;
    uint32_t gpio_odr;
    uint32_t tim_cr1;
    uint32_t tim_ccmr1;
    uint32_t tim_ccer;
    uint32_t tim_psc;
    uint32_t tim_arr;
    uint32_t tim_cnt;
    uint32_t tim_ccr;
    uint8_t requested_level;
    uint8_t override_active;
    uint8_t gpio_mode;
    uint8_t pin_high;
} stm32_fan_debug_t;

/* Bring-up override: bypass the controller and drive the motor input directly.
 * GPIO high must run the ULN2003 channel; GPIO low must stop it. */
void stm32_fan_debug_pwm(uint8_t level);
void stm32_fan_debug_gpio(int high);
void stm32_fan_debug_release(void);
void stm32_fan_debug_snapshot(stm32_fan_debug_t *debug);

void stm32_pump_set(int on);
void stm32_buzzer_set(int on);

#endif /* _STM32F103_ACTUATORS_H */
