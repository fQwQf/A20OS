#ifndef _MM_H
#define _MM_H

#include "core/types.h"
#include "core/consts.h"
#include "core/arch.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

/* Physical frame allocator */
void mm_init(void);
void *frame_alloc(void);
void frame_free(void *addr);
size_t frame_free_count(void);

/* Page table helpers and boot_pgdir are in arch/mm.h and arch/platform.h */

#define PA2PFN(pa) ((paddr_t)(pa) >> PAGE_SIZE_BITS)
#define PFN2PA(pfn) ((paddr_t)(pfn) << PAGE_SIZE_BITS)

static inline paddr_t va_to_pa(const void *va) {
    return (paddr_t)((uint64_t)(uintptr_t)va - PAGE_OFFSET);
}

/* Page table operations */
uint64_t *pt_create(void);
void pt_destroy(uint64_t *pgdir);
int pt_map(uint64_t *pgdir, vaddr_t va, paddr_t pa, uint64_t flags);
int pt_map_huge(uint64_t *pgdir, vaddr_t va, paddr_t pa, uint64_t flags);
int pt_unmap(uint64_t *pgdir, vaddr_t va);
int pt_unmap_leaf(uint64_t *pgdir, vaddr_t va, paddr_t *pa_out,
                  uint64_t *base_out, size_t *size_out, int *level_out);
paddr_t pt_translate(uint64_t *pgdir, vaddr_t va);
uint64_t *pt_walk(uint64_t *pgdir, vaddr_t va, int alloc);
uint64_t *pt_lookup_leaf(uint64_t *pgdir, vaddr_t va, int *level_out,
                         uint64_t *base_out, size_t *size_out);

/* Per-process page table helpers */
void pt_map_kernel(uint64_t *pgdir);
int  pt_map_range(uint64_t *pgdir, vaddr_t va, paddr_t pa, size_t size, uint64_t flags);
uint64_t *pt_clone(uint64_t *src_pgdir);
void pt_destroy_user(uint64_t *pgdir);

#endif
