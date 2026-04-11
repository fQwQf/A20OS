#ifndef _SBI_H
#define _SBI_H

#include "types.h"
#include "defs.h"

#define SBI_SET_TIMER_EID       0
#define SBI_CONSOLE_PUTCHAR_EID 1
#define SBI_CONSOLE_GETCHAR_EID 2
#define SBI_SHUTDOWN_EID        8

uint64_t sbi_call(uint64_t eid, uint64_t fid, uint64_t arg0, uint64_t arg1, uint64_t arg2);
void sbi_set_timer(uint64_t time);
void sbi_console_putchar(char c);
int  sbi_console_getchar(void);
void sbi_shutdown(void) NORETURN;

#endif
