#ifndef _STM32F103_KEYS_H
#define _STM32F103_KEYS_H

#include "core/types.h"

typedef enum stm32_key {
    STM32_KEY_NONE = 0,
    STM32_KEY_UP,
    STM32_KEY_DOWN,
    STM32_KEY_LEFT,
    STM32_KEY_RIGHT,
} stm32_key_t;

int stm32_keys_init(void);
stm32_key_t stm32_keys_poll(void);
int stm32_keys_ready(void);

#endif
