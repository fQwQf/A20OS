#include "core/panic.h"
#include "drv/uart.h"
#include "core/stdio.h"
#include "core/defs.h"

NORETURN void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    uart_puts("\n\n========== KERNEL PANIC ==========\n");
    vprintf(fmt, args);
    va_end(args);

    arch_halt();
}
