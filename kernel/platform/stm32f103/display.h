#ifndef _STM32F103_DISPLAY_H
#define _STM32F103_DISPLAY_H

#include "core/types.h"

int stm32_display_init(void);
int stm32_display_ready(void);
void stm32_display_show_boot(void);
void stm32_display_update_ticks(uint64_t ticks);
void stm32_display_set_peripherals(int sram_ready, size_t sram_bytes,
                                   int sd_ready, uint64_t sd_sectors,
                                   int sd_fat32, int sd_bus_width,
                                   const char *sd_volume_label,
                                   int touch_ready);
void stm32_display_show_touch(uint16_t x, uint16_t y, int pressed);

#endif
