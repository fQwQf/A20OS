#include "mm/slab.h"
#include "core/arch.h"
#include "core/string.h"

typedef struct mcu_heap_block {
    size_t size;
    struct mcu_heap_block *next;
    uint32_t free;
} mcu_heap_block_t;

extern char _heap_start[];
extern char _heap_end[];

static mcu_heap_block_t *heap_head;

static size_t align8(size_t value) {
    return (value + 7U) & ~(size_t)7U;
}

void mcu_heap_init(void) {
    uintptr_t start = align8((uintptr_t)_heap_start);
    uintptr_t end = (uintptr_t)_heap_end & ~(uintptr_t)7U;
    heap_head = NULL;
    if (end <= start + sizeof(mcu_heap_block_t))
        return;
    heap_head = (mcu_heap_block_t *)start;
    heap_head->size = end - start - sizeof(*heap_head);
    heap_head->next = NULL;
    heap_head->free = 1;
}

size_t mcu_heap_available(void) {
    size_t total = 0;
    uint32_t flags = arch_irq_save();
    for (mcu_heap_block_t *b = heap_head; b; b = b->next)
        if (b->free)
            total += b->size;
    arch_irq_restore(flags);
    return total;
}

void *kmalloc(size_t size) {
    if (size == 0)
        return NULL;
    size = align8(size);
    uint32_t flags = arch_irq_save();
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
        arch_irq_restore(flags);
        return b + 1;
    }
    arch_irq_restore(flags);
    return NULL;
}

void kfree(void *ptr) {
    if (!ptr)
        return;
    uint32_t flags = arch_irq_save();
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
    arch_irq_restore(flags);
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
    mcu_heap_block_t *block = (mcu_heap_block_t *)ptr - 1;
    if (block->size >= size)
        return ptr;
    void *new_ptr = kmalloc(size);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, block->size);
    kfree(ptr);
    return new_ptr;
}

void slab_init(void) {}
void slab_get_stats(slab_stats_t *stats) {
    if (stats)
        memset(stats, 0, sizeof(*stats));
}
size_t slab_reclaim_spare(void) { return 0; }
