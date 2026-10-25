#ifdef CONFIG_RISCV32

void arch_riscv32_console_linker_anchor(void) {
}

#endif /* CONFIG_RISCV32 */

#include "platform.h"
int arch_uart_irq(void) {
    return UART0_IRQ;
}
