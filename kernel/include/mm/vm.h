#ifndef _VM_H
#define _VM_H

#include "core/types.h"
#include "core/consts.h"

#define VM_READ      (1UL << 0)
#define VM_WRITE     (1UL << 1)
#define VM_EXEC      (1UL << 2)
#define VM_SHARED    (1UL << 3)
#define VM_ANON      (1UL << 4)
#define VM_STACK     (1UL << 5)
#define VM_GUARD     (1UL << 6)
#define VM_COW       (1UL << 7)
#define VM_FIXED     (1UL << 8)
#define VM_DONTFORK  (1UL << 9)
#define VM_WIPEONFORK (1UL << 10)
#define VM_HUGEPAGE  (1UL << 11)
#define VM_NOHUGEPAGE (1UL << 12)

typedef struct vm_area {
    uint64_t        start;
    uint64_t        end;
    uint64_t        vm_flags;
    uint64_t        pte_flags;
    struct vm_area *prev;
    struct vm_area *next;
} vm_area_t;

typedef struct mm_struct {
    vm_area_t *mmap;
    uint64_t  *pgdir;
    uint64_t   brk;
    uint64_t   start_brk;
    uint64_t   mmap_base;
    uint64_t   stack_top;
    uint64_t   stack_bottom;
    size_t     total_vm;
    size_t     rss;
    int        refcount;
} mm_struct_t;

mm_struct_t *mm_create(void);
void         mm_destroy(mm_struct_t *mm);
mm_struct_t *mm_fork(mm_struct_t *parent_mm);

vm_area_t *mm_find_vma(mm_struct_t *mm, uint64_t addr);
uint64_t   mm_find_gap(mm_struct_t *mm, uint64_t hint, size_t len);
void       mm_insert_vma(mm_struct_t *mm, vm_area_t *newv);

uint64_t mm_mmap(mm_struct_t *mm, uint64_t addr, size_t len,
                 int prot, int flags);
int      mm_munmap(mm_struct_t *mm, uint64_t addr, size_t len);
uint64_t mm_brk(mm_struct_t *mm, uint64_t newbrk);
int      mm_mprotect(mm_struct_t *mm, uint64_t addr, size_t len, int prot);
int      mm_mremap(mm_struct_t *mm, uint64_t old_addr, size_t old_size,
                   size_t new_size, int flags, uint64_t new_addr,
                   uint64_t *out_addr);
int      mm_demote_huge_page(mm_struct_t *mm, uint64_t addr);

uint64_t prot_to_pte(int prot);

#endif
