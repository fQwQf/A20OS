#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/vmar.h"
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

/* Mapping creation: mmap/mmap_file/mmap_vmo plus the shared file-mapping
 * dirty sync used by writeback paths.
 *
 * The creation transaction itself -- validation, W^X, the Linux fixed/hint
 * address contracts, address selection, the vm_flags/pte_flags/capability
 * policy, mlock admission, and the ordering and rollback of every fallible
 * step -- lives in kernel/mm/mmap_policy.rs.  What remains here is the
 * mm_seg_t layout (one repr(C) spec), the flat NOMMU allocation, the
 * mm/task/vfile/vmo field access the Rust module cannot name, the arch
 * constants that live in headers it cannot include, the page-table segment
 * annotation helpers, and the file-mapping dirty sync. */

extern vaddr_t a20_mmap_anon_rust(mm_struct_t *mm, vaddr_t addr, size_t len,
                                  int prot, int flags, int native_cap);
extern vaddr_t a20_mmap_file_rust(mm_struct_t *mm, vaddr_t addr, size_t len,
                                  int prot, int flags, struct vfile *file,
                                  uint64_t file_offset, int native_cap,
                                  struct vmar *owner);
extern vaddr_t a20_mmap_vmo_rust(mm_struct_t *mm, vaddr_t addr, size_t len,
                                 int prot, int flags, struct vmo *vmo,
                                 uint64_t vmo_offset, int native_cap,
                                 struct vmar *owner);

struct a20_mmap_seg_spec {
    uintptr_t start;
    uintptr_t end;
    uint64_t  vm_flags;
    uintptr_t pte_flags;
    uint32_t  vmar_cap;
    uint32_t  reserved;
    void     *file;
    uint64_t  backing_offset;
    void     *file_vnode;
    void     *vmo;
    void     *vmar_owner;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_mmap_seg_spec) == 80,
               "Rust/C mmap seg spec ABI");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct a20_mmap_seg_spec) == 48,
               "Rust/C mmap seg spec ABI");
#endif

/* Copy the transaction's decisions into the record.  `nommu_raw` is the flat
 * allocation the mapping owns on NOMMU and NULL everywhere else; it is kept out
 * of the spec so the Rust/C spec ABI does not change with CONFIG_NOMMU. */
void a20_mmap_seg_fill(void *vma_ptr, const struct a20_mmap_seg_spec *spec,
                       void *nommu_raw)
{
    mm_seg_t *vma = vma_ptr;
    if (!vma || !spec)
        return;
    vma->start = spec->start;
    vma->end = spec->end;
    vma->vm_flags = spec->vm_flags;
    vma->pte_flags = spec->pte_flags;
    vma->vmar_cap = spec->vmar_cap;
    vma->file = spec->file;
    vma->backing_offset = spec->backing_offset;
    vma->file_vnode = spec->file_vnode;
    vma->vmo = spec->vmo;
    vma->vmar_owner = spec->vmar_owner;
#ifdef CONFIG_NOMMU
    vma->nommu_alloc = nommu_raw;
#else
    (void)nommu_raw;
#endif
}

uintptr_t a20_mmap_mmap_base(const void *mm_ptr)
{
    const mm_struct_t *mm = mm_ptr;
    return mm ? (uintptr_t)mm->mmap_base : 0;
}

int a20_mmap_def_locked(const void *mm_ptr)
{
    const mm_struct_t *mm = mm_ptr;
    return mm && (mm->def_flags & VM_LOCKED);
}

uintptr_t a20_mmap_locked_vm(const void *mm_ptr)
{
    const mm_struct_t *mm = mm_ptr;
    return mm ? (uintptr_t)mm->locked_vm : 0;
}

void a20_mmap_locked_vm_add(void *mm_ptr, uintptr_t len)
{
    mm_struct_t *mm = mm_ptr;
    if (mm)
        mm->locked_vm += len;
}

void a20_mmap_total_vm_add(void *mm_ptr, uintptr_t pages)
{
    mm_struct_t *mm = mm_ptr;
    if (mm)
        mm->total_vm += pages;
}

uint64_t a20_mmap_memlock_limit(void)
{
    task_t *cur = proc_current();
    return cur ? cur->limits.memlock : 0;
}

int a20_mmap_has_cap_admin(void)
{
    return proc_has_cap(proc_current(), CAP_SYS_ADMIN);
}

void *a20_mmap_file_vnode(void *file_ptr)
{
    struct vfile *file = file_ptr;
    return file ? file->vnode : NULL;
}

uint64_t a20_mmap_vmo_size(void *vmo_ptr)
{
    struct vmo *vmo = vmo_ptr;
    return vmo ? vmo->size : 0;
}

void a20_mmap_vnode_shared_map_inc(void *vn_ptr)
{
    vnode_shared_map_inc(vn_ptr);
}

void a20_mmap_provision_anon(void *mm_ptr, uintptr_t start, uintptr_t end,
                             uintptr_t ptef)
{
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    (void)mm_pt_provision_anon(mm_ptr, start, end, (pte_t)ptef);
#else
    (void)mm_ptr;
    (void)start;
    (void)end;
    (void)ptef;
#endif
}

uintptr_t a20_mmap_user_va_limit(void)
{
    return USER_VA_LIMIT;
}

uintptr_t a20_mmap_base_addr(void)
{
    return MMAP_BASE_ADDR;
}

int a20_mmap_is_nommu(void)
{
#ifdef CONFIG_NOMMU
    return 1;
#else
    return 0;
#endif
}

/*
 * Pick the memory a NOMMU mapping is made of, and where it lands.
 *
 * With page tables a caller may name the address and the kernel either honours
 * it or picks a gap.  Here there is no address space to place anything at: a
 * named address is only ever compared against *this* process's VMAs, so an
 * address that misses them is not free memory -- it is another process's
 * image, a slab, a kernel object, or this process's own already-loaded
 * executable.  Laying a mapping over any of those silently aliases it.
 * glibc's allocator reaches this on its second mmap-backed arena, which is why
 * a small workload survives and a larger one walks a corrupted heap; and an
 * exec that MAP_FIXEDs over its own image is how a process ends up jumping to
 * a freshly zeroed page and dying on SIGILL.
 *
 * The transaction therefore drops the hint unconditionally -- MAP_FIXED
 * included -- and every mapping gets freshly allocated memory instead.  The
 * returned pointer is the raw allocation the record owns; the caller frees it
 * with kfree() if the mapping is refused before publication.
 */
void *a20_mmap_nommu_alloc(uintptr_t len, uintptr_t *addr_out)
{
#ifdef CONFIG_NOMMU
    size_t alloc_len = len + PAGE_SIZE - 1;
    void *raw = kmalloc(alloc_len);
    if (!raw)
        return NULL;
    uintptr_t addr = ROUND_UP((uintptr_t)raw, PAGE_SIZE);
    memset((void *)addr, 0, len);
    if (addr_out)
        *addr_out = addr;
    return raw;
#else
    (void)len;
    (void)addr_out;
    return NULL;
#endif
}

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

static vaddr_t mm_mmap_impl_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                                   int prot, int flags, int native_cap) {
    return a20_mmap_anon_rust(mm, addr, len, prot, flags, native_cap);
}

vaddr_t mm_mmap_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                       int prot, int flags)
{
    return mm_mmap_impl_locked(mm, addr, len, prot, flags, -1);
}

vaddr_t mm_mmap_native_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                              int prot, int flags)
{
    return mm_mmap_impl_locked(mm, addr, len, prot, flags, prot);
}

static vaddr_t mm_mmap_file_impl_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                            int prot, int flags, struct vfile *file,
                            uint64_t file_offset, int native_cap, struct vmar *owner)
{
    /* @file arrives referenced. On success the record takes ownership; on
     * failure ownership stays with the caller so it can release the reference
     * after dropping mm->lock. */
    return a20_mmap_file_rust(mm, addr, len, prot, flags, file, file_offset,
                              native_cap, owner);
}

vaddr_t mm_mmap_file_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                            int prot, int flags, struct vfile *file,
                            uint64_t offset)
{
    return mm_mmap_file_impl_locked(mm, addr, len, prot, flags, file, offset, -1, NULL);
}

vaddr_t mm_mmap_file_cap_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                                int prot, int flags, struct vfile *file,
                                uint64_t offset, uint32_t cap, struct vmar *owner)
{
    return mm_mmap_file_impl_locked(mm, addr, len, prot, flags, file, offset, cap, owner);
}

static vaddr_t mm_mmap_vmo_impl_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                              int prot, int flags, struct vmo *vmo,
                              uint64_t vmo_offset, int native_cap, struct vmar *owner)
{
    return a20_mmap_vmo_rust(mm, addr, len, prot, flags, vmo, vmo_offset,
                             native_cap, owner);
}

vaddr_t mm_mmap_vmo_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                           int prot, int flags, struct vmo *vmo,
                           uint64_t offset)
{
    return mm_mmap_vmo_impl_locked(mm, addr, len, prot, flags, vmo, offset, -1, NULL);
}

vaddr_t mm_mmap_vmo_cap_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                               int prot, int flags, struct vmo *vmo,
                               uint64_t offset, uint32_t cap, struct vmar *owner)
{
    return mm_mmap_vmo_impl_locked(mm, addr, len, prot, flags, vmo, offset, cap, owner);
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
    /* Resolve the caller's fd to a referenced vfile, then transfer ownership
     * through the same wrapper used by handle-based callers. */
    vfile_t *file = fdtable_get_current_file_ref(file_fd);
    if (!file)
        return (vaddr_t)-EBADF;
    return mm_mmap_vfile(mm, addr, len, prot, flags, file, file_offset);
}

/* Consumes one referenced vfile on success or failure. */
vaddr_t mm_mmap_vfile(mm_struct_t *mm, vaddr_t addr, size_t len,
                      int prot, int flags, struct vfile *file,
                      uint64_t file_offset)
{
    /* This API consumes one referenced vfile on every path. The locked helper
     * only transfers it to the VMA on success; failure cleanup stays outside
     * mm->lock because dropping a vfile can run filesystem finalizers. */
    if (!mm) {
        vfs_put_file(file);
        return (vaddr_t)-EINVAL;
    }
    mm_tlb_invalidate_begin(mm);
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    vaddr_t r = mm_mmap_file_locked(mm, addr, len, prot, flags, file,
                                    file_offset);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_tlb_invalidate_finish(mm);
    if (mm_addr_is_error(r))
        vfs_put_file(file);
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
