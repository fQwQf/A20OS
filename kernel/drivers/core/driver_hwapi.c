#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_core.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "core/errno.h"
#include "core/lock.h"
#ifdef CONFIG_IOPORT
#include "cpu.h"
#endif

extern const board_config_t *const current_board;

static irq_handler_t irq_handlers[256];
static void         *irq_priv[256];
static unsigned long irq_flags[256];
static unsigned int irq_active[256];
static spinlock_t irq_table_lock = SPINLOCK_INIT;

uint8_t ioport_read8(uint16_t port)
{
#ifdef CONFIG_IOPORT
    return inb(port);
#else
    (void)port;
    return 0xffU;
#endif
}

void ioport_write8(uint16_t port, uint8_t value)
{
#ifdef CONFIG_IOPORT
    outb(port, value);
#else
    (void)port;
    (void)value;
#endif
}

/* DRIVER_IRQ_TABLE_FIXED_LIMIT: platform IRQ lines are capped at 256 until the
 * irq registry is replaced by a dynamically sized irqdomain-style structure. */

int request_irq(uint32_t irq, irq_handler_t handler,
                unsigned long flags, void *priv) {
    if (irq >= 256 || !handler)
        return -EINVAL;
    uint64_t lock_flags = spin_lock_irqsave(&irq_table_lock);
    if (irq_handlers[irq]) {
        spin_unlock_irqrestore(&irq_table_lock, lock_flags);
        return -EBUSY;
    }
    irq_handlers[irq] = handler;
    irq_priv[irq]     = priv;
    irq_flags[irq]    = flags;
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);

    if (!(flags & IRQF_NO_AUTO_ENABLE))
        irq_enable(irq);

    return 0;
}

void free_irq(uint32_t irq, void *priv) {
    if (irq >= 256)
        return;
    uint64_t lock_flags = spin_lock_irqsave(&irq_table_lock);
    if (!irq_handlers[irq] || irq_priv[irq] != priv) {
        spin_unlock_irqrestore(&irq_table_lock, lock_flags);
        return;
    }
    irq_disable(irq);
    irq_handlers[irq] = NULL;
    irq_priv[irq]     = NULL;
    irq_flags[irq]    = 0;
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);

    /* A handler that took its snapshot before the table entry was cleared may
     * still be running on another CPU.  Driver remove cannot release its
     * private state until that invocation has returned. */
    while (__atomic_load_n(&irq_active[irq], __ATOMIC_ACQUIRE) != 0)
        __asm__ volatile("" ::: "memory");
}

void irq_enable(uint32_t irq) {
    if (irq >= 256)
        return;
    if (current_board && current_board->irqchip &&
        current_board->irqchip->enable_irq)
        current_board->irqchip->enable_irq(irq);
}

void irq_disable(uint32_t irq) {
    if (irq >= 256)
        return;
    if (current_board && current_board->irqchip &&
        current_board->irqchip->disable_irq)
        current_board->irqchip->disable_irq(irq);
}

void driver_irq_dispatch(uint32_t irq) {
    if (irq >= 256)
        return;

    /*
     * Reading IAR has already acknowledged this interrupt at the CPU
     * interface.  It must be completed even when no driver claimed the line;
     * otherwise GICv3 keeps it active and takes it again as soon as a newly
     * scheduled task unmasks IRQs.  VBox ARM exposes firmware/PCI lines that
     * are not all registered during early bring-up, so the old early return
     * could trap the first userspace child in an interrupt storm.
     */
    uint64_t lock_flags = spin_lock_irqsave(&irq_table_lock);
    irq_handler_t handler = irq_handlers[irq];
    void *priv = irq_priv[irq];
    if (!handler) {
        spin_unlock_irqrestore(&irq_table_lock, lock_flags);
        if (current_board && current_board->irqchip &&
            current_board->irqchip->eoi)
            current_board->irqchip->eoi(irq);
        return;
    }
    irq_active[irq]++;
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);

    if (current_board && current_board->irqchip &&
        current_board->irqchip->ack)
        current_board->irqchip->ack();

    handler((int)irq, priv);
    __atomic_sub_fetch(&irq_active[irq], 1, __ATOMIC_RELEASE);

    if (current_board && current_board->irqchip &&
        current_board->irqchip->eoi)
        current_board->irqchip->eoi(irq);
}

void *dma_alloc_coherent(size_t size, uint64_t *dma_handle) {
    extern void *kmalloc(size_t);
    void *ptr = kmalloc(size);
    if (ptr) {
        extern void *memset(void *, int, size_t);
        memset(ptr, 0, size);
    }
    if (dma_handle)
        *dma_handle = ptr ? (uint64_t)va_to_pa(ptr) : 0;
    return ptr;
}

void dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle) {
    (void)size;
    (void)dma_handle;
    extern void kfree(void *);
    kfree(vaddr);
}

static int dma_page_order(size_t size, size_t alignment)
{
    size_t bytes = PAGE_SIZE;
    int order = 0;
    if (alignment < PAGE_SIZE)
        alignment = PAGE_SIZE;
    while ((bytes < size || bytes < alignment) && order < MAX_ORDER) {
        bytes <<= 1;
        order++;
    }
    return (bytes < size || bytes < alignment) ? -1 : order;
}

void *dma_alloc_coherent_aligned(size_t size, size_t alignment,
                                 uint64_t *dma_handle)
{
    if (!size || !alignment || alignment > PAGE_SIZE ||
        (alignment & (alignment - 1U)))
        return NULL;
    int order = dma_page_order(size, alignment);
    if (order < 0)
        return NULL;
    pfn_t pfn = pfa_alloc(order);
    if (pfn == PFN_NONE)
        return NULL;
    void *ptr = pfn_to_virt(pfn);
    if (!ptr) {
        pfa_free(pfn, order);
        return NULL;
    }
    extern void *memset(void *, int, size_t);
    memset(ptr, 0, PAGE_SIZE << order);
    if (dma_handle)
        *dma_handle = pfn_to_phys(pfn);
    return ptr;
}

void dma_free_coherent_aligned(void *vaddr, size_t size, uint64_t dma_handle)
{
    (void)dma_handle;
    if (!vaddr || !size)
        return;
    int order = dma_page_order(size, PAGE_SIZE);
    pfn_t pfn = virt_to_pfn(vaddr);
    if (order >= 0 && pfn != PFN_NONE)
        pfa_free(pfn, order);
}

void dma_sync_for_device(void *vaddr, size_t size) {
    if (vaddr && size)
        arch_dma_sync_for_device(vaddr, size);
}

void dma_sync_for_cpu(void *vaddr, size_t size) {
    if (vaddr && size)
        arch_dma_sync_for_cpu(vaddr, size);
}

uint64_t clock_get_ticks(void) {
    if (current_board && current_board->timer &&
        current_board->timer->read_ticks)
        return current_board->timer->read_ticks();
    return 0;
}

uint64_t clock_ticks_per_sec(void) {
    if (current_board && current_board->timer &&
        current_board->timer->ticks_per_sec)
        return current_board->timer->ticks_per_sec();
    return 10000000;
}

void udelay(unsigned usecs) {
    if (current_board && current_board->timer &&
        current_board->timer->read_ticks) {
        uint64_t freq = clock_ticks_per_sec();
        uint64_t wait = (freq / 1000000ULL) * usecs;
        uint64_t start = current_board->timer->read_ticks();
        while ((current_board->timer->read_ticks() - start) < wait)
            ;
    }
}

void mdelay(unsigned msecs) {
    udelay(msecs * 1000);
}
