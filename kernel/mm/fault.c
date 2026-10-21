#include "mm/fault.h"
#include "mm/vm_internal.h"
#include "mm/shutdown_write.h"

#include "proc/proc.h"
#include "proc/signal.h"
#include "core/signal_defs.h"
#include "fs/page_cache.h"
#include "fs/vfs.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "mm/pt.h"
#include "mm/vmo.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/lock.h"
#include "core/cpu.h"
#include "core/perf.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/string.h"
#include "core/errno.h"
#include "cg/cgroup.h"
#include "mm/swap.h"
#include "ipc/userfaultfd.h"

/*
 * COW_FAULT_TLB_CONTRACT:
 * - handle_cow_fault() updates the PTE before dropping the old frame reference.
 * - A page TLB flush follows every PTE replacement before returning to user.
 * - The mm_fork() regression guard depends on this ordering when a parent thread
 *   faults while another thread forks the same mm.
 */
/*
 * MAP_SHARED_FILE_CACHE_CONTRACT:
 * - Shared file mappings use the canonical page-cache page as the backing frame.
 *   Both shared file-fault entry points do this: handle_file_fault() installs
 *   page_cache_pfn() directly (the `shared` candidate branch), and
 *   mm_shared_file_fault() below maps cache_pfn from page_cache_get().  The
 *   dispatch that selects between the private and shared file paths is the
 *   VM_FILE/VM_SHARED test in handle_demand_fault_access() plus the mirror in
 *   handle_demand_fault_locked().
 * - page_cache_get() pins the page; the pin is released by page_cache_put() when
 *   the mapping is unmapped, moved, or torn down.  handle_file_fault() transfers
 *   the pin to the mapping by clearing its window[] slot.
 * - The page-cache page therefore owns writeback: user writes through the mapped
 *   PTE update page-cache data directly; fsync/msync mark the page dirty via
 *   PTE_D scanning and then write it back through vnode->ops->writepage.
 *   The scan is mm_sync_shared_dirty_for_vnode() in mm/mmap.c, reached from
 *   vfs_fsync_vfile() and sys_msync() (and from the read path, so a stale
 *   !uptodate refill cannot overwrite newer mmap data); the writeback is
 *   page_cache_writeback_vnode() falling through to vnode->ops->writepage.
 * - Read() on the same file uses the same page cache, so shared mmap writes are
 *   visible to read() without an explicit sync.
 * - Boundary: this contract covers MAP_SHARED *file* mappings only.  It says
 *   nothing about anonymous MAP_SHARED (no VM_FILE, no page-cache identity) or
 *   about MAP_PRIVATE, whose COW conversion is specified at
 *   handle_cow_fault_locked() below.
 */

/* COW breaks served by the status-led path (mm_cow_from_status).  See
 * fault.h -- plain global so the shutdown audit can assert non-vacuity. */
uint64_t mm_cow_from_status_count;

#ifndef CONFIG_NOMMU
#if ARCH_COW_TLBI_DIAG
static volatile uint32_t aarch64_cow_tlbi_diag_seen;

static int aarch64_cow_tlbi_diag_begin(task_t *t, mm_struct_t *mm, vaddr_t va,
                                       vaddr_t leaf_base, size_t leaf_size,
                                       pte_t *pte)
{
    if (leaf_size < PMD_SIZE || !(*pte & PTE_COW) ||
        __atomic_exchange_n(&aarch64_cow_tlbi_diag_seen, 1,
                            __ATOMIC_RELAXED))
        return 0;

    uint64_t token = arch_read_addr_space_token();
    printf("[A64-COW-TLBI] before pid=%d mm=%p pgdir=%p pteslot=%p cpu=%u ttbr0=0x%lx asid=0x%lx active=0x%x va=0x%lx leaf=0x%lx size=%lu pte=0x%lx pa=0x%lx flags=0x%lx\n",
           t ? t->pid : -1, (void *)mm, (void *)mm->pgdir, (void *)pte,
           cpu_current_id(), (unsigned long)token,
           (unsigned long)((token >> 48) & 0xffffUL),
           __atomic_load_n(&mm->active_cpus, __ATOMIC_ACQUIRE),
           (unsigned long)va, (unsigned long)leaf_base,
           (unsigned long)leaf_size, (unsigned long)*pte,
           (unsigned long)arch_pte_addr(*pte),
           (unsigned long)(*pte & ~ARCH_PTE_PPN_MASK));
    return 1;
}

static void aarch64_cow_tlbi_diag_after(int enabled, task_t *t, mm_struct_t *mm,
                                        vaddr_t va,
                                        pte_t *pte, const char *branch)
{
    if (!enabled)
        return;
    uint64_t tlbi_xt;
#ifdef ARCH_COW_TLBI_DIAG_RAW_OPERAND
    tlbi_xt = va;
#else
    tlbi_xt = (va >> 12) & 0x00000FFFFFFFFFFFULL;
#endif
    printf("[A64-COW-TLBI] after branch=%s pid=%d mm=%p pgdir=%p pteslot=%p cpu=%u ttbr0=0x%lx va=0x%lx pte=0x%lx pa=0x%lx flags=0x%lx tlbi_xt=0x%lx spec_va_field=0x%lx\n",
           branch, t ? t->pid : -1, (void *)mm, (void *)mm->pgdir, (void *)pte,
           cpu_current_id(),
           (unsigned long)arch_read_addr_space_token(), (unsigned long)va,
           (unsigned long)*pte, (unsigned long)arch_pte_addr(*pte),
           (unsigned long)(*pte & ~ARCH_PTE_PPN_MASK),
           (unsigned long)tlbi_xt,
           (unsigned long)(((va >> 12) & 0x00000FFFFFFFFFFFULL)));
}
#else
static int aarch64_cow_tlbi_diag_begin(task_t *t, mm_struct_t *mm, vaddr_t va,
                                       vaddr_t leaf_base, size_t leaf_size,
                                       pte_t *pte)
{
    (void)t;
    (void)mm;
    (void)va;
    (void)leaf_base;
    (void)leaf_size;
    (void)pte;
    return 0;
}

static void aarch64_cow_tlbi_diag_after(int enabled, task_t *t, mm_struct_t *mm,
                                        vaddr_t va,
                                        pte_t *pte, const char *branch)
{
    (void)enabled;
    (void)t;
    (void)mm;
    (void)va;
    (void)pte;
    (void)branch;
}
#endif
#endif /* !CONFIG_NOMMU */

/*
 * Install one mapping through the transactional cursor.  Every fault-path PTE
 * write goes through here, so the per-PTE status and the hardware entry are
 * always updated in the same step and no path can update one without the
 * other.  A return of 1 from mm_addrspace_lock means a larger-than-needed
 * leaf already covers the address, which a leaf map must not silently ignore.
 */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
static int fault_install(mm_struct_t *mm, vaddr_t page_va, pfn_t pfn, pte_t flags,
                         uint8_t cls, int replace)
{
    mm_cursor_t cur;
    int r = mm_addrspace_lock(mm, page_va, page_va + PAGE_SIZE, &cur);
    if (r != 0)
        return r < 0 ? r : -EFAULT;
    r = replace ? mm_cursor_map(&cur, page_va, pfn_to_phys(pfn), flags, cls)
                : mm_cursor_map_if_absent(&cur, page_va, pfn_to_phys(pfn), flags, cls);
    mm_cursor_unlock(&cur);
    return r;
}
#else
static int fault_install(mm_struct_t *mm, vaddr_t page_va, pfn_t pfn, pte_t flags,
                         uint8_t cls, int replace)
{
    (void)cls;
    (void)replace;
    return pt_map(mm->pgdir, page_va, pfn_to_phys(pfn), flags);
}
#endif

static int fault_map(mm_struct_t *mm, vaddr_t page_va, pfn_t pfn, pte_t flags,
                     uint8_t cls)
{
    return fault_install(mm, page_va, pfn, flags, cls, 0);
}

/*
 * Install a whole fault-around window [start, start + n*PAGE_SIZE), capped at
 * `end`.  Same shape as fault_map() and the same reason for existing: the
 * cursor API is not available to every architecture, so the two variants live
 * side by side here instead of the caller reaching for mm_addrspace_lock()
 * directly.  Only the number of page-table descents differs between them -- a
 * cursor build takes one covering-node lock and one descent for the range, a
 * non-cursor build walks once per page through that architecture's own pt_map().
 * The mappings, the status byte, rss and the perf counters are the same either
 * way, so an architecture without a transactional backend loses the
 * single-descent optimisation and nothing else.
 *
 * Returns the number of pages installed, or a negative errno if the range
 * could not be locked at all.  A short count is reported rather than assumed,
 * and consumed PFN slots are cleared individually; a racing present page
 * leaves its candidate owned by the caller.
 *
 */
#if !defined(CONFIG_NOMMU)
#if defined(ARCH_HAS_PGTABLE_OPS)
static int fault_map_window(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                            pfn_t *pfns, size_t n, pte_t flags)
{
    mm_cursor_t cur;
    int r = mm_addrspace_lock(mm, start, end, &cur);
    if (r != 0)
        return r < 0 ? r : -EFAULT;
    int mapped = 0;
    for (size_t i = 0; i < n; i++) {
        vaddr_t va = start + (vaddr_t)i * PAGE_SIZE;
        if (va >= end)
            break;
        if (pfns[i] == PFN_NONE)
            continue;
        int r = mm_cursor_map_if_absent(&cur, va, pfn_to_phys(pfns[i]),
                                        flags, MM_ST_ANON_MAPPED);
        if (r == -EEXIST)
            continue;
        if (r < 0)
            break;
        pfns[i] = PFN_NONE; /* Transfer only this slot's frame. */
        mapped++;
    }
    mm_cursor_unlock(&cur);
    return mapped;
}
#else
static int fault_map_window(mm_struct_t *mm, vaddr_t start, vaddr_t end,
                            pfn_t *pfns, size_t n, pte_t flags)
{
    int mapped = 0;
    for (size_t i = 0; i < n; i++) {
        vaddr_t va = start + (vaddr_t)i * PAGE_SIZE;
        if (va >= end)
            break;
        if (pfns[i] == PFN_NONE)
            continue;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
        if (pte && *pte)
            continue;
        if (pt_map(mm->pgdir, va, pfn_to_phys(pfns[i]), flags) < 0)
            break;
        pfns[i] = PFN_NONE;
        mapped++;
    }
    return mapped;
}
#endif /* ARCH_HAS_PGTABLE_OPS */
#endif /* !CONFIG_NOMMU */

int mm_shared_file_fault(mm_struct_t *mm, mm_seg_t *vma, uint64_t page_va,
                         vfile_t *vf)
{
    if (!mm || !vma || !vf || !vf->vnode)
        return -1;

    uint64_t file_pos = vma->backing_offset + (page_va - vma->start);
    if (file_pos >= vf->vnode->size) {
        signal_send(proc_current()->pid, SIGBUS);
        return -1;
    }

    uint64_t index = file_pos / PAGE_SIZE;
    page_cache_page_t *pcp = page_cache_get(vf->vnode, index, 1);
    if (!pcp) {
        kerr("[SHFAULT] cache_get failed pid=%d va=0x%lx fd=%d idx=%lu\n",
             proc_current()->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
             (unsigned long)index);
        return -1;
    }

    if (!page_cache_is_uptodate(pcp)) {
        if (page_cache_fill_vfile_page(vf, pcp) < 0) {
            kerr("[SHFAULT] fill failed pid=%d va=0x%lx fd=%d idx=%lu\n",
                 proc_current()->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
                 (unsigned long)index);
            page_cache_put(pcp);
            return -1;
        }
    }

    pfn_t cache_pfn = page_cache_pfn(pcp);
    if (!pfn_valid(cache_pfn)) {
        kerr("[SHFAULT] bad pfn pid=%d va=0x%lx fd=%d idx=%lu pfn=%lu\n",
             proc_current()->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
             (unsigned long)index, (unsigned long)cache_pfn);
        page_cache_put(pcp);
        return -1;
    }

    if (vma->pte_flags & PTE_X)
        arch_flush_icache_range(page_cache_data(pcp), PAGE_SIZE);
    int r = fault_map(mm, page_va, cache_pfn, vma->pte_flags,
                      MM_ST_FILE_SHARED);
    if (r < 0) {
        kerr("[SHFAULT] map failed pid=%d va=0x%lx fd=%d idx=%lu r=%d\n",
             proc_current()->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
             (unsigned long)index, r);
        page_cache_put(pcp);
        return -1;
    }

    mm_rss_add(mm, 1);
    arch_tlb_flush_page_local(page_va);
    return 0;
}

/*
 * After a COW break has rewritten the PTE, bring the per-PTE status back in
 * step.  Every exit below clears PTE_COW and (usually) adds PTE_W, and all of
 * them used to leave the status describing the pre-fault page -- which is how
 * mm_pt_audit_all() ended up with a nonzero prot/cow mismatch, and how a
 * status-driven fault would re-install read-only over a page the process had
 * just been given write access to.
 *
 * `vma` is the mapping the fault was attributed to and is already resolved by
 * the caller; the class it implies is the backing, which COW does not change.
 */
#ifndef CONFIG_NOMMU
static void cow_sync_status(struct mm_struct *mm, vaddr_t va,
                            const mm_seg_t *vma)
{
#if defined(ARCH_HAS_PGTABLE_OPS)
    /* A COW break can also run on a huge leaf (the rc>1 branch copies the
     * whole 2 MiB frame).  The leaf's status slot then lives in its own
     * table at its own level, so both the table and the index must be taken
     * level-aware; a level-0 peek would read a neighbouring entry. */
    int level = 0;
    if (!pt_lookup_leaf(mm->pgdir, va, &level, NULL, NULL))
        return;
    pte_t *tab = mm_pt_leaf_table(mm->pgdir, va);
    if (!tab)
        return;
    int idx = arch_pt_vpn(va, level);

    /* Prefer the class the status already records; fall back to the VMA only
     * when there is none (a page whose status predates this path). */
    uint8_t cls = MM_ST_GET_CLASS(mm_pt_peek(tab, level, idx));
    if (cls == MM_ST_INVALID || cls == MM_ST_PT_NODE) {
        if (!vma)
            return;
        if (vma->vm_flags & VM_VMO)
            cls = MM_ST_VMO;
        else if (vma->vm_flags & VM_FILE)
            cls = (vma->vm_flags & VM_SHARED) ? MM_ST_FILE_SHARED
                                             : MM_ST_FILE_PRIVATE;
        else
            cls = MM_ST_ANON_MAPPED;
    }
    (void)mm_pt_sync_status(tab, level, idx, cls);
#else
    /* arm32's short-descriptor backend has no per-PTE status sidecar, so
     * there is no status to bring in step: the PTE-level COW bookkeeping is
     * the whole of the break there. */
    (void)mm;
    (void)va;
    (void)vma;
#endif /* ARCH_HAS_PGTABLE_OPS */
}
#endif /* CONFIG_NOMMU */

/* COW may replace either a base-page leaf or an AArch64 PMD block leaf.
 * arch_pte_leaf() always emits descriptor type 11, which is a table/page
 * descriptor and therefore corrupts a level-2 block mapping. Preserve the
 * block descriptor type when the lookup returned a huge leaf. */
#ifndef CONFIG_NOMMU
static pte_t cow_replacement_leaf(paddr_t pa, pte_t flags, size_t leaf_size)
{
#ifdef ARCH_HAS_PTE_BLOCK
    if (leaf_size >= PMD_SIZE)
        return arch_pte_block(pa, flags);
#else
    (void)leaf_size;
#endif
    return arch_pte_leaf(pa, flags);
}
#endif

#ifndef CONFIG_NOMMU
/*
 * The COW write-isolation transaction lives in kernel/mm/cow_fault.rs.  What
 * is left here is the architecture and allocator surface: the leaf probe and
 * its predicates, the PTE encoders and flag subsets, the status sidecar, the
 * frame allocator's lock, the copy, the diagnostic hooks and the local TLB
 * flush.
 */
extern int a20_cow_fault_locked_rust(task_t *t, mm_struct_t *mm, uint64_t stval,
                                     pfn_t *old_pfn_out,
                                     page_cache_page_t **old_page_out);

struct a20_cowf_probe {
    void     *slot;
    uintptr_t pte;
    uintptr_t base;
    uintptr_t size;
    uintptr_t pa;
    uint32_t  pfn;
    int32_t   present;
    int32_t   is_leaf;
    int32_t   user;
    int32_t   cow;
    int32_t   wr;
    int32_t   pfn_valid;
    int32_t   order;
};
/* 4 pointer-width words plus 4 + 28 bytes of scalars: 72 on LP64, 52 on
 * ILP32.  (The previous 48 for ILP32 was never reachable -- it does not match
 * the field list -- so a 32-bit build could not compile at all.)  The Rust
 * mirror in kernel/mm/cow_fault.rs pins the same pair. */
_Static_assert(sizeof(struct a20_cowf_probe) ==
                   (sizeof(void *) == 8 ? 72 : 52),
               "Rust/C cowf probe ABI");

int a20_cowf_has_pgdir(const void *mm_ptr)
{
    const mm_struct_t *mm = mm_ptr;
    return mm && mm->pgdir;
}

void a20_cowf_probe(const void *mm_ptr, uintptr_t stval,
                    struct a20_cowf_probe *out)
{
    const mm_struct_t *mm = mm_ptr;
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->pfn = PFN_NONE;
    /* See a20_cow_probe() in cow.c: the struct keeps pointer-width fields to
     * match the Rust mirror, so the vaddr_t/size_t outputs go through locals. */
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, stval, NULL, &base, &size);
    if (!pte)
        return;
    out->base = (uintptr_t)base;
    out->size = (uintptr_t)size;
    out->slot = pte;
    out->pte = (uintptr_t)*pte;
    out->present = (*pte & PTE_V) != 0;
    out->is_leaf = arch_pte_is_leaf(*pte);
    out->user = (*pte & PTE_U) != 0;
    out->cow = (*pte & PTE_COW) != 0;
    out->wr = (*pte & PTE_W) != 0;
    if (out->present) {
        out->pa = arch_pte_addr(*pte);
        out->pfn = phys_to_pfn(out->pa);
        out->pfn_valid = pfn_valid(out->pfn);
    }
    out->order = (out->size >= PMD_SIZE) ? PMD_ORDER : 0;
}

void *a20_cowf_vma(void *mm_ptr, uintptr_t va, uint64_t *vm_flags,
                   int *present)
{
    mm_struct_t *mm = mm_ptr;
    mm_seg_t *v = mm_seg_find(mm, va);
    if (vm_flags)
        *vm_flags = v ? v->vm_flags : 0;
    if (present)
        *present = v ? 1 : 0;
    return v;
}

int a20_cowf_diag_begin(task_t *t, mm_struct_t *mm, uintptr_t stval,
                        uintptr_t base, uintptr_t size, uintptr_t snapshot_pte)
{
    /* The transaction already snapshotted the leaf; the hook is a diagnostic,
     * so it gets that value instead of paying another page-table walk on the
     * COW hot path. */
    pte_t snapshot = (pte_t)snapshot_pte;
    return aarch64_cow_tlbi_diag_begin(t, mm, stval, base, size, &snapshot);
}

uint32_t a20_cowf_alloc_page(void)
{
    return pfa_alloc_page();
}

uint32_t a20_cowf_alloc_order(int order)
{
    return pfa_alloc(order);
}

void a20_cowf_copy(uint32_t dst, uint32_t src, uintptr_t size)
{
    memcpy(pfn_to_virt(dst), pfn_to_virt(src), size);
}

uint64_t a20_cowf_pfa_lock(void)
{
    return spin_lock_irqsave(&pfa.lock);
}

uint32_t a20_cowf_pfa_refs(uint32_t pfn)
{
    return pfa.meta[pfn].refcount;
}

void a20_cowf_pfa_unlock(uint64_t flags)
{
    spin_unlock_irqrestore(&pfa.lock, flags);
}

void a20_cowf_zero_ref(task_t *t, uintptr_t stval, uint32_t pfn, int order)
{
    printf("[COW ZERO-REF] pid=%d va=0x%lx pfn=%lu order=%d\n",
           t ? t->pid : -1, (unsigned long)stval, (unsigned long)pfn, order);
    panic("COW leaf references a free frame");
}

/* The two flag subsets a COW break applies: a fresh copy keeps the RWX and
 * attribute bits and gains write+dirty, while the plain-write branch keeps the
 * existing bits and adds the dirty bit. */
static pte_t a20_cowf_break_flags(pte_t pte, int kind)
{
    if (kind == 1)
        return (pte & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_G | PTE_A |
                       PTE_MAT1 | PTE_LEAF | PTE_COW)) | PTE_D;
    return (pte & (PTE_R | PTE_X | PTE_U | PTE_A | PTE_G | PTE_MAT1 |
                   PTE_LEAF)) | PTE_W | PTE_D;
}

static void a20_cowf_diag_after(int diag, task_t *t, mm_struct_t *mm,
                                uintptr_t stval, pte_t *pte, int tag)
{
    static const char *const tags[] = { "cache-copy", "copy", "reuse" };
    const char *branch = (tag >= 0 && tag < 3) ? tags[tag] : "?";
    aarch64_cow_tlbi_diag_after(diag, t, mm, stval, pte, branch);
}

/* Rewrite the leaf, bring the per-PTE status in step, run the arch diagnostic
 * and drop the local translation.  `kind` selects the flag subset; the
 * snapshot PTE is the one the transaction decided on. */
void a20_cowf_install(void *mm_ptr, uintptr_t stval, uint32_t new_pfn,
                      uintptr_t snapshot_pte, uintptr_t base, uintptr_t size,
                      void *vma_ptr, void *slot, void *t_ptr, int diag, int tag,
                      int kind)
{
    mm_struct_t *mm = mm_ptr;
    pte_t *pte = slot;
    if (!pte)
        return;
    pte_t flags = a20_cowf_break_flags((pte_t)snapshot_pte, kind);
    *pte = cow_replacement_leaf(pfn_to_phys(new_pfn), flags, size);
    cow_sync_status(mm, base, vma_ptr);
    a20_cowf_diag_after(diag, t_ptr, mm, stval, pte, tag);
    arch_tlb_flush_page_local(stval);
}

/* The in-place upgrade: rewrites under the allocator lock the caller still
 * holds, then unlocks and flushes -- the order the rc==1 branch needs so a
 * timer interrupt cannot release the frame between decision and write. */
void a20_cowf_reuse_install(void *mm_ptr, uintptr_t stval, uintptr_t old_pa,
                            uintptr_t snapshot_pte, uintptr_t base,
                            uintptr_t size, void *vma_ptr, void *slot,
                            void *t_ptr, int diag, uint64_t pfa_flags)
{
    mm_struct_t *mm = mm_ptr;
    pte_t *pte = slot;
    if (pte) {
        pte_t flags = a20_cowf_break_flags((pte_t)snapshot_pte, 0);
        *pte = cow_replacement_leaf((paddr_t)old_pa, flags, size);
        cow_sync_status(mm, base, vma_ptr);
        a20_cowf_diag_after(diag, t_ptr, mm, stval, pte, 2);
    }
    spin_unlock_irqrestore(&pfa.lock, pfa_flags);
    arch_tlb_flush_page_local(stval);
}

void a20_cowf_write_d(void *mm_ptr, uintptr_t stval, uintptr_t snapshot_pte,
                      uintptr_t base, uintptr_t size, void *vma_ptr,
                      void *slot)
{
    mm_struct_t *mm = mm_ptr;
    pte_t *pte = slot;
    if (!pte)
        return;
    pte_t flags = a20_cowf_break_flags((pte_t)snapshot_pte, 1);
    *pte = cow_replacement_leaf(arch_pte_addr((pte_t)snapshot_pte), flags,
                                size);
    cow_sync_status(mm, base, vma_ptr);
    arch_tlb_flush_page_local(stval);
}
#endif /* !CONFIG_NOMMU */

static int handle_cow_fault_locked(task_t *t, mm_struct_t *mm, uint64_t stval,
                                   pfn_t *old_pfn_out,
                                   page_cache_page_t **old_page_out) {
#ifdef CONFIG_NOMMU
    (void)t;
    (void)mm;
    (void)stval;
    (void)old_pfn_out;
    (void)old_page_out;
    return -1;
#else
    return a20_cow_fault_locked_rust(t, mm, stval, old_pfn_out, old_page_out);
#endif
}

/*
 * Demand paging + stack growth.
 *
 * Called for page faults after COW has been ruled out or was not applicable.
 * Handles lazy stack growth, brk pages, and anonymous VMA pages.
 */
/*
 * DEMAND_FAULT_TLB_CONTRACT:
 * - Stack/brk/anonymous/file/VMO/huge-page demand faults install a PTE, update
 *   rss/accounting, then flush the faulting page before returning.
 * - MAP_SHARED file faults do NOT take a private copy: they install the
 *   canonical page-cache frame itself and inherit MAP_SHARED_FILE_CACHE_CONTRACT
 *   above (dirty-bit harvest in mm_sync_shared_dirty_for_vnode() plus
 *   page_cache_writeback_vnode()).  Only MAP_PRIVATE file faults copy out of the
 *   page cache, and a read-only MAP_PRIVATE fault-around leaf may additionally
 *   map the cache frame under the COW conditions described at handle_cow_fault().
 * - Anonymous MAP_SHARED is a different thing and is NOT covered by that
 *   contract: with VM_FILE clear it takes the plain MM_ST_ANON_MAPPED zero-page
 *   path below, so its pages carry no page-cache identity and no writeback
 *   route.  Inter-process shared memory is served by the VM_VMO path instead.
 */
/*
 * MM_FAULT_RETRY -- the fault-around window could not be installed because the
 * VMA that authorised it is no longer the object covering the address, and it
 * is not an error.
 *
 * The window deliberately drops mm->lock to allocate its frames, and while it
 * is unlocked a sibling thread sharing this mm can reshape the VMA list at that
 * address.  The commonest case is benign and happens constantly: mm_insert_vma()
 * coalesces two adjacent anonymous VMAs and keeps the NEW object, deferring the
 * old one, so a mapping nobody touched changes identity underneath the fault.
 * The address is still mapped, still writable, and still backed by an
 * equivalent VMA -- vma_can_merge() only merges equal vm_flags and pte_flags.
 *
 * Returning failure here turned that into SIGSEGV on a valid address, which is
 * how a four-thread process died on a store into memory it owned.  The window
 * gives its frames back and asks the caller to start over against whatever VMA
 * is current, which costs one more lookup and turns the race into a no-op.
 */
#define MM_FAULT_RETRY (-EAGAIN)

/* A window can only lose its VMA to a list mutation, and each retry resolves a
 * fresh one, so the bound is generous enough for a heavy mmap/munmap workload
 * and still finite: without it a pathological merger could spin here. */
#define MM_FAULT_RETRY_MAX 8

/* The definition sits behind #ifndef CONFIG_NOMMU, as does the only call site
 * (handle_demand_fault_access's non-NOMMU branch), so the declaration has to
 * carry the same guard -- otherwise a NOMMU build sees a static declaration
 * with no definition and -Werror takes the whole kernel down. */
#ifndef CONFIG_NOMMU
static int handle_demand_fault_attempt(task_t *t, uint64_t stval,
                                       enum mm_fault_access access);
#endif
/* ---- P6 shadow check (see docs/status/history/mm-single-level-migration-log.md 12.6) ----
 *
 * Asks the page-table segment table what it would have said about this fault,
 * and counts how often that matches what the mapping list said.  Changes
 * nothing: a non-zero disagreement count means the segment would have faulted
 * in the wrong page.
 *
 * Since the merge (roadmap 13.18) both sides resolve to the SAME mm_seg_t
 * record, so this no longer compares two representations -- it compares two
 * ways of resolving an address to one record, which can still differ when a
 * 2 MiB node entry names a neighbouring mapping.
 *
 * It lives here rather than in pt.c because only this side walks mm->mmap, and
 * a disagreement is only diagnosable when both sides are printed together.
 */
#ifndef CONFIG_NOMMU
static void shadow_seg_check(mm_struct_t *mm, mm_seg_t *vma, vaddr_t va,
                             uint8_t kind, uint64_t base_off)
{
#if !defined(ARCH_HAS_PGTABLE_OPS)
    /* arm32 keeps no page-table segment table to shadow against; the check
     * is inert rather than wrong there, and the counters it feeds stay 0. */
    (void)mm;
    (void)vma;
    (void)va;
    (void)kind;
    (void)base_off;
#else
    if (!(vma->vm_flags & (VM_VMO | VM_FILE)))
        return;
    if ((kind == MM_SEG_VMO) != ((vma->vm_flags & VM_VMO) != 0))
        return;

    int shared = (vma->vm_flags & VM_SHARED) ? 1 : 0;
    uint64_t want = base_off + (va - vma->start);
    mm_seg_t *found = NULL;
    if (mm_pt_shadow_seg(mm, va, kind, want, shared, &found) != 0 || !found)
        return;

    /* The mapping the page tables named, against the mapping that owns this
     * address right now.  They are different records when the annotation came
     * from a neighbour sharing this (much coarser) node entry, and the extents
     * are what are supposed to tell the two apart. */
    kwarn("[SEG-SHADOW] va=0x%lx: mapping [0x%lx,0x%lx) off=0x%lx shared=%d "
          "file=%lu says kind=%u, but the page tables name "
          "[0x%lx,0x%lx) off=0x%lx kind=%u shared=%d file=%lu%s\n",
          (unsigned long)va,
          (unsigned long)vma->start, (unsigned long)vma->end,
          (unsigned long)want, shared,
          (unsigned long)(vma->file ? vma->file->identity : 0), kind,
          (unsigned long)found->start, (unsigned long)found->end,
          (unsigned long)found->backing_offset, mm_seg_kind(found),
          mm_seg_shared(found),
          (unsigned long)(found->file ? found->file->identity : 0),
          vma == found ? "" : "  (a different mapping)");
    mm_seg_put(found);
#endif /* ARCH_HAS_PGTABLE_OPS */
}
#endif /* CONFIG_NOMMU */

/* Check software mapping policy before allocating or resolving a fault.
 * A PTE's read bit may be its validity bit, so it cannot express PROT_NONE. */
#ifndef CONFIG_NOMMU
static int fault_access_allowed(uint64_t flags, enum mm_fault_access access)
{
    if (access == MM_FAULT_ACCESS_WRITE)
        return (flags & VM_WRITE) != 0;
    if (access == MM_FAULT_ACCESS_EXEC)
        return (flags & VM_EXEC) != 0;
    return (flags & (VM_READ | VM_WRITE)) != 0;
}
#endif

#if defined(CONFIG_SWAP) && !defined(CONFIG_NOMMU)
/* Speculative I/O holds its own slot reference. Only a matching PTE/VMA may
 * consume the data, so simultaneous faults, munmap and remap cannot overwrite
 * a newer page or recycle the disk slot while the read is in flight. */
static int handle_swap_fault(task_t *t, uint64_t stval, enum mm_fault_access access)
{
    mm_struct_t *mm = mm_get(t->mm);
    if (!mm) return -EFAULT;
    struct cg_node *charge = cg_task_group_pin(t);
    vaddr_t va = stval & ~(vaddr_t)(PAGE_SIZE - 1);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    mm_seg_t *vma = mm_seg_find(mm, va);
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
    int r = MM_FAULT_RETRY;
    if (t->mm != mm || !vma || va < vma->start || va >= vma->end ||
        !fault_access_allowed(vma->vm_flags, access) || !pte || !pte_is_swap(*pte)) {
        spin_unlock_irqrestore(&mm->lock, flags);
        goto out_mm;
    }
    swap_entry_t entry = pte_to_swp_entry(*pte);
    r = swap_duplicate(entry);
    if (r < 0) {
        spin_unlock_irqrestore(&mm->lock, flags);
        goto out_mm;
    }
    mm_seg_get(vma);
    spin_unlock_irqrestore(&mm->lock, flags);
    pfn_t pfn = pfa_alloc_page();
    int charged = 0;
    r = -ENOMEM;
    if (pfn == PFN_NONE) goto out_slot;
    if (cg_mem_charge(charge, 1) < 0) goto out_frame;
    charged = 1;
    r = swap_read_page(entry, pfn_to_virt(pfn));
    if (r < 0) goto out_frame;

    flags = spin_lock_irqsave(&mm->lock);
    mm_seg_t *current = mm_seg_find(mm, va);
    pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
    r = MM_FAULT_RETRY;
    if (t->mm == mm && current == vma && va >= vma->start && va < vma->end &&
        fault_access_allowed(vma->vm_flags, access) && pte &&
        pte_is_swap(*pte) && pte_to_swp_entry(*pte) == entry) {
        r = fault_install(mm, va, pfn, vma->pte_flags, MM_ST_ANON_MAPPED, 1);
        if (!r) mm_rss_add(mm, 1);
    }
    spin_unlock_irqrestore(&mm->lock, flags);
    if (!r) {
        /* Drop the replaced PTE's reference, then the speculative I/O pin. */
        swap_free(entry);
        pfn = PFN_NONE;
        charged = 0;
        arch_tlb_flush_page_local(stval);
        __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&t->perf_page_faults_maj, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_perf_sw_page_faults_maj, 1, __ATOMIC_RELAXED);
    }
out_frame:
    if (charged) cg_mem_uncharge(charge, 1);
    if (pfn != PFN_NONE) frame_put(pfn);
out_slot:
    swap_free(entry);
    mm_seg_put(vma);
out_mm:
    cg_group_unpin(charge);
    mm_destroy(mm);
    return r;
}
#endif

static int handle_demand_fault_locked(task_t *t, uint64_t stval,
                                      enum mm_fault_access access,
                                      int lock_held) {
#ifdef CONFIG_NOMMU
    (void)t;
    (void)stval;
    (void)access;
    (void)lock_held;
    return -1;
#else
    if (!t->mm || !t->mm->pgdir) return -1;

    uint64_t page_va = stval & ~(PAGE_SIZE - 1);
    mm_seg_t *policy = mm_seg_find(t->mm, page_va);
    if (policy && !fault_access_allowed(policy->vm_flags, access))
        return -1;
    pte_t *pte = pt_lookup_leaf(t->mm->pgdir, page_va, NULL, NULL, NULL);


    if (t->mm->stack_top != 0) {

        uint64_t stack_size_limit = t->limits.stack ? t->limits.stack : USER_STACK_MAX_SIZE;
        if (stack_size_limit > USER_STACK_MAX_SIZE)
            stack_size_limit = USER_STACK_MAX_SIZE;
        stack_size_limit = ROUND_UP(stack_size_limit, PAGE_SIZE);
        uint64_t stack_limit = t->mm->stack_top - stack_size_limit;
        /* Once ASLR has moved the stack top down, the lower bound for stack
         * growth moves down with it.  Clamp to USER_STACK_FLOOR so it can never
         * intrude into the fixed vDSO/vvar/TLS region below (see
         * mm/vdso_layout.h). */
        if (stack_limit < USER_STACK_FLOOR)
            stack_limit = USER_STACK_FLOOR;
        if (page_va >= stack_limit && page_va < t->mm->stack_top) {
            /*
             * A stack page is mapped read/write and never executable.  Faulting
             * one in to satisfy an *instruction* access would report success,
             * leave the leaf non-executable, and hand control back to a PC the
             * hardware cannot fetch from -- so the very next instruction
             * re-faults at the same address, forever.  That is exactly what a
             * jump through a corrupted function pointer looks like: mksh on
             * aarch64 spun on a prefetch abort at a stack address until the
             * timeout killed the machine, with no fault report, because every
             * round trip returned "handled".
             *
             * Refuse instead, so the caller reports SIGSEGV and kills the task.
             */
            if (access == MM_FAULT_ACCESS_EXEC)
                return -1;

            pte_t *pte = pt_walk(t->mm->pgdir, page_va, 0);
            if (pte && (*pte & PTE_V))
                return -1;

            pfn_t pfn = pfa_alloc_page();
            if (pfn == PFN_NONE) return -1;
            if (cg_mem_charge(t->cgroup, 1) != 0) {
                frame_put(pfn);
                return -ENOMEM;
            }
            memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

            int r = fault_map(t->mm, page_va, pfn, mm_user_stack_pte_flags(),
                              MM_ST_ANON_MAPPED);
            if (r < 0) {
                cg_mem_uncharge(t->cgroup, 1);
                frame_put(pfn);
                return r == -EEXIST ? MM_FAULT_RETRY : r;
            }

            if (page_va < t->mm->stack_bottom)
                t->mm->stack_bottom = page_va;
            mm_rss_add(t->mm, 1);
            arch_tlb_flush_page_local(stval);
            return 0;
        }
    }

    if (page_va >= t->mm->start_brk &&
        page_va < ROUND_UP(t->mm->brk, PAGE_SIZE) &&
        !mm_seg_find(t->mm, page_va)) {
        /* Same reasoning as the stack branch: a heap page is never
         * executable, so an instruction fault here must not be satisfied with
         * a fresh read/write leaf or the fault repeats indefinitely. */
        if (access == MM_FAULT_ACCESS_EXEC)
            return -1;

        if (cg_mem_charge(t->cgroup, 1) != 0) {
            return -ENOMEM;
        }
        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) { cg_mem_uncharge(t->cgroup, 1); return -1; }
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        int r = fault_map(t->mm, page_va, pfn, mm_user_brk_pte_flags(),
                          MM_ST_ANON_MAPPED);
        if (r < 0) {
            cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            return r == -EEXIST ? MM_FAULT_RETRY : r;
        }

        mm_rss_add(t->mm, 1);
        a20_perf_count(A20_PERF_MM_ANON_FAULTS);
        arch_tlb_flush_page_local(stval);
        return 0;
    }

    mm_seg_t *vma = mm_seg_find(t->mm, page_va);
    if (vma) {

        if (pte && (*pte & PTE_V)) return MM_FAULT_RETRY;
        if (!mm_pte_flags_allow_access(vma->pte_flags)) return -1;

        /* Shadow check: would the segment table have answered this fault the
         * same way?  Nothing below changes behaviour -- it is the
         * measurement that says whether the page-table name and the mapping
         * list name the same record. */
        shadow_seg_check(t->mm, vma, page_va,
                         (vma->vm_flags & VM_VMO) ? MM_SEG_VMO : MM_SEG_FILE,
                         vma->backing_offset);

        if ((vma->vm_flags & VM_FILE) && vma->file) {
            vfile_t *vf = vma->file;
            vfile_get(vf);
            if (!vf->vnode) {
                kerr("[MFAULT] no vnode pid=%d va=0x%lx file=%p\n",
                     t->pid, (unsigned long)page_va, (void *)vf);
                vfs_put_file(vf);
                return -1;
            }

            uint64_t file_pos = vma->backing_offset + (page_va - vma->start);
            if (file_pos >= vf->vnode->size) {
                kerr("[MFAULT] oob pid=%d va=0x%lx fd=%d pos=%lu size=%llu\n",
                     t->pid, (unsigned long)page_va, (unsigned long)(vma->file ? vma->file->identity : 0),
                     (unsigned long)file_pos,
                     (unsigned long long)vf->vnode->size);
                signal_send(t->pid, SIGBUS);
                vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);
                return -1;
            }

            if (vma->vm_flags & VM_SHARED) {
                int r = mm_shared_file_fault(t->mm, vma, page_va, vf);
                vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);
                return r;
            } else {
                page_cache_page_t *pcp = page_cache_get(vf->vnode,
                                                         file_pos / PAGE_SIZE, 1);
                if (!pcp) {
                    vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);
                    return -1;
                }
                if (!page_cache_is_uptodate(pcp)) {
                    if (page_cache_fill_vfile_page(vf, pcp) < 0) {
                        page_cache_put(pcp);
                        vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);
                        return -1;
                    }
                }
                vfs_put_file_ref((unsigned long)(vma->file ? vma->file->identity : 0), vf);

                pfn_t cache_pfn = page_cache_pfn(pcp);
                if (!pfn_valid(cache_pfn)) {
                    page_cache_put(pcp);
                    return -1;
                }

                if (cg_mem_charge(t->cgroup, 1) != 0) {
                    page_cache_put(pcp);
                    return -ENOMEM;
                }
                pfn_t copy = pfa_alloc_page();
                if (copy == PFN_NONE) {
                    cg_mem_uncharge(t->cgroup, 1);
                    page_cache_put(pcp);
                    return -1;
                }
                memcpy(pfn_to_virt(copy), page_cache_data(pcp), PAGE_SIZE);
                if (vma->pte_flags & PTE_X)
                    arch_flush_icache_range(pfn_to_virt(copy), PAGE_SIZE);
                page_cache_put(pcp);
                int r = fault_map(t->mm, page_va, copy, vma->pte_flags,
                                  MM_ST_FILE_PRIVATE);
                if (r < 0) {
                    cg_mem_uncharge(t->cgroup, 1);
                    frame_put(copy);
                    return -1;
                }
            }

            mm_rss_add(t->mm, 1);
            arch_tlb_flush_page_local(stval);
            return 0;
        }

        if ((vma->vm_flags & VM_VMO) && vma->vmo) {
            uint64_t voff = vma->backing_offset + (page_va - vma->start);
            if (voff >= vma->vmo->size) {
                signal_send(t->pid, SIGBUS);
                return -1;
            }
            uint32_t pg_idx = (uint32_t)(voff / PAGE_SIZE);
            pfn_t vpfn;
            int r = vmo_get_page_charged(vma->vmo, pg_idx, t->cgroup, &vpfn);
            if (r == -ENOMEM)
                return -ENOMEM;
            if (r != 0 || vpfn == PFN_NONE)
                return -1;


            if (fault_map(t->mm, page_va, vpfn, vma->pte_flags,
                          MM_ST_VMO) < 0)
                return -1;

            mm_rss_add(t->mm, 1);
            arch_tlb_flush_page_local(stval);
            return 0;
        }

#ifndef ARCH_NO_PMD_LEAF
        if (!t->policy.thp_disabled && !vma->file_vnode &&
            (vma->vm_flags & VM_HUGEPAGE) &&
            !(vma->vm_flags & VM_NOHUGEPAGE)) {
            uint64_t hbase = page_va & ~(uint64_t)(PMD_SIZE - 1);
            if (hbase >= vma->start && hbase + PMD_SIZE <= vma->end &&
                !pt_lookup_leaf(t->mm->pgdir, hbase, NULL, NULL, NULL)) {
                pfn_t hpfn = pfa_alloc(PMD_ORDER);
                if (hpfn != PFN_NONE) {
                    if (cg_mem_charge(t->cgroup, PMD_PAGE_COUNT) != 0) {
                        frame_put(hpfn);
                        return -ENOMEM;
                    }
                    memset(pfn_to_virt(hpfn), 0, PMD_SIZE);
                    int hr = pt_map_huge(t->mm, hbase, pfn_to_phys(hpfn),
                                         vma->pte_flags, MM_ST_ANON_MAPPED);
                    if (hr == 0) {
                        a20_perf_count(A20_PERF_MM_HUGE_FAULTS);
                        mm_rss_add(t->mm, PMD_PAGE_COUNT);
                        arch_tlb_flush_page_local(stval);
                        return 0;
                    }
                    cg_mem_uncharge(t->cgroup, PMD_PAGE_COUNT);
                    frame_put(hpfn);
                }
            }
        }
#endif

        /* Reserve a small forward window for private writable
         * anonymous store faults.  Compiler allocators usually touch new
         * arenas sequentially; installing four pages under one mm lock and
         * one fault return avoids three traps and repeated page-table walks.
         * Stack, shared, file, VMO and read-only mappings keep the single-page
         * path so speculative allocation cannot change their semantics. */
        if (access == MM_FAULT_ACCESS_WRITE &&
            (vma->vm_flags & (VM_ANON | VM_WRITE)) ==
                (VM_ANON | VM_WRITE) &&
            !(vma->vm_flags & (VM_SHARED | VM_STACK | VM_FILE | VM_VMO))) {
            enum { ANON_FAULT_AROUND_PAGES = 4 };
            pfn_t pfns[ANON_FAULT_AROUND_PAGES];
            size_t prepared = 0;
            size_t mapped = 0;
            uint64_t end = page_va +
                           ANON_FAULT_AROUND_PAGES * PAGE_SIZE;
            if (end < page_va || end > vma->end)
                end = vma->end;

            if (lock_held) {
                vma_get(vma);
                spin_unlock(&t->mm->lock);
            }

            for (uint64_t va = page_va; va < end; va += PAGE_SIZE) {
                pte_t *next = pt_lookup_leaf(t->mm->pgdir, va,
                                             NULL, NULL, NULL);
                if (next && (*next & PTE_V))
                    break;
                pfn_t candidate = pfa_alloc_page();
                if (candidate == PFN_NONE)
                    break;
                if (cg_mem_charge(t->cgroup, 1) != 0) {
                    frame_put(candidate);
                    break;
                }
                memset(pfn_to_virt(candidate), 0, PAGE_SIZE);
                pfns[prepared++] = candidate;
            }

            pte_t map_flags = vma->pte_flags;
            if (lock_held) {
                spin_lock(&t->mm->lock);
                pte_t *cp = pt_lookup_leaf(t->mm->pgdir, page_va,
                                           NULL, NULL, NULL);
                if (cp && (*cp & PTE_V)) {
                    for (size_t i = 0; i < prepared; i++) {
                        cg_mem_uncharge(t->cgroup, 1);
                        frame_put(pfns[i]);
                    }
                    vma_put(t->mm, vma);
                    return 0;
                }
                if (mm_seg_find(t->mm, page_va) != vma) {
                    for (size_t i = 0; i < prepared; i++) {
                        cg_mem_uncharge(t->cgroup, 1);
                        frame_put(pfns[i]);
                    }
                    vma_put(t->mm, vma);
                    return MM_FAULT_RETRY;
                } else {
                    map_flags = vma->pte_flags;
                    /* The window was sized from the VMA as it looked before
                     * the lock was dropped.  A concurrent munmap or a
                     * concurrent mprotect split can have shortened that same
                     * VMA since -- both lower vma->end in place rather than
                     * replacing the object -- so the span prepared above may
                     * now reach past the mapping.  Installing those pages
                     * anyway leaves present, zero-filled PTEs sitting in what
                     * is now a hole: the fault reports success, no thread ever
                     * wrote that memory, and the next mmap over the same
                     * address inherits the PTEs as if the application had
                     * faulted them in itself.  That is silent corruption, and
                     * it is the failure mode this window was suspected of
                     * causing, so the span is re-clamped here, under the lock,
                     * to the VMA's current end; the frames prepared beyond it
                     * are released using the slots fault_map_window()
                     * reports back. */
                    if (end > vma->end)
                        end = vma->end;
                }
            }

            /* One transaction for the whole window: a single covering-node
             * lock and a single descent, instead of one page-table walk per
             * page.  This is the property the single-level model exists for.
             * fault_map_window() keeps that where a cursor exists and falls
             * back to a per-page pt_map() where one does not; the window
             * itself, and everything the caller does with the count, is
             * identical. */
            int wmap = fault_map_window(t->mm, page_va, end, pfns, prepared,
                                        map_flags);
            if (wmap > 0)
                mapped = (size_t)wmap;
            for (size_t i = 0; i < prepared; i++) {
                if (pfns[i] == PFN_NONE)
                    continue;
                cg_mem_uncharge(t->cgroup, 1);
                frame_put(pfns[i]);
            }
            if (lock_held)
                vma_put(t->mm, vma);
            if (mapped != 0) {
                mm_rss_add(t->mm, mapped);
                a20_perf_count(A20_PERF_MM_ANON_FAULTS);
                a20_perf_count(A20_PERF_MM_ANON_BATCH_WINDOWS);
                a20_perf_add(A20_PERF_MM_ANON_BATCH_PAGES, mapped);
                arch_tlb_flush_page_local(stval);
                return 0;
            }
        }

        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) return -1;
        if (cg_mem_charge(t->cgroup, 1) != 0) {
            frame_put(pfn);
            return -ENOMEM;
        }
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        int r = fault_map(t->mm, page_va, pfn, vma->pte_flags,
                          MM_ST_ANON_MAPPED);
        if (r < 0) {
            cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            return r == -EEXIST ? MM_FAULT_RETRY : r;
        }

        mm_rss_add(t->mm, 1);
        arch_tlb_flush_page_local(stval);
        return 0;
    }

    return -1;
#endif
}

#ifndef CONFIG_NOMMU
static uint64_t fault_file_size(vnode_t *vn)
{
    if (vn && vn->ops && vn->ops->stat) {
        kstat_t st;
        if (vn->ops->stat(vn, &st) == 0) {
            vn->size = st.st_size;
            return st.st_size;
        }
    }
    return vn ? vn->size : 0;
}

static int handle_file_fault(task_t *t, uint64_t page_va,
                             uint64_t file_pos, uint64_t vma_end,
                             int shared, int fault_around, int executable,
                             int write_grant, vfile_t *vf)
{
    if (file_pos >= fault_file_size(vf->vnode)) {
        signal_send(t->pid, SIGBUS);
        vfs_put_file(vf);
        return -1;
    }
    if (!vf->vnode->ops || !vf->vnode->ops->readpage) {
        kerr("[HFF] no readpage pid=%d file=%lu shared=%d\n",
             t->pid, (unsigned long)vf->identity, shared);
        vfs_put_file(vf);
        return -1;
    }

    page_cache_page_t *window[PAGE_CACHE_FAULT_AROUND_PAGES] = {0};
    size_t window_count = 1;
    window[0] = page_cache_get(vf->vnode, file_pos / PAGE_SIZE, 1);
    if (!window[0]) {
        kerr("[HFF] cache_get NULL pid=%d fd=%d pos=%lu shared=%d\n",
             t->pid, (unsigned long)vf->identity, (unsigned long)file_pos, shared);
        vfs_put_file(vf);
        return -1;
    }

    /* Read-only MAP_PRIVATE mappings can safely populate and install a small
     * forward window.  Each installed PTE still receives its own anonymous
     * copy, so a later mprotect()/write cannot modify the file cache. */
    if (fault_around && vf->vnode->ops->readpages && vma_end > page_va) {
        uint64_t vma_pages = (vma_end - page_va) / PAGE_SIZE;
        uint64_t file_bytes = vf->vnode->size - file_pos;
        uint64_t file_pages = (file_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t limit = vma_pages < file_pages ? vma_pages : file_pages;
        if (limit > PAGE_CACHE_FAULT_AROUND_PAGES)
            limit = PAGE_CACHE_FAULT_AROUND_PAGES;
        for (uint64_t i = 1; i < limit; i++) {
            page_cache_page_t *ahead = page_cache_get(
                vf->vnode, file_pos / PAGE_SIZE + i, 1);
            if (!ahead)
                break;
            window[window_count++] = ahead;
        }
    }

    int fill_r = 0;
    int needs_fill = 0;
    for (size_t i = 0; i < window_count; i++) {
        if (!page_cache_is_uptodate(window[i])) {
            needs_fill = 1;
            break;
        }
    }
    if (needs_fill) {
        if (window_count > 1)
            fill_r = page_cache_fill_vfile_pages(vf, window, window_count);
        else
            fill_r = page_cache_fill_vfile_page(vf, window[0]);

        /* Readahead is advisory.  A later-page error must not fail the
         * hardware fault if the requested page was published successfully. */
        if (fill_r < 0 && page_cache_is_uptodate(window[0]))
            fill_r = 0;
    }
    if (fill_r < 0) {
        kerr("[HFF] fill fail pid=%d fd=%d pos=%lu shared=%d\n",
             t->pid, (unsigned long)vf->identity, (unsigned long)file_pos, shared);
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file(vf);
        return -1;
    }
    if (file_pos >= fault_file_size(vf->vnode)) {
        signal_send(t->pid, SIGBUS);
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file(vf);
        return -1;
    }

    pfn_t candidates[PAGE_CACHE_FAULT_AROUND_PAGES];
    unsigned char charged[PAGE_CACHE_FAULT_AROUND_PAGES] = {0};
    for (size_t i = 0; i < PAGE_CACHE_FAULT_AROUND_PAGES; i++)
        candidates[i] = PFN_NONE;

    /* Private file leaves may retain a canonical cache frame when the
     * backend supports batched fills.  The mapping owns its cache pin and
     * stays COW, so a later store cannot modify the cache or another mm.
     * Single-page backends keep the existing anonymous-copy policy. */
    int direct_private = !shared &&
        ((fault_around && !executable) || vf->vnode->ops->readpages);
    size_t candidate_count = shared ? 1 : window_count;
    for (size_t i = 0; i < candidate_count; i++) {
        if (!page_cache_is_uptodate(window[i]) ||
            !pfn_valid(page_cache_pfn(window[i]))) {
            candidate_count = i;
            break;
        }
        if (shared || direct_private) {
            candidates[i] = page_cache_pfn(window[i]);
            continue;
        }
        if (cg_mem_charge(t->cgroup, 1) != 0) {
            candidate_count = i;
            break;
        }
        charged[i] = 1;
        candidates[i] = pfa_alloc_page();
        if (candidates[i] == PFN_NONE) {
            cg_mem_uncharge(t->cgroup, 1);
            charged[i] = 0;
            candidate_count = i;
            break;
        }
        memcpy(pfn_to_virt(candidates[i]), page_cache_data(window[i]),
               PAGE_SIZE);
    }

    if (candidate_count == 0) {
        kerr("[HFF] no candidate pid=%d fd=%d pos=%lu shared=%d window=%lu\n",
             t->pid, (unsigned long)vf->identity, (unsigned long)file_pos, shared,
             (unsigned long)window_count);
        for (size_t i = 0; i < window_count; i++)
            page_cache_put(window[i]);
        vfs_put_file(vf);
        cg_mem_oom_kill(t->cgroup);
        return -1;
    }

    int write_guard = 0;
    if (write_grant) {
        if (mm_shared_write_enter_wait() < 0) {
            for (size_t i = 0; i < window_count; i++)
                page_cache_put(window[i]);
            vfs_put_file(vf);
            return -1;
        }
        write_guard = 1;
    }

    mm_struct_t *mm = t->mm;
retry_mapping_validation:
    spin_lock(&mm->lock);
    mm_seg_t *vma = mm_seg_find(mm, page_va);
    vfile_t *current_vf = vma && (vma->vm_flags & VM_FILE) && vma->file
        ? vma->file : NULL;
    if (current_vf)
        vfile_get(current_vf);
    int mapping_valid = vma && current_vf && current_vf->vnode == vf->vnode &&
        (vma->vm_flags & VM_FILE) &&
        mm_pte_flags_allow_access(vma->pte_flags) &&
        !!(vma->pte_flags & PTE_X) == !!executable &&
        !!(vma->vm_flags & VM_SHARED) == !!shared &&
        vma->file == vf &&
        vma->backing_offset + (page_va - vma->start) == file_pos;
    if (current_vf)
        vfs_put_file(current_vf);

    /* The initial write_grant decision was made before page-cache I/O.  A
     * concurrent mprotect may have changed this mapping from read-only to
     * writable while that I/O slept.  Re-read the current VMA under mm->lock;
     * if it now grants a shared-file write, leave the lock before waiting for
     * shutdown admission, then revalidate the mapping from scratch. */
    if (mapping_valid && shared && (vma->pte_flags & PTE_W) && !write_guard) {
        spin_unlock(&mm->lock);
        if (mm_shared_write_enter_wait() < 0) {
            for (size_t i = 0; i < window_count; i++)
                page_cache_put(window[i]);
            vfs_put_file(vf);
            return -1;
        }
        write_guard = 1;
        goto retry_mapping_validation;
    }

    int result = -1;
    size_t installed = 0;
    if (mapping_valid) {
        size_t map_count = candidate_count;
        if (shared || (vma->pte_flags & PTE_W) || !fault_around)
            map_count = 1;
        for (size_t i = 0; i < map_count; i++) {
            uint64_t va = page_va + i * PAGE_SIZE;
            uint64_t pos = file_pos + i * PAGE_SIZE;
            if (va >= vma->end ||
                vma->backing_offset + (va - vma->start) != pos)
                break;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
            if (pte && (*pte & PTE_V)) {
                if (i == 0)
                    result = 0;
                continue;
            }
            uint64_t map_flags = vma->pte_flags;
            if (direct_private) {
                /*
                 * Map the canonical page-cache frame read-only.  PTE_COW has
                 * to STAY: this leaf aliases a frame the page cache still
                 * owns, so the first store must copy rather than write
                 * through -- and PTE_COW is the only thing that routes the
                 * store to the copy in mm_fault_handle_cow(), which is
                 * written for exactly this leaf ("a read-only MAP_PRIVATE
                 * fault-around leaf maps the canonical cache frame, so this
                 * store must break that aliasing by cloning").
                 *
                 * Clearing it here left the leaf matching neither the COW
                 * branch nor the PTE_W dirty-bit branch, so the very first
                 * write to a private file page -- a .data/.bss store in
                 * ld-musl, for one -- fell through to "unhandled" and killed
                 * the process with SIGSEGV on a VMA the kernel itself
                 * considered writable.
                 */
                map_flags &= ~(uint64_t)(PTE_W | PTE_D);
                map_flags |= PTE_COW;
            }
            if (direct_private && executable)
                arch_flush_icache_range(page_cache_data(window[i]),
                                        PAGE_SIZE);
            if (pt_map_cls(mm->pgdir, va, pfn_to_phys(candidates[i]),
                           map_flags,
                           shared ? MM_ST_FILE_SHARED
                                  : MM_ST_FILE_PRIVATE) < 0)
                break;
            mm_rss_add(mm, 1);
            installed++;
            candidates[i] = PFN_NONE;
            charged[i] = 0;
            if (i == 0)
                result = 0;
            if (shared || direct_private)
                window[i] = NULL; /* Mapping retains the page-cache pin. */
        }
        if (installed > 1)
            arch_tlb_flush_local();
        else if (installed == 1 || result == 0)
            arch_tlb_flush_page_local(page_va);
    }
    spin_unlock(&mm->lock);
    if (write_guard)
        mm_shared_write_leave();

    for (size_t i = 0; i < candidate_count; i++) {
        if (candidates[i] != PFN_NONE && !shared && !direct_private)
            frame_put(candidates[i]);
        if (charged[i])
            cg_mem_uncharge(t->cgroup, 1);
    }
    for (size_t i = 0; i < window_count; i++) {
        if (window[i])
            page_cache_put(window[i]);
    }
    vfs_put_file(vf);
    return result;
}
#endif

/*
 * MM_COW_FROM_STATUS -- the status-led private-anon COW path.
 *
 * A write fault whose leaf says class MM_ST_ANON_MAPPED with the COW bit is
 * a private anonymous page fork made shared.  The caller holds mm->lock so it
 * excludes the legacy/cache fallback and fork/mprotect transactions; the
 * cursor leaf lock still protects the individual snapshot and replacement:
 *   - the class answers "anonymous" (file pages need the page-cache branch
 *     and VMO pages answer to the VMO, so both decline here);
 *   - the PTE is re-verified as still COW/still this frame UNDER the leaf
 *     lock by mm_cursor_replace_if_cow(), which is the same lock fork's
 *     parent-side rewrite and mprotect's prot rewrite now take;
 *   - the snapshot and frame pin are atomic under leaf-node then pfa.lock, so
 *     another COW replacement cannot free the old frame before the pin;
 *   - rc==1 declines: the in-place upgrade must exclude fork, which
 *     stays on the common mm->lock path.
 *
 * Copying a frame that turned out exclusive is a waste, never a bug: the
 * COW bit only means "was shared when we looked", and every sharer copies
 * or upgrades under its own exclusion.
 */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
/*
 * The status-led COW break lives in kernel/mm/cow_status.rs.  What is left here
 * is the address-space cursor and the primitives the break needs: the leaf
 * probe and pin, the compare-and-replace, the non-reclaiming frame allocator,
 * the cgroup charge and the counters.  The cursor stays C-owned, so Rust holds
 * it in an opaque buffer whose size is asserted below rather than mirroring
 * mm_cursor_t.
 */
struct a20_fault_status_ctx {
    mm_cursor_t cur;
};

_Static_assert(sizeof(struct a20_fault_status_ctx) <= 16 * sizeof(uint64_t),
               "Rust/C status-COW cursor buffer");
_Static_assert(_Alignof(struct a20_fault_status_ctx) <= _Alignof(uint64_t),
               "Rust/C status-COW cursor alignment");

extern int a20_cow_from_status_rust(task_t *t, mm_struct_t *mm,
                                    uint64_t page_va, pfn_t *retired_pfn);

/* Open the cursor over the page and pin the leaf's frame.  Returns 1 with the
 * facts filled and the cursor open (the caller must close it), or 0 when the
 * leaf is not there to break -- in which case the cursor has already been
 * released and no lock is held. */
int a20_cowf_status_open(void *ctx_ptr, mm_struct_t *mm, uint64_t page_va,
                         uint8_t *cls_out, uintptr_t *pa_out, int *uffd_out)
{
    struct a20_fault_status_ctx *ctx = ctx_ptr;
    *cls_out = 0;
    *pa_out = 0;
    *uffd_out = 0;
    if (mm_addrspace_lock(mm, page_va, page_va + PAGE_SIZE, &ctx->cur) != 0)
        return 0;
    paddr_t old_pa = 0;
    int pinned = mm_cursor_cow_snapshot_pin(&ctx->cur, page_va, cls_out, &old_pa);
    if (pinned != 1) {
        mm_cursor_unlock(&ctx->cur);
        return 0;
    }
    /* A userfaultfd-marked page is left to the uffd path.  The mark is read
     * here because the cursor is the only holder of the leaf lock. */
    *uffd_out = mm_cursor_safe_test(&ctx->cur, page_va, MM_SAFE_UFFD);
    *pa_out = old_pa;
    return 1;
}

int a20_cowf_status_replace(void *ctx_ptr, uint64_t page_va, uintptr_t expect_pa,
                            uintptr_t new_pa, uint8_t cls, uintptr_t *replaced_out)
{
    struct a20_fault_status_ctx *ctx = ctx_ptr;
    paddr_t replaced = 0;
    int r = mm_cursor_replace_if_cow(&ctx->cur, page_va, expect_pa, new_pa, cls,
                                     &replaced);
    if (replaced_out)
        *replaced_out = replaced;
    return r;
}

void a20_cowf_status_close(void *ctx_ptr)
{
    struct a20_fault_status_ctx *ctx = ctx_ptr;
    mm_cursor_unlock(&ctx->cur);
}

/* Non-reclaiming: a reclaiming allocation reaches oom_try_reclaim(), whose
 * victim teardown wants the page-table node locks this path holds. */
uint32_t a20_fault_status_alloc(void)
{
    return pfa_alloc_flags(0, 0);
}

uint32_t a20_cowf_status_pfn_of(uintptr_t pa)
{
    return phys_to_pfn(pa);
}

int a20_cowf_status_pfn_valid(uint32_t pfn)
{
    return pfn_valid(pfn) ? 1 : 0;
}

uintptr_t a20_fault_status_phys(uint32_t pfn)
{
    return pfn_to_phys(pfn);
}

int a20_fault_status_charge(task_t *t)
{
    return cg_mem_charge(t->cgroup, 1);
}

void a20_fault_status_uncharge(task_t *t)
{
    cg_mem_uncharge(t->cgroup, 1);
}

void a20_cowf_status_copy(uint32_t dst, uint32_t src)
{
    memcpy(pfn_to_virt(dst), pfn_to_virt(src), PAGE_SIZE);
}

void a20_cowf_status_bump(task_t *t)
{
    a20_perf_count(A20_PERF_MM_COW_FAULTS);
    a20_perf_count(A20_PERF_MM_COW_FROM_STATUS);
    mm_cow_from_status_count++;
    __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
}

static int mm_cow_from_status(task_t *t, mm_struct_t *mm, uint64_t page_va,
                              pfn_t *retired_pfn)
{
    return a20_cow_from_status_rust(t, mm, page_va, retired_pfn);
}
#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */

int handle_cow_fault_mm(task_t *t, mm_struct_t *mm, uint64_t stval)
{
#ifdef CONFIG_NOMMU
    return handle_cow_fault_locked(t, mm, stval, NULL, NULL);
#else
    if (!t || !mm)
        return -1;
    /* The status-led copy and the legacy/cache fallback both mutate this
     * address space's leaf.  Keep them in the same per-mm transaction so the
     * fallback's in-place rc==1 upgrade cannot race a status-led replacement.
     * Its allocation is non-reclaiming, so this lock does not enter reclaim. */
    /* Fork publishes read-only PTEs before its shootdown completes. A COW
     * copy must wait for that transaction: a sibling still using a writable
     * TLB entry could otherwise update the old frame after we copied it. */
    mutex_lock(&mm->tlb_lock);
    spin_lock(&mm->lock);
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    /* Prefer the status-led private-anon copy; it uses a cursor and compare-
     * replace.  The per-mm lock also excludes the legacy fallback below. */
    pfn_t status_old_pfn = PFN_NONE;
    if (mm_cow_from_status(t, mm,
                           stval & ~(uint64_t)(PAGE_SIZE - 1),
                           &status_old_pfn)) {
        spin_unlock(&mm->lock);
        if (status_old_pfn != PFN_NONE) {
            mm_tlb_shootdown_page(mm, stval);
            frame_put(status_old_pfn);
            frame_put(status_old_pfn);
        }
        mutex_unlock(&mm->tlb_lock);
        return 0;
    }
#endif
    pfn_t old_pfn = PFN_NONE;
    page_cache_page_t *old_page = NULL;
    int r = handle_cow_fault_locked(t, mm, stval, &old_pfn, &old_page);
    spin_unlock(&mm->lock);
    if (r == 0) {
        a20_perf_count(A20_PERF_MM_COW_FAULTS);
        mm_tlb_shootdown_page(mm, stval);
    }
    if (old_pfn != PFN_NONE)
        frame_put(old_pfn);
    if (old_page) {
        /* Drop the temporary lookup and the direct mapping's retained pin
         * only after every CPU has discarded the old PTE. */
        page_cache_put(old_page);
        page_cache_put(old_page);
    }
    if (r == 0 && t) {
        __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
    }
    mutex_unlock(&mm->tlb_lock);
    return r;
#endif
}

int handle_cow_fault(task_t *t, uint64_t stval)
{
    if (!t || !t->mm)
        return -1;
    return handle_cow_fault_mm(t, t->mm, stval);
}

int handle_demand_fault(task_t *t, uint64_t stval)
{
    return handle_demand_fault_access(t, stval, MM_FAULT_ACCESS_READ);
}

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
/* MM_AS_FAULT_FROM_STATUS -- the paper's fault handler (Fig. 8) decides from
 * per-PTE status alone: query() yields Status::PrivateAnon / Mapped / Invalid,
 * and PrivateAnon is mapped directly using the permissions recorded at mmap.  No
 * VMA is consulted, which is exactly what the paper credits for its advantage
 * over Linux -- "the time Linux spends in the VMA" (6.2).  That is also why
 * this runs BEFORE spin_lock(&mm->lock): it touches no VMA, so there is nothing
 * there for that lock to be protecting.
 *
 * Everything it does touch is cursor-owned or atomic: mm_rss_add() and the perf
 * counters were made atomic in Phase 0, cg_mem_charge() takes only its own
 * node->lock, and mm_cursor_query() reports a swap entry as already-present with
 * class MM_ST_SWAPPED, so the !already test declines swap without needing the
 * separate swap check the mm->lock path has to do.
 *
 * The page allocation is pfa_alloc_flags(0, 0) -- can_reclaim = 0 on purpose.  A
 * reclaiming allocator reaches oom_try_reclaim(), which swaps pages out and calls
 * proc_force_exit(); tearing the victim down runs pt_unmap_leaf(), which wants a
 * page-table node MCS lock this path is holding.  On exhaustion the fast path
 * declines and the VMA path allocates with reclaim, under mm->lock, where
 * sleeping is legal.
 *
 * Returns 1 if it served the fault, 0 if the caller must take the VMA path. */
/*
 * The status-led demand fault lives in kernel/mm/fault_status.rs.  The cursor
 * context and the four primitives both status-led paths share are declared with
 * the COW break above; what is added here is the query, the absent-only
 * install, the zeroing, the accounting and the local flush.
 */
extern int a20_fault_from_status_rust(task_t *t, mm_struct_t *mm,
                                      uint64_t page_va, uint64_t stval,
                                      int access);

/* Open the cursor over the page and report the entry's facts.  Returns 1 with
 * the cursor open -- the caller decides and closes it -- or 0 with no lock held
 * when there is no page table to walk. */
int a20_fault_status_open(void *ctx_ptr, mm_struct_t *mm, uint64_t page_va,
                          uint8_t *cls_out, int *already_out, int *uffd_out)
{
    struct a20_fault_status_ctx *ctx = ctx_ptr;
    *cls_out = 0;
    *already_out = 0;
    *uffd_out = 0;
    if (mm_addrspace_lock(mm, page_va, page_va + PAGE_SIZE, &ctx->cur) != 0)
        return 0;
    *already_out = mm_cursor_query(&ctx->cur, page_va, cls_out, NULL);
    *uffd_out = mm_cursor_safe_test(&ctx->cur, page_va, MM_SAFE_UFFD);
    return 1;
}

int a20_fault_status_map_if_absent(void *ctx_ptr, uint64_t page_va,
                                   uintptr_t pa, uintptr_t allow, uint8_t cls)
{
    struct a20_fault_status_ctx *ctx = ctx_ptr;
    return mm_cursor_map_if_absent(&ctx->cur, page_va, pa, allow, cls);
}

void a20_fault_status_close(void *ctx_ptr)
{
    struct a20_fault_status_ctx *ctx = ctx_ptr;
    mm_cursor_unlock(&ctx->cur);
}

void a20_fault_status_zero(uint32_t pfn)
{
    memset(pfn_to_virt(pfn), 0, PAGE_SIZE);
}

void a20_fault_status_rss_add(mm_struct_t *mm)
{
    mm_rss_add(mm, 1);
}

void a20_fault_status_bump(task_t *t)
{
    a20_perf_count(A20_PERF_MM_ANON_FAULTS);
    a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
    a20_perf_count(A20_PERF_MM_FAULT_FROM_STATUS);
    /* The per-task / global soft-fault counters the VMA paths bump alongside
     * rss++; a fault path that skips them makes every reader of these, and
     * /proc's reported fault rate, wrong. */
    __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&t->perf_page_faults_maj, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&g_perf_sw_page_faults_maj, 1, __ATOMIC_RELAXED);
}

void a20_fault_status_flush_local(uint64_t stval)
{
    arch_tlb_flush_page_local(stval);
}

static int mm_fault_from_status(task_t *t, mm_struct_t *mm, uint64_t page_va,
                                uint64_t stval, enum mm_fault_access access)
{
    return a20_fault_from_status_rust(t, mm, page_va, stval, (int)access);
}
#endif /* ARCH_HAS_PGTABLE_OPS && !CONFIG_NOMMU */

int handle_demand_fault_access(task_t *t, uint64_t stval,
                               enum mm_fault_access access)
{
#ifdef CONFIG_NOMMU
    return handle_demand_fault_locked(t, stval, access, 0);
#else
    /* MM_FAULT_RETRY is a benign VMA-identity change under the fault-around
     * window, so the entry point owns the retry rather than letting each
     * handler roll its own.  Re-entering re-resolves the VMA under the lock,
     * which is exactly the step the window could not do while unlocked. */
    for (int attempt = 0; attempt < MM_FAULT_RETRY_MAX; attempt++) {
        int r = handle_demand_fault_attempt(t, stval, access);
        if (r != MM_FAULT_RETRY)
            return r;
    }
    return -1;
#endif
}

#ifndef CONFIG_NOMMU
static int handle_demand_fault_attempt(task_t *t, uint64_t stval,
                                       enum mm_fault_access access)
{
    if (!t || !t->mm || !t->mm->pgdir)
        return -1;

    mm_struct_t *mm = t->mm;
    uint64_t page_va = stval & ~(PAGE_SIZE - 1);
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    /* Lock-free: the status fast path runs before mm->lock is taken. */
    if (mm_fault_from_status(t, mm, page_va, stval, access))
        return 0;
#endif
    spin_lock(&mm->lock);
    pte_t *pte = pt_lookup_leaf(mm->pgdir, page_va, NULL, NULL, NULL);
#ifdef CONFIG_SWAP
    if (pte && pte_is_swap(*pte)) {
        /* Read outside the MM lock, retaining and revalidating the slot. */
        spin_unlock(&mm->lock);
        int r = handle_swap_fault(t, stval, access);
        if (r == -ENOMEM) {
            cg_mem_oom_kill(t->cgroup);
            return -1;
        }
        return r;
    }
#endif
    if (pte && (*pte & PTE_V)) {
        spin_unlock(&mm->lock);
        return handle_present_page_fault(t, stval, access);
    }


    mm_seg_t *vma = mm_seg_find(mm, page_va);
    if (vma && !fault_access_allowed(vma->vm_flags, access)) {
        spin_unlock(&mm->lock);
        return -1;
    }
    /*
     * USERFAULTFD_MISSING_HOOK: anonymous private ranges registered with a
     * userfaultfd hand the fault to the handler before the kernel fabricates
     * a zero page.  The mm lock is dropped before the handler parks so the
     * faulter can sleep; when the handler resolves the page (COPY/ZEROPAGE)
     * or the range is unregistered, the fault is retried from the top.
     */
    if (vma &&
        (vma->vm_flags & (VM_ANON | VM_FILE | VM_VMO | VM_SHARED)) == VM_ANON &&
        userfaultfd_range_present(mm, page_va)) {
        spin_unlock(&mm->lock);
        if (userfaultfd_handle_fault(t, mm, page_va,
                                     access == MM_FAULT_ACCESS_WRITE) < 0)
            return -1;
        return handle_demand_fault_access(t, stval, access);
    }
    /* P6: the backing object comes off the page-table path, not off the VMA.
     *
     * The segment named for this address carries everything the file-fault
     * path needs -- the open file description, sharedness, protection, the
     * object offset for this very page, and the end of the mapping -- so when
     * one covers the address it IS the authority.  `vma` is still consulted, for two reasons that are both
     * temporary and both load-bearing: it is the fallback for addresses no
     * segment covers yet, and shadow_seg_check() counts every disagreement
     * between the two resolutions.  A disagreement is a wrong-page fault if the segment
     * wins, so it is counted, not obeyed.
     *
     * Put here and not in handle_demand_fault_locked(): this dispatcher claims
     * VM_FILE first and only falls through when it declines, so instrumenting
     * the other one records nothing and looks like a passing measurement. */
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
    mm_seg_t *seg = mm_pt_lookup_seg(mm, page_va);
#else
    /* No page-table segment table on this backend: the VMA fallback below is
     * the only authority, and the dispatch counters record that honestly. */
    mm_seg_t *seg = NULL;
#endif
    if (seg && mm_seg_kind(seg) == MM_SEG_FILE && seg->file) {
        mm_seg_dispatch_seg++;
        if (vma)
            shadow_seg_check(mm, vma, page_va, MM_SEG_FILE, vma->backing_offset);

        uint64_t sflags = seg->vm_flags;
        int prot = 0;
        if (sflags & VM_READ)  prot |= PROT_READ;
        if (sflags & VM_WRITE) prot |= PROT_WRITE;
        if (sflags & VM_EXEC)  prot |= PROT_EXEC;
        pte_t seg_ptef = mm_prot_to_pte_flags(prot);
        if (!fault_access_allowed(sflags, access)) {
            mm_seg_put(seg);
            spin_unlock(&mm->lock);
            return -1;
        }
        int shared   = mm_seg_shared(seg);
        /* The callee takes this as uint64_t.  Narrowing it to int here
         * truncated any offset at or above 4 GiB -- and, because the low 12
         * bits survive, file_pos / PAGE_SIZE still named a page that is
         * inside the file, so the fault silently read the WRONG page of the
         * right file instead of failing. */
        uint64_t file_pos = seg->backing_offset + (page_va - seg->start);
        vaddr_t seg_end = seg->end;
        /* Take our own reference on the open file description: the segment's
         * own reference can drop the moment mm_seg_put() does, and the fault
         * runs with mm->lock released. */
        vfile_t *seg_vf = seg->file;
        vfile_get(seg_vf);
        mm_seg_put(seg);
        /* The page-table path for this range may not have existed when this
         * fault arrived -- mmap builds no path, and the first touch is what
         * creates it.  Re-apply the mapping's segment so the NEXT fault here
         * is answerable from the page tables alone.  Without this the segment
         * table answers almost nothing: measured 6532 hits against 37626
         * misses. */
        if (vma)
            mm_mmap_seg_label(mm, vma);

        /* Writable private mappings stay on the single-page COW path.  A
         * read-only private mapping, including executable text, can share the
         * canonical page-cache frame.  mprotect(PROT_WRITE) converts the leaf
         * to COW before exposing writes, and unmap/exit drops the mapping's
         * cache pin.  This avoids allocating and copying the same rustc text
         * pages independently in every parallel compiler process. */
        int executable = (seg_ptef & PTE_X) != 0;
        int fault_around = !shared && !(seg_ptef & PTE_W);
        spin_unlock(&mm->lock);
        vfile_t *vf = seg_vf;
        if (!vf || !vf->vnode) {
            if (vf)
                vfs_put_file(vf);
            return -1;
        }
        int write_grant = shared && (seg_ptef & PTE_W);
        int r = handle_file_fault(t, page_va, file_pos, seg_end,
                                  shared, fault_around, executable,
                                  write_grant, vf);
        if (r == 0) {
            a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
            a20_perf_count(A20_PERF_MM_FILE_FAULTS);
            __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
            t->perf_page_faults_maj++;
        }
        return r;
    }
    mm_seg_put(seg);
    if (vma && (vma->vm_flags & VM_FILE) && vma->file) {
        mm_seg_dispatch_fallback++;
        if (!fault_access_allowed(vma->vm_flags, access)) {
            spin_unlock(&mm->lock);
            return -1;
        }
        int shared = (vma->vm_flags & VM_SHARED) != 0;
        /* Fallback: no segment covers this address yet.  This is the coverage
         * gap P6 has to close before the VMA query can go -- measured, not
         * guessed: seg_miss in the [MM-ASM] line. */
        shadow_seg_check(mm, vma, page_va, MM_SEG_FILE, vma->backing_offset);
        mm_mmap_seg_label(mm, vma);
        /* Writable private mappings stay on the single-page COW path.  A
         * read-only private mapping, including executable text, can share the
         * canonical page-cache frame.  mprotect(PROT_WRITE) converts the leaf
         * to COW before exposing writes, and unmap/exit drops the mapping's
         * cache pin.  This avoids allocating and copying the same rustc text
         * pages independently in every parallel compiler process. */
        int executable = (vma->pte_flags & PTE_X) != 0;
        int fault_around = !shared && !(vma->pte_flags & PTE_W);
        uint64_t vma_end = vma->end;
        uint64_t file_pos = vma->backing_offset + (page_va - vma->start);
        int write_grant = shared && (vma->pte_flags & PTE_W);
        vfile_t *vf = vma->file;
        vfile_get(vf);
        spin_unlock(&mm->lock);
        if (!vf || !vf->vnode) {
            if (vf)
                vfs_put_file(vf);
            return -1;
        }
        int r = handle_file_fault(t, page_va, file_pos, vma_end,
                                  shared, fault_around, executable,
                                  write_grant, vf);
        if (r == 0) {
            a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
            a20_perf_count(A20_PERF_MM_FILE_FAULTS);
            __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
            __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
            t->perf_page_faults_maj++;
        }
        return r;
    }

    /*
     * PAGED-VMO hook: a VM_VMO mapping backed by a VMO_PAGED vmo with a
     * pager hands unmaterialized faults to the user-space pager.  The mm
     * lock is dropped before parking (mirrors the userfaultfd hook above);
     * vmo_paged_fault() loops until the pager supplies the page or the
     * pager disappears.
     */
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo &&
        vma->vmo->type == VMO_PAGED) {
        uint64_t voff = vma->backing_offset + (page_va - vma->start);
        uint32_t pg_idx = (uint32_t)(voff / PAGE_SIZE);
        int paged_miss = 0;
        spin_lock(&vma->vmo->lock);
        if (pg_idx < vma->vmo->page_count &&
            vma->vmo->pages[pg_idx] == PFN_NONE && vma->vmo->pager)
            paged_miss = 1;
        spin_unlock(&vma->vmo->lock);
        if (paged_miss) {
            spin_unlock(&mm->lock);
            int pr = vmo_paged_fault(vma->vmo, pg_idx,
                                     access == MM_FAULT_ACCESS_WRITE);
            if (pr == -EAGAIN)
                return MM_FAULT_RETRY;
            if (pr < 0)
                return -1;
            return handle_demand_fault_access(t, stval, access);
        }
    }

    int r = handle_demand_fault_locked(t, stval, access, 1);
    spin_unlock(&mm->lock);
    if (r == -ENOMEM) {
        cg_mem_oom_kill(t->cgroup);
        return -1;
    }
    if (r == 0) {
        a20_perf_count(A20_PERF_MM_DEMAND_FAULTS);
        __atomic_fetch_add(&t->perf_page_faults, 1, __ATOMIC_RELAXED);
        __atomic_fetch_add(&g_perf_sw_page_faults, 1, __ATOMIC_RELAXED);
    }
    return r;
}

#endif /* !CONFIG_NOMMU */

int handle_present_page_fault(task_t *t, uint64_t stval,
                              enum mm_fault_access access)
{
#ifdef CONFIG_NOMMU
    (void)t;
    (void)stval;
    (void)access;
    return -1;
#else
    if (!t || !t->mm || !t->mm->pgdir)
        return -1;

    mm_struct_t *mm = t->mm;
    if (access == MM_FAULT_ACCESS_WRITE) {
        uint64_t flags = spin_lock_irqsave(&mm->lock);
        pte_t *leaf = pt_lookup_leaf(mm->pgdir, stval, NULL, NULL, NULL);
        mm_seg_t *vma = mm_seg_find(mm, stval);
        int candidate = leaf && (*leaf & PTE_V) && arch_pte_is_leaf(*leaf) &&
            !(*leaf & (PTE_W | PTE_COW)) && vma &&
            (vma->vm_flags & (VM_FILE | VM_SHARED | VM_WRITE)) ==
                (VM_FILE | VM_SHARED | VM_WRITE);
        spin_unlock_irqrestore(&mm->lock, flags);

        if (candidate) {
            if (mm_shared_write_enter_wait() < 0)
                return -1;
            mm_tlb_invalidate_begin(mm);
            flags = spin_lock_irqsave(&mm->lock);
            leaf = pt_lookup_leaf(mm->pgdir, stval, NULL, NULL, NULL);
            vma = mm_seg_find(mm, stval);
            int regrant = leaf && (*leaf & PTE_V) &&
                arch_pte_is_leaf(*leaf) && !(*leaf & PTE_COW) && vma &&
                (vma->vm_flags & (VM_FILE | VM_SHARED | VM_WRITE)) ==
                    (VM_FILE | VM_SHARED | VM_WRITE);
            if (regrant && !(*leaf & PTE_W)) {
#if !defined(ARCH_HAS_PGTABLE_OPS)
                /* Publishing the regranted protection goes through the per-PTE
                 * status sidecar, which this architecture does not have; fall
                 * through to the ordinary fault path below instead. */
                regrant = 0;
#else
                int level = 0;
                (void)pt_lookup_leaf(mm->pgdir, stval, &level, NULL, NULL);
                pte_t *table = mm_pt_leaf_table(mm->pgdir, stval);
                if (!table) {
                    regrant = 0;
                } else {
                    mm_pt_node_lock(table);
                    pte_t old = __atomic_load_n(leaf, __ATOMIC_ACQUIRE);
                    for (;;) {
                        if (!(old & PTE_V) || !arch_pte_is_leaf(old) ||
                            (old & PTE_COW)) {
                            regrant = 0;
                            break;
                        }
                        if (old & PTE_W)
                            break;
                        pte_t desired = arch_pte_leaf(arch_pte_addr(old),
                            arch_pte_flags(old) | (pte_t)PTE_W);
                        if (__atomic_compare_exchange_n(leaf, &old, desired,
                                0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                            (void)mm_pt_refresh_leaf_prot(table,
                                arch_pt_vpn(stval, level), desired);
                            mm_tlb_note_change(mm, stval &
                                ~(vaddr_t)(PAGE_SIZE - 1), PAGE_SIZE);
                            break;
                        }
                    }
                    mm_pt_node_unlock(table);
                }
#endif /* ARCH_HAS_PGTABLE_OPS */
            }
            spin_unlock_irqrestore(&mm->lock, flags);
            mm_tlb_invalidate_finish(mm);
            mm_shared_write_leave();
            if (regrant)
                return 0;
        }
    }

    spin_lock(&mm->lock);
    pte_t *pte = pt_lookup_leaf(mm->pgdir, stval, NULL, NULL, NULL);
    int allowed = pte && (*pte & PTE_V) && arch_pte_is_leaf(*pte) &&
                  (*pte & PTE_U);
    int retry_cow = allowed && access == MM_FAULT_ACCESS_WRITE &&
                    (*pte & PTE_COW);
    if (allowed) {
        if (access == MM_FAULT_ACCESS_WRITE)
            allowed = (*pte & PTE_W) != 0;
        else if (access == MM_FAULT_ACCESS_EXEC)
            allowed = (*pte & PTE_X) != 0;
        else
            allowed = (*pte & PTE_R) != 0;
    }
    /*
     * The PTE is not the only authority on what this address may be used
     * for: the VMA is.  A leaf can carry an execute bit the VMA never
     * granted -- a stack page whose leaf was installed from a stale flag
     * word, or a COW copy that propagated PTE_X from the parent's leaf --
     * and on every architecture whose descriptor encodes UXN/PXN
     * (aarch64's arch_pte_leaf() derives them from PTE_X) that leaf is then
     * genuinely executable at EL0.  Trusting it alone turns a jump through
     * such a leaf into a "handled" fault: the retry succeeds, the PC does
     * not advance to anything meaningful, and the same address faults again
     * immediately.  That is an unbounded silent trap loop -- a shell whose
     * stack page went executable spins on a prefetch abort at a stack
     * address until something external kills the machine, with no fault
     * report, because every round trip reported success.
     *
     * Refuse the access whenever the VMA covering the address does not grant
     * it.  The leaf may then be as wrong as it likes and the worst outcome is
     * the correct one: a clean SIGSEGV naming a mapping the process was never
     * allowed to execute.
     */
    if (allowed || retry_cow) {
        mm_seg_t *vma = mm_seg_find(mm, stval);
        if (vma && !fault_access_allowed(vma->vm_flags, access)) {
            allowed = 0;
            retry_cow = 0;
        }
    }

    /*
     * Radix-style MMUs take a reference/access (R/C) fault on a present
     * page whose R bit is clear.  Mark the leaf referenced so the retry
     * does not re-fault; the TLB flush below drops the stale entry.
     */
    if (allowed && pte)
        *pte |= PTE_A;
    spin_unlock(&mm->lock);

    /* A concurrent fork can write-protect a leaf between the initial COW
     * attempt and demand-fault recheck. Resolve that new COW state rather
     * than rejecting a write the VMA permits. */
    if (retry_cow)
        return handle_cow_fault(t, stval);
    if (!allowed)
        return -1;

    arch_tlb_flush_page_local(stval);
    return 0;
#endif
}
