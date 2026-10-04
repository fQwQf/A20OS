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

static void seg_release_file(mm_seg_t *s)
{
    if (s->vnode)
        vnode_put(s->vnode);
}

static void seg_release_vmo(mm_seg_t *s)
{
    if (s->vmo)
        vmo_release(s->vmo);
}

/* Build a segment describing [start, end) of `kind`, or NULL.  The caller owns
 * the returned reference. */
static mm_seg_t *mm_seg_build(int kind, vaddr_t start, vaddr_t end, int fd,
                              uint64_t offset, uint64_t flags,
                              vnode_t *vnode, struct vmo *vmo)
{
    if (end <= start)
        return NULL;
    mm_seg_t *s = mm_seg_alloc();
    if (!s)
        return NULL;
    s->kind    = (uint8_t)kind;
    s->shared  = (flags & VM_SHARED) ? 1 : 0;
    s->fd      = fd;
    s->base_va = start;
    s->len     = end - start;
    s->offset  = offset;
    s->flags   = flags;
    s->vnode   = vnode;
    s->vmo     = vmo;
    s->release = (kind == MM_SEG_VMO) ? seg_release_vmo : seg_release_file;
    if (vnode)
        vnode_get(vnode);            /* the segment's own reference */
    if (vmo)
        vmo_ref(vmo);
    return s;
}

/* Install (or rebuild) the VMA's segment, and label whatever page-table path
 * already exists.  Caller holds mm->lock. */
static void vma_seg_set(mm_struct_t *mm, vm_area_t *vma, vaddr_t start,
                        vaddr_t end)
{
    /* Anonymous mappings get a segment too (MM_SEG_ANON).  Until now this
     * function classified everything as VMO or FILE because it was only ever
     * called for mappings that had VM_VMO or VM_FILE, so the segment set was
     * never the full set of mappings -- heap, stack and anonymous mmap were
     * simply absent from it.  An ordered index over segments cannot answer
     * "is [start,end) covered?" while half the address space is missing, so
     * the anon case has to be representable before that index means
     * anything. */
    int is_vmo = (vma->vm_flags & VM_VMO) != 0;
    int is_file = (vma->vm_flags & VM_FILE) != 0;
    int kind = is_vmo ? MM_SEG_VMO : (is_file ? MM_SEG_FILE : MM_SEG_ANON);
    uint64_t base_off = is_vmo ? vma->vmo_offset : vma->file_offset;
    mm_seg_t *s = mm_seg_build(kind, start, end,
                               is_vmo ? -1 : vma->file_fd,
                               base_off + (start - vma->start),
                               vma->vm_flags,
                               is_vmo ? NULL : vma->file_vnode,
                               is_vmo ? vma->vmo : NULL);
    if (!s)
        return;
    /* The policy half of the mapping.  mm_seg_build() cannot see these --
     * it is handed the backing object, not the VMA -- so they are stamped on
     * the segment that actually goes into the page table.  Everything else
     * about this VMA is now reachable from the segment alone. */
    s->vmar_cap    = vma->vmar_cap;
    s->sysv_shmid  = vma->sysv_shmid;
    mm_seg_put(vma->seg);            /* the old one, if any */
    vma->seg = s;
    (void)mm_pt_annotate_seg(mm, start, end, s);
}

void mm_mmap_seg_annotate(mm_struct_t *mm, vm_area_t *vma)
{
    if (!mm || !vma || vma->end <= vma->start)
        return;
    /* Anonymous mappings do NOT get a node-entry index.  They are not
     * dispatched from -- the dispatcher claims MM_SEG_FILE only -- and a node
     * entry is 2 MiB holding at most MM_SEGTAB_NAMES mappings, so letting anon
     * into that shared budget spends names nothing reads on evictions.  With
     * anon annotated unconditionally the gate measured table_full=42384,
     * nibbles_full=47965 and seg_miss 1572 (3.6%), against 265 (0.6%) without.
     *
     * Anon's segment still has to exist for the segment set to be the full set
     * of mappings; that is the ordered index's job (roadmap 13.14), not this
     * one's.  See mm_mmap_seg_label() for why it is safe to skip here. */
    if (!(vma->vm_flags & (VM_VMO | VM_FILE)))
        return;
    vma_seg_set(mm, vma, vma->start, vma->end);
}

/* Re-apply the VMA's segment to the page-table path.
 *
 * This is the piece the first version was missing, and the shadow measurement
 * is what found it.  mmap does not create any page-table path -- the path is
 * built lazily by the first fault -- so annotating inside the mmap call
 * labelled nothing: the walk descends only into nodes that exist, and at that
 * moment none of them did.  Measured on the real-software gate: seg_ok=6532,
 * seg_miss=37626, i.e. the segment table answered 15% of file faults.
 *
 * So it is applied again after a fault has built part of the path.  Coverage
 * converges as the mapping is touched, and every call is one walk of the
 * mapping's own path, which is short.
 */
void mm_mmap_seg_label(mm_struct_t *mm, vm_area_t *vma)
{
    if (!mm || !vma || !vma->seg || vma->end <= vma->start)
        return;
    (void)mm_pt_annotate_seg(mm, vma->start, vma->end, vma->seg);
}

void mm_mmap_seg_unannotate(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                            mm_seg_t *only)
{
    if (!mm || end <= start)
        return;
    mm_pt_unannotate_seg(mm, start, end, only);
}

/* Rebuild after a split moved a boundary.  The tail has a different offset and
 * therefore needs its own segment; addresses in the node entry that straddles
 * the boundary keep whichever name covers them, and lookup tells the two apart
 * by extent -- see "Entry index packing" in mm/pt.c. */
void mm_mmap_seg_reannotate(mm_struct_t *mm, vm_area_t *vma, vaddr_t start,
                            vaddr_t end)
{
    if (!mm || !vma || end <= start)
        return;
    if (!(vma->vm_flags & (VM_VMO | VM_FILE)))  /* anon: see mm_mmap_seg_annotate */
        return;
    /* No VM_VMO|VM_FILE guard any more: anonymous mappings carry a segment
     * too, and a split has to be able to re-annotate one.  Leaving the guard
     * in would make a split of an anonymous mapping leave its old segment's
     * name on the page table with nothing to replace it -- the range would
     * keep resolving to the pre-split extent. */
    /* Retire the old segment's name over the range IT named, not over the VMA's
     * range.  The VMA's range has already been narrowed by the caller before
     * this is reached -- a head cut has moved vma->start, a tail cut has moved
     * vma->end, a middle cut has done both -- so passing vma->start/end here
     * left the CUT-AWAY part still named by the old segment.  That part is
     * exactly the range the unmap just freed, and the old segment's recorded
     * extent still covers it, which is the one thing mm_pt_lookup_seg() trusts.
     *
     * So a later mapping landing there resolved to the segment of the mapping
     * that used to be there.  Measured: the gate's shadow check reported
     * anonymous segments shadowing file mappings of the identical one-page
     * extent, and git died with SIGSEGV.  Anonymous mappings made it common
     * because heap and brk are split constantly, but the leak was in all
     * three split branches and predates them.
     *
     * The segment's own base_va/len is the authority for what it named, and it
     * does not change when the VMA's bounds do. */
    mm_seg_t *old = vma->seg;
    if (old)
        mm_mmap_seg_unannotate(mm, old->base_va, old->base_va + old->len, old);
    vma_seg_set(mm, vma, start, end);
}

#else /* !ARCH_HAS_PGTABLE_OPS || CONFIG_NOMMU */

void mm_mmap_seg_annotate(mm_struct_t *mm, vm_area_t *vma) { (void)mm; (void)vma; }
void mm_mmap_seg_label(mm_struct_t *mm, vm_area_t *vma) { (void)mm; (void)vma; }
void mm_mmap_seg_unannotate(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                            mm_seg_t *only)
{ (void)mm; (void)start; (void)end; (void)only; }
void mm_mmap_seg_reannotate(mm_struct_t *mm, vm_area_t *vma, vaddr_t start,
                            vaddr_t end)
{ (void)mm; (void)vma; (void)start; (void)end; }

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

    uint64_t proc_flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED || !t->mm)
            continue;
        mm_struct_t *mm = t->mm;
        spin_lock(&mm->lock);
        for (vm_area_t *vma = mm->mmap; vma; vma = vma->next) {
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
                uint64_t idx = vma->file_offset + (va - vma->start);
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
    }
    spin_unlock_irqrestore(&proc_lock, proc_flags);
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

    vm_area_t *vma = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!vma) {
#ifdef CONFIG_NOMMU
        kfree(nommu_raw);
#endif
        return (vaddr_t)-ENOMEM;
    }
    refcount_set(&vma->refcount, 1);
    vma->start     = addr;
    vma->end       = addr + len;
    vma->vm_flags  = vmf;
    vma->pte_flags = ptef;
    vma->vmar_cap  = (uint32_t)mm_pte_flags_to_prot(ptef);
    vma->file_fd   = -1;
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
    /* Annotate here, where the mapping is published.  The shm and vmo helpers
     * below both do this and the main mmap path did not -- so every mapping
     * made by an ordinary mmap() was named only by whichever fault happened
     * to touch it first, and one that was never faulted stayed unnamed for
     * good.  With provisioning in mm_pt_annotate_seg() this labels the whole
     * extent at once. */
    mm_mmap_seg_annotate(mm, vma);

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
                              int prot, int flags, int file_fd,
                              uint64_t file_offset)
{
    if (file_fd < 0 || (file_offset & (PAGE_SIZE - 1)))
        return (vaddr_t)-EINVAL;
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

    int rr = vfs_ref_fd(file_fd);
    if (rr < 0)
        return (vaddr_t)rr;

    if ((flags & MAP_FIXED_NOREPLACE) && addr != 0) {
        if (mm_range_overlaps(mm, addr, len, NULL)) {
            vfs_close(file_fd);
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
            vfs_close(file_fd);
            return (vaddr_t)-ENOMEM;
        }
    }
#else
    if (addr == 0)
        addr = mm_find_gap(mm, mm->mmap_base ? mm->mmap_base : MMAP_BASE_ADDR, len);

    if (addr == 0 || addr + len < addr || addr + len > USER_VA_LIMIT) {
        vfs_close(file_fd);
        return (vaddr_t)-ENOMEM;
    }
#endif

    uint64_t vmf = VM_FILE;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    if (flags & MAP_SHARED) vmf |= VM_SHARED;
    if (flags & MAP_HUGETLB) vmf |= VM_HUGEPAGE;

    vm_area_t *vma = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!vma) {
#ifdef CONFIG_NOMMU
        kfree(nommu_raw);
#endif
        vfs_close(file_fd);
        return (vaddr_t)-ENOMEM;
    }
    refcount_set(&vma->refcount, 1);
    vma->start       = addr;
    vma->end         = addr + len;
    vma->vm_flags    = vmf;
    vma->pte_flags   = mm_prot_to_pte_flags(prot);
    vma->vmar_cap    = (uint32_t)prot;
    vma->file_fd     = file_fd;
    vma->file_offset = file_offset;
#ifdef CONFIG_NOMMU
    vma->nommu_alloc = nommu_raw;
#endif

    /* Every file VMA retains its vnode.  MAP_SHARED needs it for dirty-page
     * writeback; read-only MAP_PRIVATE additionally uses it to distinguish a
     * direct page-cache leaf from an anonymous COW leaf during teardown. */
    vfile_t *vf = vfs_get_file_ref(file_fd);
    if (vf && vf->vnode) {
        vnode_get(vf->vnode);
        vma->file_vnode = vf->vnode;
    }
    if (vf)
        vfs_put_file_ref(file_fd, vf);

    if (mm->def_flags & VM_LOCKED) {
        task_t *cur = proc_current();
        if (mm->locked_vm + len > cur->limits.memlock && !proc_has_cap(cur, CAP_SYS_ADMIN)) {
            if (vma->file_vnode) {
                vnode_put(vma->file_vnode);
                vma->file_vnode = NULL;
            }
            kfree(vma);
            vfs_close(file_fd);
            return (vaddr_t)-ENOMEM;
        }
        vma->vm_flags |= VM_LOCKED;
        mm->locked_vm += len;
    }

    if ((vma->vm_flags & VM_SHARED) && vma->file_vnode)
        vnode_shared_map_inc(vma->file_vnode);

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;
    mm_mmap_seg_annotate(mm, vma);
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
        vm_area_t *existing = mm_find_vma(mm, addr);
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

    vm_area_t *vma = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!vma)
        return (vaddr_t)-ENOMEM;
    refcount_set(&vma->refcount, 1);
    vma->start       = addr;
    vma->end         = addr + len;
    vma->vm_flags    = vmf;
    vma->pte_flags   = mm_prot_to_pte_flags(prot);
    vma->vmar_cap    = (uint32_t)prot;   /* Native VMAR capability at creation */
    vma->file_fd     = -1;
    vma->vmo         = vmo;
    vma->vmo_offset  = vmo_offset;
    vmo_ref(vmo);

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;
    mm_mmap_seg_annotate(mm, vma);
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
    mm_tlb_invalidate_begin(mm);
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_mmap_file_locked(mm, addr, len, prot, flags, file_fd,
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
