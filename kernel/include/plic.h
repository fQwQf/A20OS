#ifndef _PLIC_H
#define _PLIC_H

#include "types.h"

void plic_init(void);
void plic_init_hart(void);
uint32_t plic_claim(void);
void plic_complete(uint32_t irq);

#endif
