#ifdef CONFIG_LOONGARCH64

#include "arch.h"

void plic_init(void) {
    arch_write_sie(arch_read_sie() | SIE_SEIE);
}

void plic_init_hart(void) {
}

uint32_t plic_claim(void) {
    return 0;
}

void plic_complete(uint32_t irq) {
    (void)irq;
}

#endif
