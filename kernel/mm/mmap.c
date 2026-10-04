#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/fault.h"
#include "mm/swap.h"
#include "mm/pt.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/errno.h"

/* Mapping creation: mmap/mmap_file/mmap_vmo plus the shared
 * file-mapping dirty sync used by writeback paths. */

#ifdef CONFIG_NOMMU
static void *nommu_alloc_aligned(size_t len, vaddr_t *addr_out)
{
    size_t alloc_len = len + PAGE_SIZE - 1;
    void *raw = kmalloc(alloc_len);
    if (!raw)
        return NULL;
    vaddr_t addr = ROUND_UP((vaddr_t)raw, PAGE_SIZE);
    memset((void *)addr, 0, len);
    if (addr_out)
        *addr_out = addr;
    return raw;
}
#endif /* CONFIG_NOMMU */

/* ------------------------------------------------------------------ *
 * Backing-object segments  (P6, docs/roadmap/single-level-mm-model.md 12.6)
 * ------------------------------------------------------------------ *
 * A mapping's backing object -- the vnode, the file offset, whether it is
 * MAP_SHARED -- is currently only reachable through the VMA, which is why the
 * VMA still has to exist.  Recording it on the mapping's page-table path is
 * what lets fault dispatch answer "which object backs this address?" from the
 * page tables alone.
 *
 * Only file and VMO mappings get one.  An anonymous page needs no object, and
 * giving it a segment anyway would make every process pay a page per PT node.
 *
 * Every function here is best effort.  A failure to allocate, or a page-table
 * path that needs more than MM_SEGTAB_MAX distinct segments, leaves the range
 * unannotated -- and an unannotated range behaves exactly as it did before the
 * segment table existed, because the VMA-based fault path is still in place.
 */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)

/* Whether this mapping's name belongs in the node-entry index.
 *
 * Annotating is about the 2 MiB node-entry table, which is a dispatch cache:
 * it is only read for MM_SEG_FILE, and a node entry holds at most
 * MM_SEGTAB_NAMES names, so spending those on mappings nothing dispatches
 * from evicts the ones that are used.  With anon annotated unconditionally the
 * gate measured table_full=42384, nibbles_full=47965 and seg_miss 1572 (3.6%),
 * against 265 (0.6%) without.
 *
 * An earlier version of this file guarded on VM_VMO|VM_FILE and, because the
 * segment was built behind the same guard, skipped BUILDING as well.  That
 * measured as 10 of 14 mappings in the gate's own process having no segment at
 * all -- heap, stack and brk among them -- which is exactly the hole an ordered
 * index over segments cannot tolerate.  The two decisions are now separate, and
 * building one is not a decision at all: every mapping is a segment. */
static int seg_dispatchable(const mm_seg_t *m)
{
    return (m->vm_flags & (VM_VMO | VM_FILE)) != 0;
}

/* Name [start, end) of @m in whatever page-table path already exists.
 *
 * Caller holds mm->lock.  Best effort: a range that cannot be named is left
 * unannotated, and an unannotated range behaves exactly as it did before the
 * segment table existed.
 *
 * The extent is a parameter rather than read off @m because the callers that
 * change a mapping's bounds have already changed them by the time they get
 * here, and the node entries must be told what the mapping named BEFORE as
 * well as after.  That is what mm_mmap_seg_retire() is for. */
void mm_mmap_seg_annotate(mm_struct_t *mm, mm_seg_t *m, vaddr_t start,
                          vaddr_t end)
{
    if (!mm || !m || end <= start || !seg_dispatchable(m))
        return;
    (void)mm_pt_annotate_seg(mm, start, end, m);
}

/* Stop naming [start, end) as @m.  The mirror of the above, and the reason the
 * extent is passed in: a split narrows the mapping, and the half that was cut
 * away must stop being named too.
 *
 * Passing the mapping's own bounds here instead left the CUT-AWAY part still
 * named.  That part is exactly the range the unmap just freed, so a later
 * mapping landing there resolved to the mapping that used to be there.  The
 * gate's shadow check reported anonymous mappings shadowing file mappings of
 * the identical one-page extent, and git died with SIGSEGV.  The leak was in
 * all three split branches and predates them. */
void mm_mmap_seg_retire(mm_struct_t *mm, mm_seg_t *m, vaddr_t start, vaddr_t end)
{
    if (!mm || !m || end <= start)
        return;
    mm_pt_unannotate_seg(mm, start, end, m);
}

/* Re-apply after a fault has built part of the page-table path.
 *
 * mmap does not create any page-table path -- the path is built lazily by the
 * first fault -- so annotating inside the mmap call labelled nothing: the walk
 * descends only into nodes that exist, and at that moment none of them did.
 * Measured on the real-software gate: seg_ok=6532, seg_miss=37626, i.e. the
 * segment table answered 15% of file faults.
 *
 * Coverage therefore converges as the mapping is touched, and every call is one
 * walk of the mapping's own path, which is short. */
void mm_mmap_seg_label(mm_struct_t *mm, mm_seg_t *m)
{
    if (!m)
        return;
    mm_mmap_seg_annotate(mm, m, m->start, m->end);
}

#else /* !ARCH_HAS_PGTABLE_OPS || CONFIG_NOMMU */

void mm_mmap_seg_annotate(mm_struct_t *mm, mm_seg_t *m, vaddr_t start,
                          vaddr_t end)
{ (void)mm; (void)m; (void)start; (void)end; }
void mm_mmap_seg_retire(mm_struct_t *mm, mm_seg_t *m, vaddr_t start, vaddr_t end)
{ (void)mm; (void)m; (void)start; (void)end; }
void mm_mmap_seg_label(mm_struct_t *mm, mm_seg_t *m) { (void)mm; (void)m; }

#endif

void mm_sync_shared_dirty_for_vnode(vnode_t *vn)
{
    if (!vn)
        return;

    /*
     * Fast path: no MAP_SHARED file VMA references this vnode, so there is
     * nothing to harvest.  The full scan below visits every task, every VMA
     * and every page table, so the common case (build inputs read through
     * the page cache but never mmap'd shared) must not pay it per read.
     */
    if (__atomic_load_n(&vn->shared_file_maps, __ATOMIC_ACQUIRE) == 0)
        return;

    /* E2: tasklist_lock walks the global list; the address space is pinned
     * with mm_get() under the owning task's park_lock so a concurrent exit
     * cannot drop the last reference mid-scan.  Order stays
     * tasklist_lock -> park_lock -> mm->lock. */
    uint64_t proc_flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (proc_task_state_get(t) == PROC_UNUSED)
            continue;
        mm_struct_t *mm = proc_task_get_mm(t);
        if (!mm)
            continue;
        spin_lock(&mm->lock);
        for (mm_seg_t *vma = mm->mmap; vma; vma = vma->next) {
            if (!(vma->vm_flags & VM_SHARED) || !(vma->vm_flags & VM_FILE))
                continue;
            if (vma->file_vnode != vn)
                continue;
            for (uint64_t va = vma->start; va < vma->end; ) {
                mm_leaf_info_t leaf;
                if (!mm_query_leaf(mm->pgdir, va, &leaf)) {
                    va += PAGE_SIZE;
                    continue;
                }
                int dirty = leaf.dirty;
                uint64_t idx = vma->backing_offset + (va - vma->start);
                idx /= PAGE_SIZE;
                va = leaf.base + leaf.size;
                if (!dirty)
                    continue;

                page_cache_page_t *pcp = page_cache_get(vn, idx, 0);
                if (pcp) {
                    page_cache_mark_dirty(pcp);
                    page_cache_put(pcp);
                }
            }
        }
        spin_unlock(&mm->lock);
        mm_destroy(mm);   /* drop the scan's reference */
    }
    spin_unlock_irqrestore(&tasklist_lock, proc_flags);
}

vaddr_t mm_mmap_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                         int prot, int flags) {
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && (addr & (PAGE_SIZE - 1)))
        return (vaddr_t)-EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return (vaddr_t)-EINVAL;
    if (len > USER_VA_LIMIT) return (vaddr_t)-ENOMEM;

    /* W^X: a MAP_FIXED overwrite of an existing VMA also lands here and is
     * bound by the same policy */
    prot = mm_wx_filter_prot(prot, "mmap");
    if (prot < 0) return (vaddr_t)prot;

    pte_t ptef = mm_prot_to_pte_flags(prot);
    uint64_t vmf = VM_ANON;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    if (flags & MAP_SHARED) vmf |= VM_SHARED;
    if (flags & MAP_HUGETLB) vmf |= VM_HUGEPAGE;

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        if (mm_range_overlaps(mm, addr, len, NULL))
            return (vaddr_t)-EEXIST;
        flags |= MAP_FIXED;
    }

    if ((flags & MAP_FIXED) && addr != 0) {
        int mr = mm_munmap_locked(mm, addr, len);
        if (mr < 0)
            return (vaddr_t)mr;
    } else if (addr != 0) {
        /* Linux hint semantics: a hint outside the user VA range (or
         * whose whole range collides with an existing VMA) is ignored,
         * never fatal -- V8's GetRandomMmapAddr generates hints up to
         * 2^46 and relies on the kernel falling back to a legal address.
         * Only MAP_FIXED above may fail for an out-of-range address.
         * The check must cover the entire [addr, addr+len) range: looking
         * only at the VMA containing `addr` misses a hint that starts in a
         * gap but runs into the following VMA, which would otherwise insert
         * an overlapping VMA and corrupt the address space. */
        if (addr + len < addr || addr + len > USER_VA_LIMIT ||
            mm_range_overlaps(mm, addr, len, NULL)) {
            addr = 0;
        }
    }

#ifdef CONFIG_NOMMU
    void *nommu_raw = NULL;
    if (addr == 0) {
        nommu_raw = nommu_alloc_aligned(len, &addr);
        if (!nommu_raw) return (vaddr_t)-ENOMEM;
    }
#else
    if (addr == 0)
        addr = mm_find_gap(mm, mm->mmap_base ? mm->mmap_base : MMAP_BASE_ADDR, len);

    if (addr == 0) return (vaddr_t)-ENOMEM;
    if (addr + len < addr || addr + len > USER_VA_LIMIT)
        return (vaddr_t)-ENOMEM;
#endif

    mm_seg_t *vma = mm_seg_new();
    if (!vma) {
#ifdef CONFIG_NOMMU
        kfree(nommu_raw);
#endif
        return (vaddr_t)-ENOMEM;
    }
    vma->start     = addr;
    vma->end       = addr + len;
    vma->vm_flags  = vmf;
    vma->pte_flags = ptef;
    vma->vmar_cap  = (uint32_t)mm_pte_flags_to_prot(ptef);
    vma->file      = NULL;
#ifdef CONFIG_NOMMU
    vma->nommu_alloc = nommu_raw;
#endif

    if (mm->def_flags & VM_LOCKED) {
        task_t *cur = proc_current();
        if (mm->locked_vm + len > cur->limits.memlock && !proc_has_cap(cur, CAP_SYS_ADMIN)) {
            kfree(vma);
            return (vaddr_t)-ENOMEM;
        }
        vma->vm_flags |= VM_LOCKED;
        mm->locked_vm += len;
    }

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;
    /* No mm_mmap_seg_annotate() here: mm_insert_vma() builds the segment for
     * whichever mapping survives its merges, which is the only one whose
     * extent is final.  Annotating `vma` at this point could name a mapping
     * that the insert just merged away. */

    /*
     * CortenMM on-demand paging (paper SS4.3): record the reservation per PTE
     * so a later fault on this range can be served from per-PTE status alone,
     * with its permissions, instead of re-deriving them from the VMA.  The
     * paper pays page-table pages up front here, which is why its mmap is
     * slightly slower than Linux's while mmap-PF is faster (SS6.2).
     *
     * Provisioning is best effort: a range too large to provision eagerly just
     * keeps the VMA-based fault path, which remains correct.  It runs under
     * mm->lock, so the order mm->lock -> page-table lock is the same one the
     * fault path already uses.
     */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    if ((vmf & VM_ANON) && !(vmf & VM_SHARED))
        (void)mm_pt_provision_anon(mm, addr, addr + len, ptef);
#endif

    return addr;
}

vaddr_t mm_mmap_file_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                              int prot, int flags, struct vfile *file,
                              uint64_t file_offset)
{
    /* @file arrives referenced; on success the VMA owns that reference, on
     * failure this function releases it. */
    if (!file || (file_offset & (PAGE_SIZE - 1))) {
        vfs_put_file(file);
        return (vaddr_t)-EINVAL;
    }
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && (addr & (PAGE_SIZE - 1)))
        return (vaddr_t)-EINVAL;

    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0)
        return (vaddr_t)-EINVAL;
    if (len > USER_VA_LIMIT)
        return (vaddr_t)-ENOMEM;

    /* W^X: filter before taking the fd reference, to avoid pointless
     * refcount churn */
    prot = mm_wx_filter_prot(prot, "mmap_file");
    if (prot < 0)
        return (vaddr_t)prot;

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        if (mm_range_overlaps(mm, addr, len, NULL)) {
            vfs_put_file(file);
            return (vaddr_t)-EEXIST;
        }
        flags |= MAP_FIXED;
    }

    if ((flags & MAP_FIXED) && addr != 0) {
        int mr = mm_munmap_locked(mm, addr, len);
        if (mr < 0)
            return (vaddr_t)mr;
    } else if (addr != 0) {
        /* Same hint semantics as the anonymous path: out-of-range or
         * colliding hints fall back to the gap search.  Check the whole
         * [addr, addr+len) range, not just the VMA containing `addr`. */
        if (addr + len < addr || addr + len > USER_VA_LIMIT ||
            mm_range_overlaps(mm, addr, len, NULL)) {
            addr = 0;
        }
    }

#ifdef CONFIG_NOMMU
    void *nommu_raw = NULL;
    if (addr == 0) {
        nommu_raw = nommu_alloc_aligned(len, &addr);
        if (!nommu_raw) {
            vfs_put_file(file);
            return (vaddr_t)-ENOMEM;
        }
    }
#else
    if (addr == 0)
        addr = mm_find_gap(mm, mm->mmap_base ? mm->mmap_base : MMAP_BASE_ADDR, len);

    if (addr == 0 || addr + len < addr || addr + len > USER_VA_LIMIT) {
        vfs_put_file(file);
        return (vaddr_t)-ENOMEM;
    }
#endif

    uint64_t vmf = VM_FILE;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    if (flags & MAP_SHARED) vmf |= VM_SHARED;
    if (flags & MAP_HUGETLB) vmf |= VM_HUGEPAGE;

    mm_seg_t *vma = mm_seg_new();
    if (!vma) {
#ifdef CONFIG_NOMMU
        kfree(nommu_raw);
#endif
        vfs_put_file(file);
        return (vaddr_t)-ENOMEM;
    }
    vma->start       = addr;
    vma->end         = addr + len;
    vma->vm_flags    = vmf;
    vma->pte_flags   = mm_prot_to_pte_flags(prot);
    vma->vmar_cap    = (uint32_t)prot;
    vma->file        = file;   /* the record takes over the caller's reference */
    vma->backing_offset = file_offset;
#ifdef CONFIG_NOMMU
    vma->nommu_alloc = nommu_raw;
#endif

    /* Every file VMA retains its vnode.  MAP_SHARED needs it for dirty-page
     * writeback; read-only MAP_PRIVATE additionally uses it to distinguish a
     * direct page-cache leaf from an anonymous COW leaf during teardown. */
    if (file->vnode) {
        vnode_get(file->vnode);
        vma->file_vnode = file->vnode;
    }

    if (mm->def_flags & VM_LOCKED) {
        task_t *cur = proc_current();
        if (mm->locked_vm + len > cur->limits.memlock && !proc_has_cap(cur, CAP_SYS_ADMIN)) {
            if (vma->file_vnode) {
                vnode_put(vma->file_vnode);
                vma->file_vnode = NULL;
            }
            kfree(vma);
            vfs_put_file(file);
            return (vaddr_t)-ENOMEM;
        }
        vma->vm_flags |= VM_LOCKED;
        mm->locked_vm += len;
    }

    if ((vma->vm_flags & VM_SHARED) && vma->file_vnode)
        vnode_shared_map_inc(vma->file_vnode);

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;
    return addr;
}

vaddr_t mm_mmap_vmo_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                              int prot, int flags, struct vmo *vmo,
                              uint64_t vmo_offset)
{
    if (!mm || !vmo)
        return (vaddr_t)-EINVAL;
    if ((flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)) && (addr & (PAGE_SIZE - 1)))
        return (vaddr_t)-EINVAL;

    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0)
        return (vaddr_t)-EINVAL;
    if (len > USER_VA_LIMIT)
        return (vaddr_t)-ENOMEM;
    if (vmo_offset >= vmo->size || len > vmo->size - vmo_offset)
        return (vaddr_t)-EINVAL;
    if (vmo_offset & (PAGE_SIZE - 1))
        return (vaddr_t)-EINVAL;

    /* W^X: a Native VMO mapping follows the same policy as Linux mmap */
    prot = mm_wx_filter_prot(prot, "mmap_vmo");
    if (prot < 0)
        return (vaddr_t)prot;

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        if (mm_range_overlaps(mm, addr, len, NULL))
            return (vaddr_t)-EEXIST;
        flags |= MAP_FIXED;
    }

    if ((flags & MAP_FIXED) && addr != 0) {
        int mr = mm_munmap_locked(mm, addr, len);
        if (mr < 0)
            return (vaddr_t)mr;
    } else if (addr != 0) {
        mm_seg_t *existing = mm_seg_find(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }

#ifdef CONFIG_NOMMU
    (void)vmo_offset;
    return (vaddr_t)-EOPNOTSUPP;
#else
    if (addr == 0)
        addr = mm_find_gap(mm, mm->mmap_base ? mm->mmap_base : MMAP_BASE_ADDR, len);

    if (addr == 0 || addr + len < addr || addr + len > USER_VA_LIMIT)
        return (vaddr_t)-ENOMEM;
#endif

    uint64_t vmf = VM_VMO;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    if (flags & MAP_SHARED) vmf |= VM_SHARED;

    mm_seg_t *vma = mm_seg_new();
    if (!vma)
        return (vaddr_t)-ENOMEM;
    vma->start       = addr;
    vma->end         = addr + len;
    vma->vm_flags    = vmf;
    vma->pte_flags   = mm_prot_to_pte_flags(prot);
    vma->vmar_cap    = (uint32_t)prot;   /* Native VMAR capability at creation */
    vma->file        = NULL;
    vma->vmo         = vmo;
    vma->backing_offset  = vmo_offset;
    vmo_ref(vmo);

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;
    return addr;
}

vaddr_t mm_mmap(mm_struct_t *mm, vaddr_t addr, size_t len, int prot, int flags)
{
    if (!mm) return (vaddr_t)-EINVAL;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_mmap_locked(mm, addr, len, prot, flags);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_tlb_invalidate_finish(mm);
    return r;
}

vaddr_t mm_mmap_file(mm_struct_t *mm, vaddr_t addr, size_t len,
                     int prot, int flags, int file_fd, uint64_t file_offset)
{
    if (!mm) return (vaddr_t)-EINVAL;
    /* Resolve the caller's fd to a referenced vfile; mm_mmap_file_locked
     * takes over that reference (success) or drops it (failure). */
    vfile_t *file = fdtable_get_current_file_ref(file_fd);
    if (!file)
        return (vaddr_t)-EBADF;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_mmap_file_locked(mm, addr, len, prot, flags, file,
                                    file_offset);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_tlb_invalidate_finish(mm);
    return r;
}

vaddr_t mm_mmap_vmo(mm_struct_t *mm, vaddr_t addr, size_t len,
                    int prot, int flags, struct vmo *vmo, uint64_t vmo_offset)
{
    if (!mm) return (vaddr_t)-EINVAL;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_mmap_vmo_locked(mm, addr, len, prot, flags, vmo,
                                   vmo_offset);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_tlb_invalidate_finish(mm);
    return r;
}
