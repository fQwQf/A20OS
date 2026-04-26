#ifndef _ARCH_LOONGARCH64_FIRMWARE_H
#define _ARCH_LOONGARCH64_FIRMWARE_H

#include "core/types.h"

void firmware_shutdown(void);
void firmware_reboot(void);
void firmware_console_putchar(char c);
int  firmware_console_getchar(void);
void firmware_set_timer(uint64_t time);

/* SBI compatibility — generic code calls sbi_* via sbi.h shim */
void sbi_set_timer(uint64_t time);
void sbi_console_putchar(char c);
int  sbi_console_getchar(void);
void sbi_shutdown(void);
void sbi_reboot(void);

#endif
