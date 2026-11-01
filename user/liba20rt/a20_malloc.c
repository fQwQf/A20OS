/*
 * A20 Native SDK allocator.
 *
 * Backed by the common vm_alloc interface.  This is a real first-fit
 * allocator rather than the old "one vm_alloc per object" version: on a
 * 352 KiB part each extra page of alignment slack matters, and CMSIS creates
 * a dozen small control blocks before it creates a single thread.
 *
 * Regions come from vm_alloc and are never returned (the flat model has no
 * per-object unmap in this profile); freed chunks go on a free list and are
 * reused.  The allocator is serialized by one word, because the CMSIS backend
 * calls it from every thread and from the timer service.
 */
#include "a20_mem.h"
#include "a20_string.h"
#include "a20_sync.h"
#include "a20_task.h"

#define HEAP_ALIGN 16u
#define HEAP_GROW_MIN (16u * 1024u)

typedef struct a20_region {
    uint64_t size;
    uint64_t used;
    struct a20_region *next;
} __attribute__((aligned(HEAP_ALIGN))) a20_region_t;

typedef struct a20_chunk {
    uint64_t size;
    struct a20_chunk *next;
} __attribute__((aligned(HEAP_ALIGN))) a20_chunk_t;

static a20_region_t *a20_regions;
static a20_chunk_t *a20_free_list;
static uint32_t a20_heap_lock;

static void heap_lock(void)
{
    for (;;) {
        uint32_t exchanged = 0;
        if (a20_atomic32(&a20_heap_lock, A20_ATOMIC32_COMPARE_EXCHANGE, 0, 1, 0,
                         &exchanged) == A20_OK &&
            exchanged)
            return;
        (void)a20_thread_yield();
    }
}

static void heap_unlock(void)
{
    (void)a20_atomic32_store(&a20_heap_lock, 0);
}

static uint64_t align_up(uint64_t value)
{
    return (value + HEAP_ALIGN - 1U) & ~(uint64_t)(HEAP_ALIGN - 1U);
}

static a20_region_t *grow(uint64_t payload)
{
    uint64_t need = payload + sizeof(a20_region_t) + sizeof(a20_chunk_t);
    uint64_t size = need > HEAP_GROW_MIN ? align_up(need) : HEAP_GROW_MIN;
    if (size > (uint64_t)UINTPTR_MAX - 4095)
        return (void *)0;
    a20_vm_alloc_args_t args = {.size = sizeof(args),
                                .version = 1,
                                .length = size,
                                .prot = A20_PROT_READ | A20_PROT_WRITE,
                                .flags = 0x20};
    if (a20_vm_alloc(&args) < 0 || !args.out_addr)
        return (void *)0;
    a20_region_t *r = (a20_region_t *)(uintptr_t)args.out_addr;
    r->size = size;
    r->used = sizeof(*r);
    r->next = a20_regions;
    a20_regions = r;
    return r;
}

void *a20_malloc(uint64_t size)
{
    if (!size || size > (uint64_t)UINTPTR_MAX - HEAP_ALIGN)
        return (void *)0;
    size = align_up(size);

    heap_lock();
    a20_chunk_t **pp = &a20_free_list;
    while (*pp) {
        a20_chunk_t *c = *pp;
        if (c->size >= size) {
            *pp = c->next;
            /* Split when the remainder can hold another header plus a
             * minimum allocation. */
            if (c->size >= size + sizeof(a20_chunk_t) + HEAP_ALIGN) {
                a20_chunk_t *tail =
                    (a20_chunk_t *)((uint8_t *)(c + 1) + size);
                tail->size = c->size - size - sizeof(a20_chunk_t);
                tail->next = a20_free_list;
                a20_free_list = tail;
                c->size = size;
            }
            heap_unlock();
            return c + 1;
        }
        pp = &c->next;
    }

    for (a20_region_t *r = a20_regions; r; r = r->next) {
        if (r->used + size + sizeof(a20_chunk_t) <= r->size) {
            a20_chunk_t *c = (a20_chunk_t *)((uint8_t *)r + r->used);
            c->size = size;
            r->used += size + sizeof(a20_chunk_t);
            heap_unlock();
            return c + 1;
        }
    }

    a20_region_t *r = grow(size);
    if (!r) {
        heap_unlock();
        return (void *)0;
    }
    a20_chunk_t *c = (a20_chunk_t *)((uint8_t *)r + r->used);
    c->size = size;
    r->used += size + sizeof(a20_chunk_t);
    heap_unlock();
    return c + 1;
}

void a20_free(void *ptr)
{
    if (!ptr)
        return;
    a20_chunk_t *c = (a20_chunk_t *)ptr - 1;
    heap_lock();
    c->next = a20_free_list;
    a20_free_list = c;
    heap_unlock();
}

void *a20_calloc(uint64_t nmemb, uint64_t size)
{
    uint64_t total;
    if (__builtin_mul_overflow(nmemb, size, &total))
        return (void *)0;
    void *p = a20_malloc(total);
    if (p)
        a20_memset(p, 0, (size_t)total);
    return p;
}

void *a20_realloc(void *ptr, uint64_t size)
{
    if (!ptr)
        return a20_malloc(size);
    if (!size) {
        a20_free(ptr);
        return (void *)0;
    }
    a20_chunk_t *c = (a20_chunk_t *)ptr - 1;
    uint64_t old = c->size;
    if (old >= size)
        return ptr;
    void *fresh = a20_malloc(size);
    if (!fresh)
        return (void *)0;
    a20_memcpy(fresh, ptr, (size_t)old);
    a20_free(ptr);
    return fresh;
}
