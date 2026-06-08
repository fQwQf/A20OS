#ifndef _ARCH_X86_64_FIRMWARE_H
#define _ARCH_X86_64_FIRMWARE_H

#include "core/types.h"

void firmware_shutdown(void);
void firmware_reboot(void);
void firmware_set_timer(uint64_t time);
void firmware_console_putchar(char c);
int  firmware_console_getchar(void);

#endif
