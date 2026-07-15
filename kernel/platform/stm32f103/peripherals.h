#ifndef _STM32F103_PERIPHERALS_H
#define _STM32F103_PERIPHERALS_H

#include "core/types.h"

typedef struct stm32_peripheral_state {
    int display_ready;
    uint16_t display_id;
    int external_sram_ready;
    size_t external_sram_bytes;
    int sdcard_ready;
    int touch_armed;
    int keys_ready;
} stm32_peripheral_state_t;

void stm32_peripherals_init(void);
void stm32_peripherals_service(uint64_t now);
const stm32_peripheral_state_t *stm32_peripherals_state(void);

#endif
