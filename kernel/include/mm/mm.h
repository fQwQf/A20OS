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
void *frame_alloc_nz(void);
void frame_free(void *addr);
size_t frame_free_count(void);

/* Page table helpers and boot_pgdir are in arch/mm.h and arch/platform.h */

#define PA2PFN(pa) ((paddr_t)(pa) >> PAGE_SIZE_BITS)
#define PFN2PA(pfn) ((paddr_t)(pfn) << PAGE_SIZE_BITS)

static inline paddr_t va_to_pa(const void *va) {
    return (paddr_t)((uint64_t)(uintptr_t)va - PAGE_OFFSET);
}

/* Page table operations */
pte_t *pt_create(void);
void pt_destroy(pt_root_t *pgdir);
int pt_map(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags);
int pt_map_huge(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags);
int pt_unmap(pt_root_t *pgdir, vaddr_t va);
int pt_unmap_leaf(pt_root_t *pgdir, vaddr_t va, paddr_t *pa_out,
                  vaddr_t *base_out, size_t *size_out, int *level_out);
paddr_t pt_translate(pt_root_t *pgdir, vaddr_t va);
pte_t *pt_walk(pt_root_t *pgdir, vaddr_t va, int alloc);
pte_t *pt_lookup_leaf(pt_root_t *pgdir, vaddr_t va, int *level_out,
                      vaddr_t *base_out, size_t *size_out);

typedef struct mm_leaf_info {
    int level;
    vaddr_t base;
    size_t size;
    paddr_t pa;
    pte_t flags;
    int dirty;
} mm_leaf_info_t;

int mm_query_leaf(pt_root_t *pgdir, vaddr_t va, mm_leaf_info_t *out);
uint64_t mm_pagemap_entry(pt_root_t *pgdir, vaddr_t va);
int mm_query_leaf_kaddr(pt_root_t *pgdir, vaddr_t va, void **kaddr_out,
                        size_t *avail_out);
int mm_fetch_user_insn32(pt_root_t *pgdir, vaddr_t va, uint32_t *out);
int mm_mark_leaf_dirty_if_writable(pt_root_t *pgdir, vaddr_t va);
int mm_debug_pte_value(pt_root_t *pgdir, vaddr_t va, uintptr_t *slot_out,
                       pte_t *value_out);

/* Per-process page table helpers */
void pt_map_kernel(pt_root_t *pgdir);
int  pt_map_range(pt_root_t *pgdir, vaddr_t va, paddr_t pa, size_t size, pte_t flags);
pte_t *pt_clone(pt_root_t *src_pgdir);
void pt_destroy_user(pt_root_t *pgdir);

#endif
