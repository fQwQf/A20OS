#ifndef _MM_H
#define _MM_H

#include "core/types.h"
#include "core/consts.h"
#include "core/arch.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

/* Defined in mm/vm.h; only ever handled as a pointer here. */
struct mm_struct;

/* Physical frame allocator */
void mm_init(void);
void *frame_alloc(void);
void *frame_alloc_nz(void);
void *frame_alloc_nr(void);
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
/* pt_map with an explicit per-page status class (see mm/pt.h).  Callers that
 * know the page is file-backed, fork-shared or VMO-backed pass the real class
 * so a later fault resolves it from the metadata instead of a VMA. */
int pt_map_cls(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags,
               uint8_t cls);
/* cls is the status class the huge leaf carries (MM_ST_ANON_MAPPED for a
 * fresh THP fault, the parent's class across fork, the source's class across
 * mremap).  A huge leaf is ONE entry of its table, so it gets ONE status
 * slot -- at its own level, not level 0 -- and the auditor checks the pair.
 * An empty level-0 table left by earlier 4K faulting is retired (mm owns
 * the retire queue), not treated as a conflict. */
int pt_map_huge(struct mm_struct *mm, vaddr_t va, paddr_t pa, pte_t flags,
                uint8_t cls);
/* Huge-leaf installs (see mm/mm.c).  Plain global, printed in [MM-ASM]. */
extern uint64_t mm_huge_install_count;
int pt_unmap(struct mm_struct *mm, vaddr_t va);
int pt_unmap_leaf(struct mm_struct *mm, vaddr_t va, paddr_t *pa_out,
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

/*
 * Kernel-image W^X (KXAN).  arch_kernel_wx_finalize() splits the boot-time
 * megapage map into per-section mappings (text ROX / rodata RO / data RW+NX)
 * and makes the direct map non-executable; call once after mm_init() while
 * the boot page table is still the only kernel address space.  Architectures
 * that cannot split their boot map keep the weak no-op default.
 *
 * arch_kwx_module_protect()/unprotect() flip direct-map pages backing a
 * drvmod module between RX (text region) and RW+NX; the default no-ops match
 * architectures whose direct map remains executable.
 */
void arch_kernel_wx_finalize(void);
int  arch_kwx_module_protect(paddr_t base_pa, size_t exec_bytes,
                             size_t total_bytes);
void arch_kwx_module_unprotect(paddr_t base_pa, size_t total_bytes);

#endif
