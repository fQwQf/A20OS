#ifdef CONFIG_PPC64LE

#include "console.h"

void ppc64le_console_link_anchor(void)
{
}

#endif /* CONFIG_PPC64LE */

#include "platform.h"
int arch_uart_irq(void) {
    return UART0_IRQ;
}
