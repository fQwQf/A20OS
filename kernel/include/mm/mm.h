#ifndef _MM_H
#define _MM_H

#include "core/types.h"
#include "core/consts.h"
#include "mm/slab.h"

/* Physical frame allocator */
void mm_init(void);
void *frame_alloc(void);
void frame_free(void *addr);
size_t frame_free_count(void);

/* Page table helpers and boot_pgdir are in arch/mm.h and arch/platform.h */

#define PA2PFN(pa) ((paddr_t)(pa) >> PAGE_SIZE_BITS)
#define PFN2PA(pfn) ((paddr_t)(pfn) << PAGE_SIZE_BITS)

/* Page table operations */
uint64_t *pt_create(void);
void pt_destroy(uint64_t *pgdir);
int pt_map(uint64_t *pgdir, vaddr_t va, paddr_t pa, uint64_t flags);
int pt_unmap(uint64_t *pgdir, vaddr_t va);
paddr_t pt_translate(uint64_t *pgdir, vaddr_t va);
uint64_t *pt_walk(uint64_t *pgdir, vaddr_t va, int alloc);

/* Per-process page table helpers */
void pt_map_kernel(uint64_t *pgdir);
int  pt_map_range(uint64_t *pgdir, vaddr_t va, paddr_t pa, size_t size, uint64_t flags);
uint64_t *pt_clone(uint64_t *src_pgdir);
void pt_destroy_user(uint64_t *pgdir);

long copy_from_user(void *dst, const void *src, size_t n);
long copy_to_user(void *dst, const void *src, size_t n);
long user_strncpy(char *dst, const char *src, size_t max);

#endif
