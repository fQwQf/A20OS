#ifdef CONFIG_ARM32

#include "console.h"

void arm32_console_link_anchor(void) {
}

#endif

#include "platform.h"
int arch_uart_irq(void) {
    return UART0_IRQ;
}
