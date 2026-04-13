#ifndef _PLAT_IRQ_H
#define _PLAT_IRQ_H

#include "types.h"

void plat_irq_init(void);
void plat_irq_init_cpu(void);
void plat_irq_enable(unsigned int irq);
void plat_irq_disable(unsigned int irq);
int  plat_irq_claim(void);
void plat_irq_complete(unsigned int irq);

#endif
