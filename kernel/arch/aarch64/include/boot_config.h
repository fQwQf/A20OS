#ifndef _AARCH64_BOOT_CONFIG_H
#define _AARCH64_BOOT_CONFIG_H
/* Optional assembler-safe board overrides. Existing virt and VBox boot
 * layouts retain their defaults. Slots describe 1-GiB L1 blocks. */
#if __has_include("board_boot.h")
#include "board_boot.h"
#endif
#ifndef AARCH64_BOOT_NORMALIZE_EL
#define AARCH64_BOOT_NORMALIZE_EL 0
#endif
#ifndef AARCH64_BOOT_IMAGE_HEADER
#define AARCH64_BOOT_IMAGE_HEADER 0
#endif
#ifndef AARCH64_BOOT_IMAGE_FLAGS
#define AARCH64_BOOT_IMAGE_FLAGS 2 /* LE, 4KiB pages, load near RAM base. */
#endif
#ifndef AARCH64_BOOT_CUSTOM_LAYOUT
#define AARCH64_BOOT_CUSTOM_LAYOUT 0
#endif
#endif
