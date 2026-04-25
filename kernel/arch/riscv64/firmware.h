#ifndef _ARCH_RISCV64_FIRMWARE_H
#define _ARCH_RISCV64_FIRMWARE_H

#include "types.h"

#define SBI_SET_TIMER_EID       0
#define SBI_CONSOLE_PUTCHAR_EID 1
#define SBI_CONSOLE_GETCHAR_EID 2
#define SBI_SHUTDOWN_EID        8
#define SBI_SRST_EID            0x53525354UL
#define SBI_SRST_SHUTDOWN       0
#define SBI_SRST_COLD_REBOOT    1

void firmware_set_timer(uint64_t time);
void firmware_console_putchar(char c);
int  firmware_console_getchar(void);
void firmware_shutdown(void);
void firmware_reboot(void);

uint64_t sbi_call(uint64_t eid, uint64_t fid, uint64_t a0, uint64_t a1, uint64_t a2);
void sbi_set_timer(uint64_t time);
void sbi_console_putchar(char c);
int  sbi_console_getchar(void);
void sbi_shutdown(void);
void sbi_reboot(void);

#endif
