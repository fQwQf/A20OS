#ifndef _ARM32_BOOT_CONFIG_H
#define _ARM32_BOOT_CONFIG_H
#if __has_include("board_boot.h")
#include "board_boot.h"
#endif
#ifndef ARM32_BOOT_BOARD_LAYOUT
#define ARM32_BOOT_BOARD_LAYOUT 0
#endif
#ifndef ARM32_BOOT_RAM_SECTIONS
#define ARM32_BOOT_RAM_SECTIONS 1024
#endif
#ifndef ARM32_BOOT_NORMALIZE_HYP
/* Linux ARM bootloaders may hand off in non-secure HYP or SVC. */
#define ARM32_BOOT_NORMALIZE_HYP 1
#endif
#endif
