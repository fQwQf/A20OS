#include "core/trace.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/driver_core.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "core/string.h"
#include "mm/slab.h"
#ifdef CONFIG_IOPORT
#include "cpu.h"
#endif

extern const board_config_t *const current_board;

static irq_handler_t irq_handlers[256];
static void         *irq_priv[256];
static unsigned long irq_flags[256];
static unsigned int irq_active[256];
static spinlock_t irq_table_lock = SPINLOCK_INIT;

/* IRQ_SHARED_CHAIN_MODEL:
 * - A line whose primary registration and the new request both carry
 *   IRQF_SHARED accepts additional handlers on a singly linked chain.
 * - Nodes are allocated before taking irq_table_lock so no allocation
 *   happens inside the spinlock; unlink runs under the lock, and the
 *   freed node is only released after irq_active[irq] drains, so a
 *   dispatch that already snapshotted the chain head finishes its walk
 *   before any node memory is reused.
 * - Dispatch invokes the primary handler first, then every chain node,
 *   because a level-triggered shared line stays asserted until ALL
 *   devices on it have cleared their interrupt source. */
typedef struct irq_shared_node {
    irq_handler_t handler;
    void *priv;
    unsigned long flags;
    struct irq_shared_node *next;
} irq_shared_node_t;

static irq_shared_node_t *irq_shared_chain[256];

/* See IRQ_TRIGGER_MODEL in driver_hwapi.h.  A platform that knows which of
 * its lines are level-triggered overrides this; the default reports edge so
 * dispatch never masks a line whose platform did not say it was safe. */
int __attribute__((weak)) arch_irq_line_is_level(uint32_t irq)
{
    (void)irq;
    return 0;
}

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

uint32_t ioport_read32(uint16_t port)
{
#ifdef CONFIG_IOPORT
    return inl(port);
#else
    (void)port;
    return 0xffffffffU;
#endif
}

void ioport_write32(uint16_t port, uint32_t value)
{
#ifdef CONFIG_IOPORT
    outl(port, value);
#else
    (void)port;
    (void)value;
#endif
}

/* DRIVER_IRQ_TABLE_FIXED_LIMIT: platform IRQ lines are capped at 256 until the
 * irq registry is replaced by a dynamically sized irqdomain-style structure. */

/*
 * A message-signalled interrupt needs a line that nothing else is using, and
 * the line number is the vector the device puts in the message -- so unlike
 * INTx there is no hardware routing step that could pick a free one.  The
 * allocator below hands out a contiguous block from the window the platform
 * declares, and irq_free_vectors() gives it back.
 *
 * The window is a weak arch hook rather than a constant: a platform with no
 * message-signalled path reports an empty range, which makes every allocation
 * fail and leaves callers on their existing fallback.
 */
int __attribute__((weak)) arch_irq_msix_vector_range(int *base, int *end)
{
    *base = 0;
    *end = 0;
    return -EOPNOTSUPP;
}

static uint8_t g_irq_vector_map[256];

int irq_alloc_vectors(unsigned count)
{
    if (count == 0 || count > 256)
        return -EINVAL;

    int base, end;
    if (arch_irq_msix_vector_range(&base, &end) < 0)
        return -EOPNOTSUPP;
    if (base < 0 || end > 256 || end <= base)
        return -EINVAL;

    uint64_t lock_flags = spin_lock_irqsave(&irq_table_lock);
    int first = -1;
    for (int irq = base; irq + (int)count <= end; irq++) {
        int free_run = 0;
        for (int i = 0; i < (int)count; i++) {
            if (g_irq_vector_map[irq + i]) {
                free_run = 0;
                break;
            }
            free_run++;
        }
        if (free_run == (int)count) {
            for (int i = 0; i < (int)count; i++)
                g_irq_vector_map[irq + i] = 1;
            first = irq;
            break;
        }
    }
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);

    if (first < 0)
        return -ENOSPC;
    return first;
}

void irq_free_vectors(uint32_t first, unsigned count)
{
    uint64_t lock_flags = spin_lock_irqsave(&irq_table_lock);
    for (unsigned i = 0; i < count; i++) {
        if ((uint64_t)first + i >= 256)
            break;
        g_irq_vector_map[first + i] = 0;
    }
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);
}

int request_irq(uint32_t irq, irq_handler_t handler,
                unsigned long flags, void *priv) {
    if (irq >= 256 || !handler)
        return -EINVAL;

    /* Allocate the shared-chain node outside the spinlock; it is released
     * below when the fast path does not need it. */
    irq_shared_node_t *node = NULL;
    if (flags & IRQF_SHARED) {
        extern void *kmalloc(size_t);
        node = (irq_shared_node_t *)kmalloc(sizeof(*node));
        if (!node)
            return -ENOMEM;
    }

    uint64_t lock_flags = spin_lock_irqsave(&irq_table_lock);
    if (irq_handlers[irq]) {
        int shareable = (irq_flags[irq] & IRQF_SHARED) &&
                        (flags & IRQF_SHARED);
        if (!shareable) {
            spin_unlock_irqrestore(&irq_table_lock, lock_flags);
            extern void kfree(void *);
            if (node)
                kfree(node);
            return -EBUSY;
        }
        node->handler = handler;
        node->priv    = priv;
        node->flags   = flags;
        node->next    = NULL;
        irq_shared_node_t **tail = &irq_shared_chain[irq];
        while (*tail)
            tail = &(*tail)->next;
        *tail = node;
        spin_unlock_irqrestore(&irq_table_lock, lock_flags);
        return 0;
    }
    irq_handlers[irq] = handler;
    irq_priv[irq]     = priv;
    irq_flags[irq]    = flags;
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);

    if (node) {
        extern void kfree(void *);
        kfree(node);
    }

    if (!(flags & IRQF_NO_AUTO_ENABLE))
        irq_enable(irq);

    return 0;
}

int synchronize_irq(uint32_t irq)
{
    if (irq >= 256)
        return -EINVAL;
    while (__atomic_load_n(&irq_active[irq], __ATOMIC_ACQUIRE) != 0)
        __asm__ volatile("" ::: "memory");
    return 0;
}

void free_irq(uint32_t irq, void *priv) {
    if (irq >= 256)
        return;
    extern void kfree(void *);
    irq_shared_node_t *dead = NULL;
    uint64_t lock_flags = spin_lock_irqsave(&irq_table_lock);
    if (irq_handlers[irq] && irq_priv[irq] == priv) {
        irq_disable(irq);
        irq_handlers[irq] = NULL;
        irq_priv[irq]     = NULL;
        irq_flags[irq]    = 0;
        /* Promote the first shared handler to the primary slot so the
         * remaining devices on the line keep working. */
        irq_shared_node_t *head = irq_shared_chain[irq];
        if (head) {
            irq_shared_chain[irq] = head->next;
            irq_handlers[irq] = head->handler;
            irq_priv[irq]     = head->priv;
            irq_flags[irq]    = head->flags;
            irq_enable(irq);
            dead = head;
        }
    } else {
        irq_shared_node_t **link = &irq_shared_chain[irq];
        while (*link && (*link)->priv != priv)
            link = &(*link)->next;
        if (!*link) {
            spin_unlock_irqrestore(&irq_table_lock, lock_flags);
            return;
        }
        irq_shared_node_t *node = *link;
        *link = node->next;
        dead = node;
        if (!irq_handlers[irq] && !irq_shared_chain[irq])
            irq_disable(irq);
    }
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);

    /* A handler that took its snapshot before the table entry was cleared may
     * still be running on another CPU.  Driver remove cannot release its
     * private state until that invocation has returned. */
    synchronize_irq(irq);
    /* An older level dispatch may have rearmed before its EOI completed.
     * Leave an empty line masked after that dispatch has fully drained. */
    lock_flags = spin_lock_irqsave(&irq_table_lock);
    if (!irq_handlers[irq])
        irq_disable(irq);
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);
    if (dead)
        kfree(dead);
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
    irq_shared_node_t *shared = irq_shared_chain[irq];
    unsigned long flags = irq_flags[irq];
    if (!handler) {
        spin_unlock_irqrestore(&irq_table_lock, lock_flags);
        if (current_board && current_board->irqchip &&
            current_board->irqchip->eoi)
            current_board->irqchip->eoi(irq);
        return;
    }
    __atomic_add_fetch(&irq_active[irq], 1, __ATOMIC_RELAXED);
    spin_unlock_irqrestore(&irq_table_lock, lock_flags);

    /*
     * Mask a level-triggered line for the duration of its handlers.  The
     * source is still asserted at this point (that is why the interrupt was
     * delivered), so leaving the line armed would let it re-enter as soon as
     * any return path enables interrupts.  IRQF_ONESHOT leaves the mask in
     * place: the handler is only a wake-up, and the thread that clears the
     * source is responsible for irq_enable().  See IRQ_TRIGGER_MODEL.
     */
    int oneshot = (flags & IRQF_ONESHOT) != 0;
    int level = oneshot || (flags & IRQF_LEVEL) != 0 ||
                arch_irq_line_is_level(irq);
    if (level)
        irq_disable(irq);

    if (current_board && current_board->irqchip &&
        current_board->irqchip->ack)
        current_board->irqchip->ack();

    a20_trace_token_t span = a20_trace_begin(A20_TRACE_IRQ, irq, 0);
    handler((int)irq, priv);
    /* Shared handlers: free_irq() unlinks under the table lock and only
     * releases node memory after irq_active[irq] drains, so this lockless
     * walk either sees a node or a stale-but-still-valid one. */
    while (shared) {
        irq_shared_node_t *next = shared->next;
        shared->handler((int)irq, shared->priv);
        shared = next;
    }
    a20_trace_end(span, 0);
    if (level && !oneshot)
        irq_enable(irq);

    if (current_board && current_board->irqchip &&
        current_board->irqchip->eoi)
        current_board->irqchip->eoi(irq);
    __atomic_sub_fetch(&irq_active[irq], 1, __ATOMIC_RELEASE);
}

/* DRIVER_DMA_MASK_MODEL (implementation side): a mask is a run of low-order
 * ones, so its complement is a run of high-order ones.  dma_set_mask() checks
 * that shape instead of rounding: a driver that wrote 0xffffffff80000000 by
 * accident would otherwise get a window it never asked for, and the allocator
 * would happily hand out memory the device cannot decode.
 *
 * The property is tested on @mask itself.  A run of low-order ones is exactly
 * the value x with x & (x + 1) == 0, and running that test on ~mask instead
 * asks for the opposite shape: it rejected every legitimate mask shorter than
 * 64 bits -- DMA_MASK_32BIT among them, which is the only mask any driver in
 * this tree asks for besides the 64-bit one -- and accepted masks that are a
 * run of high-order ones, the half-declared window this function exists to
 * refuse. */
static int dma_mask_well_formed(uint64_t mask)
{
    if (!mask)
        return 0;
    return (mask & (mask + 1U)) == 0;
}
/* Non-coherent architectures may require a permission-checked cache handoff
 * before any driver can own DMA memory. An absent hook preserves the existing
 * coherent-platform contract; an explicit refusal must fail before allocation. */
extern int arch_device_dma_ready(void) __attribute__((weak));
static int dma_arch_ready(void) {
    return !arch_device_dma_ready || arch_device_dma_ready();
}

int dma_set_mask(struct device *dev, uint64_t mask)
{
    if (!dev)
        return -EINVAL;
    if (!dma_mask_well_formed(mask))
        return -EINVAL;
    if (!dma_arch_ready())
        return -EOPNOTSUPP;
    dev->dma_mask = mask;
    return 0;
}

uint64_t dma_get_mask(const struct device *dev)
{
    /* 0 is the undeclared state every zero-initialised device_t carries; it
     * means the full window, not an empty one. */
    if (!dev || !dev->dma_mask)
        return DMA_MASK_64BIT;
    return dev->dma_mask;
}

static int dma_range_ok_mask(uint64_t mask, uint64_t addr, size_t size)
{
    if (!size)
        return 1;
    if (addr & ~mask)
        return 0;
    uint64_t last = addr + (uint64_t)size - 1U;
    if (last < addr)          /* the range wraps 64 bits: never addressable */
        return 0;
    return (last & ~mask) == 0;
}

int dma_addr_ok(const struct device *dev, uint64_t addr)
{
    return dma_range_ok_mask(dma_get_mask(dev), addr, 1);
}

int dma_range_ok(const struct device *dev, uint64_t addr, size_t size)
{
    return dma_range_ok_mask(dma_get_mask(dev), addr, size);
}

static int dma_page_order(size_t size, size_t alignment);

static void *dma_alloc_pages(uint64_t mask, size_t size, size_t alignment,
                             uint64_t *dma_handle)
{
    int order = dma_page_order(size, alignment);
    if (order < 0)
        return NULL;
    /* Freeing and retrying the unrestricted allocator can return the same
     * high block forever, despite free low memory. Select within the mask. */
    pfn_t pfn = mask == DMA_MASK_64BIT ? pfa_alloc(order)
                                     : pfa_alloc_max(order, mask);
    if (pfn == PFN_NONE)
        return NULL;
    void *ptr = pfn_to_virt(pfn);
    uint64_t pa = pfn_to_phys(pfn);
    if (!ptr || !dma_range_ok_mask(mask, pa, PAGE_SIZE << order)) {
        pfa_free(pfn, order);
        return NULL;
    }
    extern void *memset(void *, int, size_t);
    memset(ptr, 0, PAGE_SIZE << order);
    if (dma_handle)
        *dma_handle = pa;
    return ptr;
}

void *dma_alloc_coherent(struct device *dev, size_t size, uint64_t *dma_handle)
{
    if (dma_handle)
        *dma_handle = 0;
    if (!size || !dma_arch_ready())
        return NULL;
    extern void *kmalloc(size_t);
    void *ptr = kmalloc(size);
    if (ptr) {
        extern void *memset(void *, int, size_t);
        memset(ptr, 0, size);
        uint64_t pa = va_to_pa(ptr);
        if (dma_range_ok_mask(dma_get_mask(dev), pa, size)) {
            if (dma_handle)
                *dma_handle = pa;
            return ptr;
        }
        /* The slab allocator picks the address, so there is nothing to retry:
         * if the block is outside the window, hand it back and ask the frame
         * allocator for something physically inside it instead. */
        extern void kfree(void *);
        kfree(ptr);
    }
    return dma_alloc_pages(dma_get_mask(dev), size, PAGE_SIZE, dma_handle);
}

void dma_free_coherent(void *vaddr, size_t size, uint64_t dma_handle) {
    if (!vaddr)
        return;
    /* dma_alloc_coherent() has two provenances -- a kmalloc block on the fast
     * path, a raw frame-allocator page block when the slab block landed outside
     * the mask -- and only the pointer comes back here.  Ask the slab who owns
     * it rather than guessing from alignment or from the handle: kmalloc() also
     * returns page-aligned blocks (big allocs), so neither test distinguishes
     * them, and a frame-allocator block handed to kfree() panics. */
    extern int kmalloc_owns(const void *ptr);
    if (kmalloc_owns(vaddr)) {
        extern void kfree(void *);
        kfree(vaddr);
        return;
    }
    dma_free_coherent_aligned(vaddr, size, dma_handle);
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

/* An allocation owner, independent of mappings, requests and driver binding.
 * Preserve the physical allocator's compact PFN and actual order for put. */
struct driver_dma_buffer {
    refcount_t references;
    struct device *device;
    pfn_t pfn;
    int order;
    void *data;
    uint64_t address;
    size_t size;
};

int driver_dma_buffer_create(struct device *device, size_t size, size_t alignment,
                              struct driver_dma_buffer **out)
{
    if (!out) return -EINVAL;
    *out = NULL;
    if (!device || !size || !alignment || (alignment & (alignment - 1U)))
        return -EINVAL;
    size_t maximum = (size_t)PAGE_SIZE << MAX_ORDER;
    if (size > maximum || alignment > maximum) return -E2BIG;
    uint64_t mask = dma_get_mask(device);
    if (!dma_mask_well_formed(mask)) return -EINVAL;
    if (!dma_arch_ready()) return -EOPNOTSUPP;
    if (!device_is_registered(device) || device_is_stopping(device)) return -ENODEV;
    struct device *owner = device_get(device);
    if (!owner) return -ENODEV;
    struct driver_dma_buffer *buffer = kmalloc(sizeof(*buffer));
    if (!buffer) { device_put(owner); return -ENOMEM; }
    int order = dma_page_order(size, alignment);
    pfn_t pfn = mask == DMA_MASK_64BIT ? pfa_alloc(order) : pfa_alloc_max(order, mask);
    if (pfn == PFN_NONE) { kfree(buffer); device_put(owner); return -ENOMEM; }
    void *data = pfn_to_virt(pfn);
    uint64_t address = pfn_to_phys(pfn);
    size_t bytes = (size_t)PAGE_SIZE << order;
    int ret = 0;
    if (!data || (address & (alignment - 1U)) || !dma_range_ok_mask(mask, address, bytes))
        ret = -ERANGE;
    else if (!device_is_registered(device) || device_is_stopping(device)) ret = -ENODEV;
    else if (dma_get_mask(device) != mask) ret = -EAGAIN;
    if (ret) {
        pfa_free(pfn, order);
        kfree(buffer);
        device_put(owner);
        return ret;
    }
    memset(data, 0, bytes);
    refcount_set(&buffer->references, 1);
    buffer->device = owner;
    buffer->pfn = pfn;
    buffer->order = order;
    buffer->data = data;
    buffer->address = address;
    buffer->size = size;
    *out = buffer;
    return 0;
}

void driver_dma_buffer_get(struct driver_dma_buffer *buffer)
{
    if (buffer) refcount_inc(&buffer->references);
}

void driver_dma_buffer_put(struct driver_dma_buffer *buffer)
{
    if (!buffer || !refcount_dec_and_test(&buffer->references)) return;
    struct device *device = buffer->device;
    pfa_free(buffer->pfn, buffer->order);
    kfree(buffer);
    device_put(device);
}

void *driver_dma_buffer_data(const struct driver_dma_buffer *buffer)
{
    return buffer ? buffer->data : NULL;
}

uint64_t driver_dma_buffer_address(const struct driver_dma_buffer *buffer)
{
    return buffer ? buffer->address : 0;
}

size_t driver_dma_buffer_size(const struct driver_dma_buffer *buffer)
{
    return buffer ? buffer->size : 0;
}

void *dma_alloc_coherent_aligned(struct device *dev, size_t size,
                                 size_t alignment, uint64_t *dma_handle)
{
    if (dma_handle)
        *dma_handle = 0;
    if (!size || !dma_arch_ready() || !alignment || alignment > PAGE_SIZE ||
        (alignment & (alignment - 1U)))
        return NULL;
    return dma_alloc_pages(dma_get_mask(dev), size, alignment, dma_handle);
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

/* Exercises the actual frame allocator and direct-map aliases, without
 * submitting DMA to hardware. Keep all buffers live to catch alias reuse. */
int a20_boot_selftest_dma_window(void)
{
    struct device test_device = {0};
    void *buffers[32] = {0};
    uint64_t addresses[32] = {0};
    const size_t size = 64U << 10;
    int result = dma_set_mask(&test_device, DMA_MASK_32BIT);
    if (result)
        return result;
    for (unsigned i = 0; i < 32; i++) {
        buffers[i] = dma_alloc_coherent_aligned(&test_device, size, PAGE_SIZE, &addresses[i]);
        if (!buffers[i] || !dma_range_ok(&test_device, addresses[i], size)) {
            result = -ENOMEM;
            break;
        }
        for (unsigned j = 0; j < i; j++)
            if (addresses[j] == addresses[i])
                result = -EIO;
        ((volatile uint32_t *)buffers[i])[0] = 0xa2000000U + i;
    }
    for (unsigned i = 0; i < 32; i++) {
        if (!buffers[i])
            continue;
        if (((volatile uint32_t *)buffers[i])[0] != 0xa2000000U + i)
            result = -EIO;
        dma_free_coherent_aligned(buffers[i], size, addresses[i]);
    }
    return result;
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
