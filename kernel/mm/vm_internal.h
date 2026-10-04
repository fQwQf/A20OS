#ifndef _MM_VM_INTERNAL_H
#define _MM_VM_INTERNAL_H

#include "mm/vm.h"

/*
 * Private helpers shared between mm/vm.c and the split-out translation units
 * (mm/vma.c, mm/cow.c, mm/pte_flags.c).  These are not part of the public mm
 * API in mm/vm.h and must not be referenced outside kernel/mm/.
 */

int mm_range_overlaps(mm_struct_t *mm, vaddr_t start, vaddr_t len,
                      mm_seg_t *ignore);

void vma_release_file(mm_seg_t *vma);
void vma_release_ipc(mm_seg_t *vma);
void vma_release(mm_seg_t *vma);
int  vma_ref_file(mm_seg_t *vma);
int  vma_ref_fork(mm_seg_t *vma);
int  vma_ref_aux(mm_seg_t *vma);
void mm_seg_index_invalidate(mm_struct_t *mm);
mm_seg_t *vma_split(mm_seg_t *vma, vaddr_t split);
mm_seg_t *vma_try_merge(mm_struct_t *mm, mm_seg_t *vma);
struct page_cache_page *mm_file_cache_mapping_get(mm_seg_t *vma,
                                                   vaddr_t va, pfn_t pfn);

void free_vma_pages(mm_struct_t *mm, mm_seg_t *vma);

int mm_fork_clone_page(mm_struct_t *child, mm_struct_t *parent, vaddr_t va,
                       int shared);
int mm_fork_clone_range(mm_struct_t *child, mm_struct_t *parent,
                        vaddr_t start, vaddr_t end, int shared);
int mm_fork_clone_leaf(mm_struct_t *child, mm_struct_t *parent,
                       pte_t *src_pte, vaddr_t va, int level, int shared);
int mm_fork_clone_present_level(mm_struct_t *child, mm_struct_t *parent,
                                pte_t *table, int level, vaddr_t base,
                                vaddr_t start, vaddr_t end, int shared);
int mm_fork_clone_present_range(mm_struct_t *child, mm_struct_t *parent,
                                vaddr_t start, vaddr_t end, int shared);

#endif /* _MM_VM_INTERNAL_H */
