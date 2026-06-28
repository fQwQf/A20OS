#include "core/panic.h"
#include "drivers/char/uart.h"
#include "core/stdio.h"
#include "core/defs.h"
#include "core/arch.h"

NORETURN void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    uart_puts("\n\n========== KERNEL PANIC ==========\n");
    vprintf(fmt, args);
    va_end(args);

    uart_puts("\n[PANIC] attempting firmware poweroff\n");
    firmware_shutdown();

    arch_halt();
    for (;;)
        __asm__ volatile("");
}
