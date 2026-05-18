/*
 * A20OS Native SDK — User-space memory allocator.
 *
 * Simple linked-list allocator backed by a20_vm_alloc.
 * Uses a page-granularity growth strategy with a free list.
 */
#include "a20_mem.h"
#include "a20_string.h"

struct a20_alloc_block {
    uint64_t size;
    uint64_t used;
    struct a20_alloc_block *next;
};

static struct a20_alloc_block *a20_heap_head;
static uint64_t a20_heap_end;
static const uint64_t A20_HEAP_ALIGN = 16;

static uint64_t a20_align_up(uint64_t v, uint64_t align)
{
    return (v + align - 1) & ~(align - 1);
}

static struct a20_alloc_block *a20_grow_heap(uint64_t min_size)
{
    uint64_t needed = a20_align_up(min_size + sizeof(struct a20_alloc_block), 4096);
    a20_vm_alloc_args_t args;
    args.size      = sizeof(args);
    args.version   = 1;
    args.addr_hint = 0;
    args.length    = needed;
    args.prot      = A20_PROT_READ | A20_PROT_WRITE;
    args.flags     = 0x20;
    args.out_addr  = 0;
    if (a20_vm_alloc(&args) < 0) return (void *)0;
    if (args.out_addr == 0) return (void *)0;

    struct a20_alloc_block *blk = (struct a20_alloc_block *)args.out_addr;
    blk->size = needed - sizeof(struct a20_alloc_block);
    blk->used = 0;
    blk->next = a20_heap_head;
    a20_heap_head = blk;
    return blk;
}

void *a20_malloc(uint64_t size)
{
    if (size == 0) return (void *)0;
    size = a20_align_up(size, A20_HEAP_ALIGN);

    struct a20_alloc_block *blk = a20_heap_head;
    while (blk) {
        if (!blk->used && blk->size >= size) {
            blk->used = size;
            return (void *)((uint64_t)blk + sizeof(struct a20_alloc_block));
        }
        blk = blk->next;
    }

    blk = a20_grow_heap(size);
    if (!blk) return (void *)0;
    blk->used = size;
    return (void *)((uint64_t)blk + sizeof(struct a20_alloc_block));
}

void a20_free(void *ptr)
{
    if (!ptr) return;
    struct a20_alloc_block *blk =
        (struct a20_alloc_block *)((uint64_t)ptr - sizeof(struct a20_alloc_block));
    blk->used = 0;
}

void *a20_calloc(uint64_t nmemb, uint64_t size)
{
    uint64_t total = nmemb * size;
    void *p = a20_malloc(total);
    if (p) a20_memset(p, 0, total);
    return p;
}

void *a20_realloc(void *ptr, uint64_t size)
{
    if (!ptr) return a20_malloc(size);
    if (size == 0) { a20_free(ptr); return (void *)0; }

    struct a20_alloc_block *blk =
        (struct a20_alloc_block *)((uint64_t)ptr - sizeof(struct a20_alloc_block));
    uint64_t old_used = blk->used;

    if (blk->size >= size) {
        blk->used = a20_align_up(size, A20_HEAP_ALIGN);
        return ptr;
    }

    void *new_ptr = a20_malloc(size);
    if (!new_ptr) return (void *)0;
    a20_memcpy(new_ptr, ptr, old_used < size ? old_used : size);
    a20_free(ptr);
    return new_ptr;
}
