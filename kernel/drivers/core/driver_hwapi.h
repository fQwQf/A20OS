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

struct device;

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

/* Port-mapped I/O. Unsupported architectures return all-ones on reads and
 * ignore writes; drivers must only use ports supplied as RES_IOPORT. */
uint8_t ioport_read8(uint16_t port);
void ioport_write8(uint16_t port, uint8_t value);

/* ============================================================
 * DMA API — delegates to arch_dma_sync_for_*
 * DRIVER_IRQ_DMA_SEMANTICS: coherent allocations return zeroed CPU addresses
 * with stable dma_handle values; non-coherent arches must implement sync hooks
 * before device ownership changes. IRQ handlers run with board irqchip ack/eoi
 * ordering from driver_irq_dispatch(), and request/free must be paired.
 * DRIVER_DMA_MASK_MODEL: the address mask below constrains what the allocators
 * are allowed to return, so a coherent handle is always one the device can be
 * given.  See the block after the allocators for the full contract.
 * ============================================================ */
void  *dma_alloc_coherent(struct device *dev, size_t size, uint64_t *dma_handle);
void   dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle);
/* Page-backed physically contiguous allocation for queue/ring hardware whose
 * base address has an alignment requirement stronger than kmalloc provides. */
void  *dma_alloc_coherent_aligned(struct device *dev, size_t size,
                                  size_t alignment, uint64_t *dma_handle);
void   dma_free_coherent_aligned(void *vaddr, size_t size,
                                 uint64_t dma_handle);
void   dma_sync_for_device(void *vaddr, size_t size);
void   dma_sync_for_cpu(void *vaddr, size_t size);

/* ============================================================
 * DMA address mask
 *
 * DRIVER_DMA_MASK_MODEL:
 * - device_t.dma_mask holds the address bits the device's bus-master engine can
 *   be given.  0 means "not declared" and dma_get_mask() reports DMA_MASK_64BIT,
 *   so a device nobody has spoken for yet keeps the whole 64-bit window.
 * - A mask must be a run of low-order ones.  dma_set_mask() rejects anything
 *   else with -EINVAL rather than rounding it up or down: a half-declared window
 *   is how a driver ends up believing it is safe above 4 GiB.
 * - The allocators take the device and refuse to return memory it cannot be
 *   given.  dma_alloc_coherent() keeps the kmalloc fast path and only falls
 *   back to physically contiguous pages when the slab block already lands
 *   outside the window; dma_alloc_coherent_aligned() asks the frame allocator
 *   and retries a bounded number of times.  When nothing satisfies the mask,
 *   both return NULL.  NULL is the caller's cue to fail probe -- there is no
 *   bounce path here and no handle is ever silently truncated.
 * - dma_alloc_*_coherent(NULL, ...) means "no device to constrain" and uses the
 *   full 64-bit window.  That is the right answer for the legacy arch scan path
 *   (virtio_blk_init()), which has no device_t at all.
 * - The free entry points take no device: releasing consults no mask, and the
 *   dma_handle is what pairs an allocation with its release.  Because
 *   dma_alloc_coherent() may hand back either allocator's block and the release
 *   only receives the pointer, dma_free_coherent() asks the slab which one owns
 *   it (kmalloc_owns()) and dispatches: kfree() for a slab or big-alloc block,
 *   pfa_free() by way of dma_free_coherent_aligned() for a frame-allocator
 *   block.  A dma_alloc_coherent() block is released with dma_free_coherent()
 *   and a dma_alloc_coherent_aligned() block with the _aligned pair.
 * - dma_addr_ok()/dma_range_ok() are for the addresses a driver derives itself
 *   (a kmalloc()ed ring, a static context) rather than through the allocators.
 *   A driver that hands out such an address without checking is writing a bus
 *   address the device may not be able to decode.
 * ============================================================ */
#define DMA_MASK_32BIT 0x00000000ffffffffULL
#define DMA_MASK_64BIT 0xffffffffffffffffULL

/* Declare the window this device can be given.  Call it from probe once the
 * hardware capability has been read, before any DMA is allocated. */
int      dma_set_mask(struct device *dev, uint64_t mask);
uint64_t dma_get_mask(const struct device *dev);
/* 1 when @addr / @addr..@addr+@size-1 lie inside dev's window, else 0. */
int      dma_addr_ok(const struct device *dev, uint64_t addr);
int      dma_range_ok(const struct device *dev, uint64_t addr, size_t size);

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

/* Reserve @count consecutive IRQ line ids and return the first, or a negative
 * errno.  A message-signalled device puts the returned number straight into
 * its interrupt message, so the ids have to be exclusive rather than routed.
 * The window comes from arch_irq_msix_vector_range(); a platform without one
 * returns -ENOTSUP and the caller keeps its fallback path. */
int   irq_alloc_vectors(unsigned count);
void  irq_free_vectors(uint32_t first, unsigned count);

#define IRQF_TRIGGER_RISING  0x01
#define IRQF_TRIGGER_FALLING 0x02
/* Both the existing registration and the new request must carry IRQF_SHARED
 * for a line to accept additional handlers; dispatch invokes every handler
 * on the line and free_irq() matches priv against primary and shared
 * handlers alike. */
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
