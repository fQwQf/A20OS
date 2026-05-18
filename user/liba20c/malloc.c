/*
 * A20OS liba20c — malloc implementation.
 * Bump allocator with free list for liba20c Phase 3.
 */
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include "../liba20rt/a20_syscall.h"

struct malloc_block {
    uint64_t            size;
    uint64_t            used;
    struct malloc_block *next;
};

static struct malloc_block *free_list = NULL;
static void *arena_base = NULL;
static uint64_t arena_pos = 0;
static uint64_t arena_size = 0;

static void *arena_alloc(uint64_t size)
{
    size = (size + 15) & ~(uint64_t)15;
    if (arena_pos + size > arena_size) {
        uint64_t chunk = size > (256 * 1024) ? size : (256 * 1024);
        uint64_t req_size = arena_size + chunk;
        /* A20 vm_alloc: args = {size, flags, out_addr} */
        uint64_t args[3] = {req_size, 0, 0};
        int64_t r = a20_vm_alloc(args);
        if (r < 0) return NULL;
        if (arena_base == NULL) {
            arena_base = (void *)args[2];
            arena_size = req_size;
        } else {
            arena_size = req_size;
        }
    }
    void *p = (char *)arena_base + arena_pos;
    arena_pos += size;
    return p;
}

void *malloc(size_t size)
{
    if (size == 0) return NULL;
    size = (size + 15) & ~(size_t)15;

    struct malloc_block **pp = &free_list;
    while (*pp) {
        if ((*pp)->size >= size) {
            struct malloc_block *b = *pp;
            *pp = b->next;
            b->used = 1;
            return (void *)(b + 1);
        }
        pp = &(*pp)->next;
    }

    struct malloc_block *b = (struct malloc_block *)arena_alloc(sizeof(*b) + size);
    if (!b) return NULL;
    b->size = size;
    b->used = 1;
    b->next = NULL;
    return (void *)(b + 1);
}

void free(void *ptr)
{
    if (!ptr) return;
    struct malloc_block *b = (struct malloc_block *)ptr - 1;
    b->used = 0;
    b->next = free_list;
    free_list = b;
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;
    if (total / nmemb != size) return NULL;
    void *p = malloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size)
{
    if (!ptr) return malloc(size);
    if (size == 0) { free(ptr); return NULL; }
    struct malloc_block *b = (struct malloc_block *)ptr - 1;
    size_t old = b->size;
    if (size <= old) return ptr;
    void *p = malloc(size);
    if (!p) return NULL;
    memcpy(p, ptr, old < size ? old : size);
    free(ptr);
    return p;
}
