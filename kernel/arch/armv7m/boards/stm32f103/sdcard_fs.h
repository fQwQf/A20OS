#ifndef _STM32F103_SDCARD_FS_H
#define _STM32F103_SDCARD_FS_H

#include "fs/fat32lite.h"

int stm32_sdcard_fs_mount(void);
void stm32_sdcard_fs_unmount(void);
fat32lite_fs_t *stm32_sdcard_fs(void);
int stm32_sdcard_fs_load_touch_calibration(void);

#endif
