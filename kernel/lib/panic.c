#include "panic.h"
#include "uart.h"
#include "stdio.h"
#include "arch_ops.h"

NORETURN void panic(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);

    uart_puts("\n\n========== KERNEL PANIC ==========\n");
    vprintf(fmt, args);
    uart_puts("\n\nSystem halted.\n");
    va_end(args);

    arch_irq_disable();
    while (1) arch_cpu_wait();
}
