#ifndef _VM_H
#define _VM_H

#include "core/types.h"
#include "core/consts.h"
#include "core/refcount.h"
#include "core/lock.h"

struct a20_vmo;
struct vnode;

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
#define VM_FILE      (1UL << 13)
#define VM_VMO       (1UL << 14)
#define VM_LOCKED    (1UL << 15)
#define VM_SYSV_SHM  (1UL << 16)
#define VM_PFNMAP    (1UL << 17)

#ifdef CONFIG_NOMMU
#define NOMMU_ALLOC_MAX    32
#define NOMMU_ALLOC_IMAGE  0
#define NOMMU_ALLOC_STACK  1
#define NOMMU_ALLOC_TLS    2
#endif

typedef struct vm_area {
    vaddr_t         start;
    vaddr_t         end;
    uint64_t        vm_flags;
    pte_t           pte_flags;
    int             file_fd;
    int             sysv_shmid;
    uint64_t        file_offset;
    struct a20_vmo *vmo;
    uint64_t        vmo_offset;
    struct vnode   *file_vnode;     /* referenced vnode for VM_FILE+VM_SHARED */
#ifdef CONFIG_NOMMU
    void           *nommu_alloc;     /* raw allocation backing this VMA */
#endif
    struct vm_area *prev;
    struct vm_area *next;
} vm_area_t;

/*
 * mm_struct lifetime and address-space invariants:
 * - refcount is shared by every task/thread that uses the same address space.
 *   A task_t may store mm == NULL only for kernel-only tasks or after teardown
 *   has detached it from user memory.
 * - mm_destroy() drops one reference. The final reference owns destruction of
 *   all VMAs, VMA-backed resources, user page-table leaves, and the user page
 *   table itself; callers must not keep VMA or pgdir pointers after dropping the
 *   last reference.
 * - mmap is an ordered VMA list owned by the mm. VMA insertion, removal, split,
 *   merge, and resource release must preserve non-overlap and must account
 *   total_vm/rss/locked_vm consistently with mapped pages.
 * - pgdir belongs to the mm. Page-table writers must pair permission or mapping
 *   changes with the TLB flush required for the affected address space before
 *   user mode can observe stale translations.
 * - mm->lock is the intended serialization point for VMA list and page-table
 *   mutations. Some current paths still rely on single-threaded execution or
 *   narrower local locking; threaded user address spaces and SMP require those
 *   paths to be converted before broadening use.
 *
 * MM_LOCK_MODEL:
 * - Writers must hold mm->lock for mmap list mutations and user page-table
 *   mutations: mm_mmap/mm_mmap_file, mm_munmap, mm_mprotect, mm_mremap,
 *   mm_brk shrink, mm_fork COW setup, demand fault installs, COW fault installs,
 *   huge-page demotion, exec replacement, and exit teardown.
 * - Read-only VMA walks may run without mm->lock only when the caller owns the
 *   task/mm exclusively or when the walk cannot race with mmap writers. Shared
 *   address-space readers need either mm->lock, a pinned VMA/page-cache object,
 *   or a future RCU-style VMA lifetime scheme.
 * - Page-table writers must publish the new PTE before dropping the object/page
 *   reference it replaces and must flush the affected TLB range before returning
 *   to user mode. Permission relax/tighten, unmap, demote, demand fault, file
 *   mmap, COW, and dirty-bit updates are all page-table writes.
 * - File VMAs own one fd reference and, for shared mappings, a referenced vnode
 *   in file_vnode. Private file faults copy from page cache; shared file faults
 *   map the canonical page-cache page directly and pin it for the lifetime of
 *   the mapping. Dirty PTEs are synced to the page cache before fsync/msync
 *   writeback so shared mmap writes are visible through read(), fsync(), and
 *   fork-shared cloning.
 * - OOM/reclaim must not free frames still reachable from task->mm, VMA lists,
 *   page tables, page cache refs, VMO refs, or Native handles. Reclaim may only
 *   choose unpinned cache/slab objects or kill a task and let normal exit/mm
 *   teardown release memory.
 */
typedef struct mm_struct {
    spinlock_t lock;
    vm_area_t *mmap;
    pt_root_t *pgdir;
    vaddr_t    brk;
    vaddr_t    start_brk;
    vaddr_t    mmap_base;
    vaddr_t    stack_top;
    vaddr_t    stack_bottom;
    size_t     total_vm;
    size_t     rss;
    size_t     locked_vm;
    uint32_t   def_flags;
    refcount_t refcount;
#ifdef CONFIG_NOMMU
    void      *nommu_allocs[NOMMU_ALLOC_MAX];
    size_t     nommu_alloc_sizes[NOMMU_ALLOC_MAX];
    uint8_t    nommu_alloc_types[NOMMU_ALLOC_MAX];
    int        num_nommu_allocs;
#endif
} mm_struct_t;

mm_struct_t *mm_create(void);
void         mm_destroy(mm_struct_t *mm);
mm_struct_t *mm_fork(mm_struct_t *parent_mm);

vm_area_t *mm_find_vma(mm_struct_t *mm, vaddr_t addr);
vaddr_t    mm_find_gap(mm_struct_t *mm, vaddr_t hint, size_t len);
void       mm_insert_vma(mm_struct_t *mm, vm_area_t *newv);
int        mm_split_vma_at(mm_struct_t *mm, vaddr_t addr);

void mm_sync_shared_dirty_for_vnode(struct vnode *vn);

#ifdef CONFIG_NOMMU
void mm_track_nommu_alloc(mm_struct_t *mm, void *ptr, size_t size, uint8_t type);
void mm_untrack_nommu_alloc(mm_struct_t *mm, void *ptr);
#endif

vaddr_t mm_mmap(mm_struct_t *mm, vaddr_t addr, size_t len,
                int prot, int flags);
vaddr_t mm_mmap_file(mm_struct_t *mm, vaddr_t addr, size_t len,
                     int prot, int flags, int file_fd, uint64_t file_offset);
int     mm_munmap(mm_struct_t *mm, vaddr_t addr, size_t len);
vaddr_t mm_brk(mm_struct_t *mm, vaddr_t newbrk);
int     mm_mprotect(mm_struct_t *mm, vaddr_t addr, size_t len, int prot);
int     mm_mremap(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
                  size_t new_size, int flags, vaddr_t new_addr,
                  vaddr_t *out_addr);
int     mm_demote_huge_page(mm_struct_t *mm, vaddr_t addr);

pte_t mm_prot_to_pte_flags(int prot);
int   mm_pte_flags_to_prot(pte_t pte_flags);
pte_t mm_vm_flags_to_pte_flags(uint64_t vm_flags);
uint64_t mm_pte_flags_to_vm_flags(pte_t pte_flags);
pte_t mm_user_stack_pte_flags(void);
pte_t mm_user_brk_pte_flags(void);
int   mm_pte_flags_allow_access(pte_t pte_flags);
pte_t mm_pte_flags_apply_prot(pte_t old_flags, pte_t prot_flags);
pte_t mm_pte_flags_make_writable_dirty(pte_t pte_flags);

#endif
