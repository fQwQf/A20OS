#include "plat_irq.h"
#include "riscv64_consts.h"
#include "arch_ops.h"
#include "types.h"

void plat_irq_init(void) {
    *(volatile uint32_t *)(PLIC_PRIORITY + UART0_IRQ * 4) = 1;
    plat_irq_init_cpu();
    arch_irq_enable_external();
}

void plat_irq_init_cpu(void) {
    int hart = 0;
    *(volatile uint32_t *)PLIC_SENABLE(hart) = (1 << UART0_IRQ);
    *(volatile uint32_t *)PLIC_SPRIORITY(hart) = 0;
}

int plat_irq_claim(void) {
    int hart = 0;
    return *(volatile uint32_t *)PLIC_SCLAIM(hart);
}

void plat_irq_complete(uint32_t irq) {
    int hart = 0;
    *(volatile uint32_t *)PLIC_SCLAIM(hart) = irq;
}
