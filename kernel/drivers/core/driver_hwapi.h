/*
 * A20OS Driver Model — Hardware Access API
 *
 * Arch-independent hardware access primitives.  Drivers include
 * ONLY this header for MMIO/DMA/IRQ — never arch/ headers.
 *
 * readl/writel are static inline (zero overhead, arch-specific
 * barrier semantics already provided by mb/rmb/wmb).
 */
#ifndef _DRIVER_HWAPI_H
#define _DRIVER_HWAPI_H

#include "core/types.h"
#include "core/defs.h"

/* ============================================================
 * MMIO access — static inline for zero overhead
 * ============================================================ */

static inline uint8_t readb(const volatile void *addr) {
    uint8_t val = *(volatile const uint8_t *)addr;
    rmb();
    return val;
}

static inline uint16_t readw(const volatile void *addr) {
    uint16_t val = *(volatile const uint16_t *)addr;
    rmb();
    return val;
}

static inline uint32_t readl(const volatile void *addr) {
    uint32_t val = *(volatile const uint32_t *)addr;
    rmb();
    return val;
}

static inline uint64_t readq(const volatile void *addr) {
    uint64_t val = *(volatile const uint64_t *)addr;
    rmb();
    return val;
}

static inline void writeb(uint8_t val, volatile void *addr) {
    wmb();
    *(volatile uint8_t *)addr = val;
}

static inline void writew(uint16_t val, volatile void *addr) {
    wmb();
    *(volatile uint16_t *)addr = val;
}

static inline void writel(uint32_t val, volatile void *addr) {
    wmb();
    *(volatile uint32_t *)addr = val;
}

static inline void writeq(uint64_t val, volatile void *addr) {
    wmb();
    *(volatile uint64_t *)addr = val;
}

/* relaxed variants — no barriers, use in tightly loops */
static inline uint32_t readl_relaxed(const volatile void *addr) {
    return *(volatile const uint32_t *)addr;
}

static inline void writel_relaxed(uint32_t val, volatile void *addr) {
    *(volatile uint32_t *)addr = val;
}

/* ============================================================
 * DMA API — delegates to arch_dma_sync_for_*
 * DRIVER_IRQ_DMA_SEMANTICS: coherent allocations return zeroed CPU addresses
 * with stable dma_handle values; non-coherent arches must implement sync hooks
 * before device ownership changes. IRQ handlers run with board irqchip ack/eoi
 * ordering from driver_irq_dispatch(), and request/free must be paired.
 * ============================================================ */
void  *dma_alloc_coherent(size_t size, uint64_t *dma_handle);
void   dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle);
void   dma_sync_for_device(void *vaddr, size_t size);
void   dma_sync_for_cpu(void *vaddr, size_t size);

/* ============================================================
 * IRQ API — delegates to board irqchip_ops
 * ============================================================ */
typedef int (*irq_handler_t)(int irq, void *priv);

int   request_irq(uint32_t irq, irq_handler_t handler,
                  unsigned long flags, void *priv);
/* priv is an ownership token and must exactly match request_irq().  free_irq()
 * masks the line and waits for an in-progress handler before returning. */
void  free_irq(uint32_t irq, void *priv);
void  irq_enable(uint32_t irq);
void  irq_disable(uint32_t irq);
void  driver_irq_dispatch(uint32_t irq);

#define IRQF_TRIGGER_RISING  0x01
#define IRQF_TRIGGER_FALLING 0x02
#define IRQF_SHARED          0x04
#define IRQF_NO_AUTO_ENABLE  0x08

/* ============================================================
 * Delay / Timer helpers
 * ============================================================ */
void    udelay(unsigned usecs);
void    mdelay(unsigned msecs);
uint64_t clock_get_ticks(void);
uint64_t clock_ticks_per_sec(void);

#endif /* _DRIVER_HWAPI_H */
