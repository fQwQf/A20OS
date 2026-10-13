/* A20 Native SDK allocator backed by the common vm_alloc interface.
 * Free blocks stay mapped for reuse until task exit. Callers must serialize
 * allocator calls; this small runtime does not provide a threaded malloc. */
#include "a20_mem.h"
#include "a20_string.h"

struct a20_alloc_block {
    uint64_t size;
    uint64_t used;
    struct a20_alloc_block *next;
};

static struct a20_alloc_block *a20_heap_head;
#define A20_HEAP_ALIGN 16u
#define A20_HEAP_HEADER ((sizeof(struct a20_alloc_block) + 15u) & ~(size_t)15u)

static int a20_aligned_size(uint64_t size, uint64_t *out) {
    if (!size || size > (uint64_t)UINTPTR_MAX - (A20_HEAP_ALIGN - 1))
        return 0;
    *out = (size + A20_HEAP_ALIGN - 1) & ~(uint64_t)(A20_HEAP_ALIGN - 1);
    return 1;
}

static struct a20_alloc_block *a20_grow_heap(uint64_t min_size) {
    if (min_size > (uint64_t)UINTPTR_MAX - A20_HEAP_HEADER - 4095)
        return (void *)0;
    uint64_t needed = (min_size + A20_HEAP_HEADER + 4095) & ~4095ULL;
    a20_vm_alloc_args_t args = {.size = sizeof(args),
                                .version = 1,
                                .length = needed,
                                .prot = A20_PROT_READ | A20_PROT_WRITE,
                                .flags = 0x20};
    if (a20_vm_alloc(&args) < 0 || !args.out_addr)
        return (void *)0;
    struct a20_alloc_block *blk = (void *)(uintptr_t)args.out_addr;
    blk->size = needed - A20_HEAP_HEADER;
    blk->used = 0;
    blk->next = a20_heap_head;
    a20_heap_head = blk;
    return blk;
}

void *a20_malloc(uint64_t size) {
    if (!a20_aligned_size(size, &size))
        return (void *)0;
    struct a20_alloc_block *blk = a20_heap_head;
    while (blk) {
        if (!blk->used && blk->size >= size) {
            blk->used = size;
            return (uint8_t *)blk + A20_HEAP_HEADER;
        }
        blk = blk->next;
    }
    blk = a20_grow_heap(size);
    if (!blk)
        return (void *)0;
    blk->used = size;
    return (uint8_t *)blk + A20_HEAP_HEADER;
}

void a20_free(void *ptr) {
    if (!ptr)
        return;
    struct a20_alloc_block *blk = (void *)((uint8_t *)ptr - A20_HEAP_HEADER);
    blk->used = 0;
}

void *a20_calloc(uint64_t nmemb, uint64_t size) {
    uint64_t total;
    if (__builtin_mul_overflow(nmemb, size, &total))
        return (void *)0;
    void *p = a20_malloc(total);
    if (p)
        a20_memset(p, 0, (size_t)total);
    return p;
}

void *a20_realloc(void *ptr, uint64_t size) {
    if (!ptr)
        return a20_malloc(size);
    if (!size) {
        a20_free(ptr);
        return (void *)0;
    }
    if (!a20_aligned_size(size, &size))
        return (void *)0;
    struct a20_alloc_block *blk = (void *)((uint8_t *)ptr - A20_HEAP_HEADER);
    uint64_t old_used = blk->used;
    if (blk->size >= size) {
        blk->used = size;
        return ptr;
    }
    void *new_ptr = a20_malloc(size);
    if (!new_ptr)
        return (void *)0;
    a20_memcpy(new_ptr, ptr, (size_t)(old_used < size ? old_used : size));
    a20_free(ptr);
    return new_ptr;
}
