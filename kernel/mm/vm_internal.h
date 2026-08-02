#ifndef _MM_VM_INTERNAL_H
#define _MM_VM_INTERNAL_H

#include "mm/vm.h"

/*
 * Private helpers shared between mm/vm.c and the split-out translation units
 * (mm/vma.c, mm/cow.c, mm/pte_flags.c).  These are not part of the public mm
 * API in mm/vm.h and must not be referenced outside kernel/mm/.
 */

int mm_range_overlaps(mm_struct_t *mm, vaddr_t start, vaddr_t len,
                      vm_area_t *ignore);

void vma_release_file(vm_area_t *vma);
void vma_release_ipc(vm_area_t *vma);
void vma_release(vm_area_t *vma);
int  vma_ref_file(vm_area_t *vma);
int  vma_ref_fork(vm_area_t *vma);
int  vma_ref_aux(vm_area_t *vma);
vm_area_t *vma_split(vm_area_t *vma, vaddr_t split);
vm_area_t *vma_try_merge(vm_area_t *vma);

void free_vma_pages(mm_struct_t *mm, vm_area_t *vma);

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
