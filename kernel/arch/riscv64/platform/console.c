
#include "platform.h"
#include "core/bootargs.h"
#include "core/string.h"
int arch_uart_irq(void) {
#ifdef CONFIG_BOARD_QEMU_VIRT_RISCV64
    /* The A20 virtual 16550 exposes a FIFO but no injectable PLIC source.
     * This existing guest boot flag describes that platform contract. Read
     * the firmware command line: uart_init precedes bootargs_init. */
    const char *args = arch_bootargs_get();
    const char flag[] = "a20.hypguest=1";
    while (args && *args) {
        while (*args == ' ' || *args == '\t') args++;
        size_t len = 0;
        while (args[len] && args[len] != ' ' && args[len] != '\t') len++;
        if (len == sizeof(flag) - 1 && !memcmp(args, flag, len))
            return -1;
        args += len;
    }
#endif
    return UART0_IRQ;
}
