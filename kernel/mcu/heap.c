#include "mm/slab.h"
#include "core/arch.h"
#include "core/string.h"
#include "heap_backend.h"
#include "heap.h"
#ifdef CONFIG_MCU_SHARED_KERNEL
#include "shared_kernel.h"
#define heap_lock() mcu_fabric_lock(A20_FABRIC_HEAP_GATE)
#define heap_unlock(flags) mcu_fabric_unlock(A20_FABRIC_HEAP_GATE, flags)
#else
#define heap_lock() arch_irq_save()
#define heap_unlock(flags) arch_irq_restore(flags)
#endif

typedef struct mcu_heap_block {
    size_t size;
    struct mcu_heap_block *next;
    uint32_t free;
} mcu_heap_block_t;

extern char _heap_start[];
extern char _heap_end[];

static mcu_heap_block_t *heap_head;
static uintptr_t heap_start_addr;
static uintptr_t heap_end_addr;

static size_t align8(size_t value) {
    return (value + 7U) & ~(size_t)7U;
}

void mcu_heap_init(void) {
    uintptr_t start = align8((uintptr_t)_heap_start);
    uintptr_t end = (uintptr_t)_heap_end & ~(uintptr_t)7U;
    heap_start_addr = start;
    heap_end_addr = end;
    heap_head = NULL;
    if (end <= start + sizeof(mcu_heap_block_t))
        return;
    heap_head = (mcu_heap_block_t *)start;
    heap_head->size = end - start - sizeof(*heap_head);
    heap_head->next = NULL;
    heap_head->free = 1;
}

size_t mcu_heap_available(void) {
    mcu_heap_stats_t stats;
    mcu_heap_get_stats(&stats);
    size_t total = stats.free_bytes;
#ifndef CONFIG_MCU_SHARED_KERNEL
    total += arch_mcu_heap_available();
#endif
    return total;
}

void mcu_heap_get_stats(mcu_heap_stats_t *stats) {
    if (!stats)
        return;

    stats->arena_bytes =
        heap_end_addr > heap_start_addr ? heap_end_addr - heap_start_addr : 0;
    stats->free_bytes = 0;
    stats->largest_free_bytes = 0;

    uint32_t flags = heap_lock();
    for (mcu_heap_block_t *b = heap_head; b; b = b->next) {
        if (!b->free)
            continue;
        stats->free_bytes += b->size;
        if (b->size > stats->largest_free_bytes)
            stats->largest_free_bytes = b->size;
    }
    heap_unlock(flags);
    stats->used_bytes = stats->arena_bytes - stats->free_bytes;
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;
    size = align8(size);
    uint32_t flags = heap_lock();
    for (mcu_heap_block_t *b = heap_head; b; b = b->next) {
        if (!b->free || b->size < size)
            continue;
        if (b->size >= size + sizeof(*b) + 8U) {
            mcu_heap_block_t *tail =
                (mcu_heap_block_t *)((char *)(b + 1) + size);
            tail->size = b->size - size - sizeof(*tail);
            tail->next = b->next;
            tail->free = 1;
            b->next = tail;
            b->size = size;
        }
        b->free = 0;
        heap_unlock(flags);
        return b + 1;
    }
    heap_unlock(flags);
#ifdef CONFIG_MCU_SHARED_KERNEL
    return NULL;
#else
    return arch_mcu_heap_alloc(size);
#endif
}

void kfree(void *ptr) {
    if (!ptr)
        return;
#ifndef CONFIG_MCU_SHARED_KERNEL
    if (arch_mcu_heap_owns(ptr)) {
        arch_mcu_heap_free(ptr);
        return;
    }
#endif
    uintptr_t value = (uintptr_t)ptr;
    if (value < heap_start_addr + sizeof(mcu_heap_block_t) ||
        value >= heap_end_addr)
        return;
    uint32_t flags = heap_lock();
    mcu_heap_block_t *block = (mcu_heap_block_t *)ptr - 1;
    block->free = 1;
    for (mcu_heap_block_t *b = heap_head; b && b->next; ) {
        if (b->free && b->next->free) {
            b->size += sizeof(*b) + b->next->size;
            b->next = b->next->next;
        } else {
            b = b->next;
        }
    }
    heap_unlock(flags);
}

void *kcalloc(size_t nmemb, size_t size) {
    if (size && nmemb > SIZE_MAX / size)
        return NULL;
    size_t total = nmemb * size;
    void *ptr = kmalloc(total);
    if (ptr)
        memset(ptr, 0, total);
    return ptr;
}

void *krealloc(void *ptr, size_t size) {
    if (!ptr)
        return kmalloc(size);
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }
    size_t old_size;
#ifndef CONFIG_MCU_SHARED_KERNEL
    if (arch_mcu_heap_owns(ptr)) {
        old_size = arch_mcu_heap_allocation_size(ptr);
    } else
#endif
    {
        uintptr_t value = (uintptr_t)ptr;
        if (value < heap_start_addr + sizeof(mcu_heap_block_t) ||
            value >= heap_end_addr)
            return NULL;
        old_size = ((mcu_heap_block_t *)ptr - 1)->size;
    }
    if (old_size >= size)
        return ptr;
    void *new_ptr = kmalloc(size);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
    return new_ptr;
}

void slab_init(void) {}
void slab_get_stats(slab_stats_t *stats) {
    if (stats)
        memset(stats, 0, sizeof(*stats));
}
size_t slab_reclaim_spare(void) { return 0; }
