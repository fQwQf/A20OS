#ifndef STM32_DEVICE_MODEL_H
#define STM32_DEVICE_MODEL_H

/* Publish initialized STM32 peripherals to the A20 driver core.  The MCU
 * profile may omit driver_core; the implementation then becomes a no-op. */
void stm32_device_model_publish(void);

#endif
