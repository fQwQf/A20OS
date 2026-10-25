#ifndef LS2K0300_LIOINTC_H
#define LS2K0300_LIOINTC_H
#include "core/types.h"

/* Register mechanics for the two 32-source legacy banks (manual v1.01,
 * section 3.5). Offsets are relative to the first bank, not CPU addresses. */
typedef struct {
    void *context;
    uint32_t (*read32)(void *, unsigned);
    void (*write32)(void *, unsigned, uint32_t);
    void (*write8)(void *, unsigned, uint8_t);
} ls2k0300_lio_io_t;
typedef struct {
    ls2k0300_lio_io_t io;
    uint32_t enabled[2], edge[2], polarity[2];
} ls2k0300_lio_t;
int ls2k0300_lio_init(ls2k0300_lio_t *, const ls2k0300_lio_io_t *);
int ls2k0300_lio_configure(ls2k0300_lio_t *, unsigned irq, unsigned type);
int ls2k0300_lio_enable(ls2k0300_lio_t *, unsigned irq);
void ls2k0300_lio_disable(ls2k0300_lio_t *, unsigned irq);
void ls2k0300_lio_dispatch(ls2k0300_lio_t *, unsigned bank, uint32_t pending,
                           void (*dispatch)(uint32_t));
void ls2k0300_irq_init(void);
void ls2k0300_handle_device_irq(unsigned bank);
extern const struct irqchip_ops ls2k0300_irqchip_ops;
#endif
