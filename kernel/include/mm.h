#ifndef _MM_H
#define _MM_H

#include "types.h"
#include "consts.h"
#include "slab.h"

/* Physical frame allocator */
void mm_init(void);
void *frame_alloc(void);
void frame_free(void *addr);
size_t frame_free_count(void);

/* SV39 page table helpers */
#define SV39_LEVELS     3
#define SV39_VPN_BITS   9
#define SV39_VPN_MASK   0x1FF
#define SV39_VPN(va, lvl) (((va) >> (12 + 9 * (lvl))) & SV39_VPN_MASK)
#define SV39_PTE_PPN(pte) (((pte) >> 10) & 0x3FFFFFFFFFFFFFUL)
#define SV39_PTE_ADDR(pte) (SV39_PTE_PPN(pte) << 12)
#define PTE_FROM_PA(pa)  (((uint64_t)(pa) >> 12) << 10)

/* PTE entries store physical addresses. Use this to get a kernel VA pointer. */
#define PTE_TO_PTR(pte)  ((uint64_t *)((uint64_t)SV39_PTE_ADDR(pte) + PAGE_OFFSET))

extern uint64_t boot_pgdir[512];

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
