#ifndef _STM32F103_DISPLAY_H
#define _STM32F103_DISPLAY_H

#include "core/types.h"

int stm32_display_init(void);
void stm32_display_show_boot(void);
void stm32_display_update_ticks(uint64_t ticks);

#endif
