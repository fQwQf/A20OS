#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/shutdown_write.h"
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

/* mprotect: permission changes on an existing address range. */

/* Caller holds mm->lock. Only shared file VMAs can expose a writable
 * page-cache mapping to shutdown; anonymous/private permission changes do not
 * need to join that admission transaction. */
static int mm_mprotect_needs_shared_write_locked(mm_struct_t *mm,
                                                  vaddr_t addr,
                                                  size_t len,
                                                  int prot)
{
    if (!(prot & PROT_WRITE) || !len || addr >= USER_VA_LIMIT)
        return 0;
    /* Bound the round-up and the end computation by the native address width
     * rather than by UINT64_MAX: on a 32-bit build a len/addr pair that wraps
     * the address width has to be rejected here, or the scan below stops at
     * the wrapped end and skips the shared-file mappings past it -- which is
     * exactly the range that must join the shutdown admission transaction. */
    vaddr_t limit = (vaddr_t)-1;
    if (len > limit - (PAGE_SIZE - 1))
        return 0;
    len = ROUND_UP(len, PAGE_SIZE);
    if ((vaddr_t)len > limit - addr)
        return 0;
    vaddr_t end = addr + len;
    for (mm_seg_t *v = mm_seg_find(mm, addr); v && v->start < end;
         v = v->next) {
        if (v->end > addr &&
            (v->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED))
            return 1;
    }
    return 0;
}

/* AArch64 aliases the hardware dirty indication with write permission and
 * ARM32 has no dirty bit. Preserve the cache's dirty state before mprotect
 * removes the only software-visible evidence from a shared file leaf. */
#if PTE_D == 0 || PTE_D == PTE_W
static void mm_mprotect_harvest_shared_write(mm_seg_t *vma, vaddr_t va)
{
    if (!vma || !vma->file_vnode)
        return;
    uint64_t index = (vma->backing_offset + (va - vma->start)) / PAGE_SIZE;
    page_cache_page_t *cached = page_cache_get(vma->file_vnode, index, 0);
    if (cached) {
        page_cache_mark_dirty(cached);
        page_cache_put(cached);
    }
}
#endif

/*
 * The protection-change transaction lives in kernel/mm/mprotect_txn.rs.  What
 * is left here is what it cannot name: the mm_seg_t field access, the split
 * primitive, the architecture's PTE bit definitions and encoders, the page
 * table walk and leaf install with its owning node lock, the physical-frame
 * reference peek, the instruction-cache flush and the page-cache lookup.
 */
extern int a20_mprotect_locked_rust(mm_struct_t *mm, vaddr_t addr, size_t len,
                                    int prot);

struct a20_mprotect_rec {
    void     *vma;
    uintptr_t start;
    uintptr_t end;
    uint64_t  vm_flags;
    uintptr_t pte_flags;
    uint32_t  vmar_cap;
    uint32_t  reserved;
};
struct a20_mprotect_leaf {
    uintptr_t pte;
    uintptr_t base;
    uintptr_t size;
    uintptr_t pa;
    uint32_t  pfn;
    int32_t   level;
    int32_t   present;
    int32_t   reserved;
};
struct a20_mprotect_bits {
    uintptr_t w;
    uintptr_t d;
    uintptr_t x;
    uintptr_t cow;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_mprotect_rec) == 48, "Rust/C mprotect rec ABI");
_Static_assert(sizeof(struct a20_mprotect_leaf) == 48, "Rust/C mprotect leaf ABI");
_Static_assert(sizeof(struct a20_mprotect_bits) == 32, "Rust/C mprotect bits ABI");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(sizeof(struct a20_mprotect_rec) == 40, "Rust/C mprotect rec ABI");
_Static_assert(sizeof(struct a20_mprotect_leaf) == 32, "Rust/C mprotect leaf ABI");
_Static_assert(sizeof(struct a20_mprotect_bits) == 16, "Rust/C mprotect bits ABI");
#endif

int a20_mprotect_has_pgdir(const void *mm_ptr)
{
    const mm_struct_t *mm = mm_ptr;
    return mm && mm->pgdir;
}

uintptr_t a20_mprotect_user_va_limit(void)
{
    return USER_VA_LIMIT;
}

int a20_mprotect_is_nommu(void)
{
#ifdef CONFIG_NOMMU
    return 1;
#else
    return 0;
#endif
}

int a20_mprotect_record_from(const void *mm_ptr, uintptr_t va,
                             struct a20_mprotect_rec *out)
{
    const mm_struct_t *mm = mm_ptr;
    if (!mm || !out)
        return 0;
    for (mm_seg_t *v = mm->mmap; v; v = v->next) {
        if (v->end <= va)
            continue;
        out->vma = v;
        out->start = v->start;
        out->end = v->end;
        out->vm_flags = v->vm_flags;
        out->pte_flags = v->pte_flags;
        out->vmar_cap = v->vmar_cap;
        return 1;
    }
    return 0;
}

int a20_mprotect_split_at(void *mm_ptr, uintptr_t addr)
{
    return mm_split_vma_at(mm_ptr, addr);
}

void a20_mprotect_set_vma(void *vma_ptr, uintptr_t pte_flags, uint64_t vm_flags)
{
    mm_seg_t *v = vma_ptr;
    if (!v)
        return;
    v->pte_flags = (pte_t)pte_flags;
    v->vm_flags = vm_flags;
}

void *a20_mprotect_merge(void *mm_ptr, void *vma_ptr, uintptr_t *next_va)
{
    mm_seg_t *v = vma_try_merge(mm_ptr, vma_ptr);
    if (next_va)
        *next_va = v ? v->end : 0;
    return v;
}

void a20_mprotect_bits(struct a20_mprotect_bits *out)
{
    if (!out)
        return;
    out->w = PTE_W;
    out->d = PTE_D;
    out->x = PTE_X;
    out->cow = PTE_COW;
}

#ifndef CONFIG_NOMMU
void a20_mprotect_probe(const void *mm_ptr, uintptr_t va,
                        struct a20_mprotect_leaf *out)
{
    const mm_struct_t *mm = mm_ptr;
    if (!out)
        return;
    out->pte = 0;
    out->base = 0;
    out->size = 0;
    out->pa = 0;
    out->pfn = PFN_NONE;
    out->level = 0;
    out->present = 0;
    out->reserved = 0;
    /* Bridge the width-fixed Rust ABI struct (int32_t/uintptr_t) to
     * pt_lookup_leaf()'s native int/vaddr_t: same width, different C type on
     * a 32-bit target. */
    int level = 0;
    vaddr_t base = 0;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &out->size);
    out->level = level;
    out->base = base;
    if (!pte)
        return;
    out->pte = (uintptr_t)*pte;
    out->present = (*pte & PTE_V) != 0;
    if (out->present) {
        out->pa = arch_pte_addr(*pte);
        out->pfn = phys_to_pfn(out->pa);
    }
}

/* Install one leaf.  The owning node lock makes the snapshot check and the
 * store one step: a status-driven COW that replaced the leaf between the
 * probe and here is reported as 0 so the transaction recomputes for the same
 * address.  The status sidecar is refreshed under the same lock, because that
 * is the byte the lockless fault paths read. */
int a20_mprotect_install(void *mm_ptr, uintptr_t va, uintptr_t observed,
                         uintptr_t pa, uintptr_t flags)
{
    mm_struct_t *mm = mm_ptr;
    pte_t *pte = pt_lookup_leaf(mm->pgdir, va, NULL, NULL, NULL);
    if (!pte)
        return 0;
#if defined(ARCH_HAS_PGTABLE_OPS)
    pte_t *ltab = mm_pt_leaf_table(mm->pgdir, va);
    if (ltab)
        mm_pt_node_lock(ltab);
#endif
    if (*pte != (pte_t)observed) {
#if defined(ARCH_HAS_PGTABLE_OPS)
        if (ltab)
            mm_pt_node_unlock(ltab);
#endif
        return 0;
    }
    pte_t replacement = arch_pte_leaf((paddr_t)pa, (pte_t)flags);
    if (replacement != (pte_t)observed) {
        *pte = replacement;
        mm_tlb_note_change(mm, va, PAGE_SIZE);
    }
#if defined(ARCH_HAS_PGTABLE_OPS)
    if (ltab)
        (void)mm_pt_refresh_leaf_prot(ltab, arch_pt_vpn(va, 0), (pte_t)flags);
    if (ltab)
        mm_pt_node_unlock(ltab);
#endif
    return 1;
}

/* A leaf that was reserved but never faulted has no PTE to carry the new
 * permissions; the per-PTE status is what a later fault installs.  The status
 * lives in the table's metadata, so the refresh takes that table's node lock
 * like every other reader and writer of the byte. */
void a20_mprotect_refresh_absent(void *mm_ptr, uintptr_t va, uintptr_t ptef)
{
#if defined(ARCH_HAS_PGTABLE_OPS)
    mm_struct_t *mm = mm_ptr;
    pte_t *ltab = mm_pt_leaf_table(mm->pgdir, va);
    if (ltab)
        mm_pt_node_lock(ltab);
    if (ltab)
        (void)mm_pt_refresh_leaf_prot(ltab, arch_pt_vpn(va, 0), (pte_t)ptef);
    if (ltab)
        mm_pt_node_unlock(ltab);
#else
    /* arm32's short-descriptor backend keeps no per-PTE status sidecar, so a
     * never-faulted reservation has no status byte to carry the new
     * permissions; the PTE a later fault installs takes them from the VMA. */
    (void)mm_ptr;
    (void)va;
    (void)ptef;
#endif
}
#else
/* NOMMU has no page tables: the transaction takes its flat branch and never
 * reaches these, but the symbols still have to exist. */
void a20_mprotect_probe(const void *mm_ptr, uintptr_t va,
                        struct a20_mprotect_leaf *out)
{
    (void)mm_ptr;
    (void)va;
    if (out)
        memset(out, 0, sizeof(*out));
}

int a20_mprotect_install(void *mm_ptr, uintptr_t va, uintptr_t observed,
                         uintptr_t pa, uintptr_t flags)
{
    (void)mm_ptr;
    (void)va;
    (void)observed;
    (void)pa;
    (void)flags;
    return 0;
}

void a20_mprotect_refresh_absent(void *mm_ptr, uintptr_t va, uintptr_t ptef)
{
    (void)mm_ptr;
    (void)va;
    (void)ptef;
}
#endif /* CONFIG_NOMMU */

/* Preserve the cache's dirty state before a shared file leaf loses the only
 * software-visible evidence of it.  Where the architecture has no dirty bit
 * the whole rule is compiled out. */
void a20_mprotect_harvest(void *mm_ptr, void *vma_ptr, uintptr_t va,
                          uintptr_t observed, int removing_write)
{
    (void)mm_ptr;
#if PTE_D == 0 || PTE_D == PTE_W
    mm_seg_t *vma = vma_ptr;
    if (!removing_write || !vma)
        return;
    if (!(vma->vm_flags & VM_WRITE))
        return;
    if ((vma->vm_flags & (VM_FILE | VM_SHARED)) != (VM_FILE | VM_SHARED))
        return;
    if (!((pte_t)observed & PTE_W))
        return;
    mm_mprotect_harvest_shared_write(vma, va);
#else
    (void)vma_ptr;
    (void)va;
    (void)observed;
    (void)removing_write;
#endif
}

uint32_t a20_mprotect_frame_refs(uint32_t pfn)
{
    if (!pfn_valid(pfn))
        return 0;
    uint64_t flags = spin_lock_irqsave(&pfa.lock);
    uint32_t refs = pfa.meta[pfn].refcount;
    spin_unlock_irqrestore(&pfa.lock, flags);
    return refs;
}

void a20_mprotect_flush_icache(uint32_t pfn)
{
    if (!pfn_valid(pfn))
        return;
    arch_flush_icache_range(pfn_to_virt(pfn), PAGE_SIZE);
}

int mm_mprotect_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                           int prot) {
    return a20_mprotect_locked_rust(mm, addr, len, prot);
}

int mm_mprotect(mm_struct_t *mm, vaddr_t addr, size_t len, int prot)
{
    if (!mm) return -EINVAL;
    int write_guard = 0;
retry_mprotect:
    if ((prot & PROT_WRITE) && !write_guard) {
        uint64_t check_flags = spin_lock_irqsave(&mm->lock);
        int needs_gate = mm_mprotect_needs_shared_write_locked(mm, addr, len,
                                                               prot);
        spin_unlock_irqrestore(&mm->lock, check_flags);
        if (needs_gate) {
            int admission = mm_shared_write_enter_wait();
            if (admission < 0)
                return admission;
            write_guard = 1;
        }
    }
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    if (!write_guard &&
        mm_mprotect_needs_shared_write_locked(mm, addr, len, prot)) {
        spin_unlock_irqrestore(&mm->lock, flags);
        mm_tlb_invalidate_finish(mm);
        int admission = mm_shared_write_enter_wait();
        if (admission < 0)
            return admission;
        write_guard = 1;
        goto retry_mprotect;
    }
    int r = mm_mprotect_locked(mm, addr, len, prot);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    if (write_guard)
        mm_shared_write_leave();
    return r;
}

/* In-frame signal trampolines need execute without replacing a live stack
 * mapping. Use the ordinary VMA/PTE protection protocol, including remote
 * TLB invalidation, rather than an unmap/remap that can free the backing page. */
int mm_make_page_exec(mm_struct_t *mm, vaddr_t addr)
{
    if (!mm) return -EINVAL;
    addr &= ~(vaddr_t)(PAGE_SIZE - 1);
    int write_guard = 0;
retry_make_exec:
    if (!write_guard) {
        uint64_t check_flags = spin_lock_irqsave(&mm->lock);
        mm_seg_t *check_vma = mm_seg_find(mm, addr);
        int needs_gate = check_vma && !(check_vma->vm_flags & VM_EXEC) &&
            (check_vma->vm_flags & (VM_FILE | VM_SHARED | VM_WRITE)) ==
                (VM_FILE | VM_SHARED | VM_WRITE);
        spin_unlock_irqrestore(&mm->lock, check_flags);
        if (needs_gate) {
            int admission = mm_shared_write_enter_wait();
            if (admission < 0)
                return admission;
            write_guard = 1;
        }
    }
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    mm_seg_t *vma = mm_seg_find(mm, addr);
    int needs_gate = vma && !(vma->vm_flags & VM_EXEC) &&
        (vma->vm_flags & (VM_FILE | VM_SHARED | VM_WRITE)) ==
            (VM_FILE | VM_SHARED | VM_WRITE);
    if (needs_gate && !write_guard) {
        spin_unlock_irqrestore(&mm->lock, flags);
        mm_tlb_invalidate_finish(mm);
        int admission = mm_shared_write_enter_wait();
        if (admission < 0)
            return admission;
        write_guard = 1;
        goto retry_make_exec;
    }
    int r = -EFAULT;
    if (vma) {
        r = 0;
        if (!(vma->vm_flags & VM_EXEC)) {
            int prot = PROT_EXEC;
            if (vma->vm_flags & VM_READ) prot |= PROT_READ;
            if (vma->vm_flags & VM_WRITE) prot |= PROT_WRITE;
            r = mm_mprotect_locked(mm, addr, PAGE_SIZE, prot);
        }
    }
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    if (write_guard)
        mm_shared_write_leave();
    return r;
}
