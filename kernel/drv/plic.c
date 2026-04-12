#include "plic.h"
#include "consts.h"
#include "arch_ops.h"

void plic_init(void) {
    *(volatile uint32_t *)(PLIC_PRIORITY + UART0_IRQ * 4) = 1;
    plic_init_hart();
    arch_irq_enable_external();
}

void plic_init_hart(void) {
    int hart = 0;
    *(volatile uint32_t *)PLIC_SENABLE(hart) = (1 << UART0_IRQ);
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

uint32_t plic_claim(void) {
    int hart = 0;
    return *(volatile uint32_t *)PLIC_SCLAIM(hart);
}

void plic_complete(uint32_t irq) {
    int hart = 0;
    *(volatile uint32_t *)PLIC_SCLAIM(hart) = irq;
}
