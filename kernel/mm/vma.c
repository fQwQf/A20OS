#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/vmo.h"
#include "mm/slab.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"

/*
 * VMA list management: sorted non-overlapping vm_area_t chain plus the backing
 * resource reference helpers used by split/merge/teardown.  Kept separate from
 * the page-table operations in mm/vm.c so the hot map/unmap/protect paths stay
 * readable.
 */

static int vma_can_merge(vm_area_t *a, vm_area_t *b)
{
    if (!a || !b || a->end != b->start)
        return 0;
    if ((a->vm_flags | b->vm_flags) & VM_SYSV_SHM)
        return 0;
    if (a->vm_flags != b->vm_flags || a->pte_flags != b->pte_flags)
        return 0;
    if ((a->vm_flags | b->vm_flags) & VM_FILE) {
        if (a->file_fd != b->file_fd)
            return 0;
        return a->file_offset + (a->end - a->start) == b->file_offset;
    }
    if ((a->vm_flags | b->vm_flags) & VM_VMO) {
        if (!(a->vm_flags & VM_VMO) || !(b->vm_flags & VM_VMO))
            return 0;
        if (a->vmo != b->vmo)
            return 0;
        return a->vmo_offset + (a->end - a->start) == b->vmo_offset;
    }
    return 1;
}

void vma_release_file(vm_area_t *vma)
{
    if (vma && (vma->vm_flags & VM_FILE) && vma->file_fd >= 0) {
        if (vma->file_vnode) {
            vnode_put(vma->file_vnode);
            vma->file_vnode = NULL;
        }
        vfs_close(vma->file_fd);
        vma->file_fd = -1;
    }
}

void vma_release_ipc(vm_area_t *vma)
{
    if (vma && (vma->vm_flags & VM_SYSV_SHM))
        sysv_shm_unref_attach(vma->sysv_shmid);
}

void vma_release(vm_area_t *vma)
{
    vma_release_file(vma);
    vma_release_ipc(vma);
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo) {
        vmo_release(vma->vmo);
        vma->vmo = NULL;
        vma->vm_flags &= ~(uint64_t)VM_VMO;
    }
}

int vma_ref_file(vm_area_t *vma)
{
    if (!vma || !(vma->vm_flags & VM_FILE) || vma->file_fd < 0)
        return 0;
    if (vma->file_vnode)
        vnode_get(vma->file_vnode);
    return vfs_ref_fd(vma->file_fd);
}

int vma_ref_fork(vm_area_t *vma)
{
    int r = vma_ref_file(vma);
    if (r < 0)
        return r;
    if (vma && (vma->vm_flags & VM_SYSV_SHM)) {
        r = sysv_shm_ref_attach(vma->sysv_shmid);
        if (r < 0) {
            vma_release_file(vma);
            return r;
        }
    }
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo)
        vmo_ref(vma->vmo);
    return 0;
}

/*
 * Retain every backing resource a copied VMA must own independently of the
 * original.  Used by the split paths (vma_split, mm_split_vma_at, munmap
 * split) after the tail VMA has been struct-copied from the head.
 */
int vma_ref_aux(vm_area_t *vma)
{
    int r = vma_ref_file(vma);
    if (r < 0)
        return r;
    if (vma && (vma->vm_flags & VM_VMO) && vma->vmo)
        vmo_ref(vma->vmo);
    return 0;
}

// 查找包含指定地址的 VMA
vm_area_t *mm_find_vma(mm_struct_t *mm, vaddr_t addr) {
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (addr < v->end && addr >= v->start) return v;
        if (v->start > addr) break;
    }
    return NULL;
}

// 在虚拟地址空间中查找一个足够大的空隙
vaddr_t mm_find_gap(mm_struct_t *mm, vaddr_t hint, size_t len) {
    vaddr_t prev_end = hint;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= prev_end && v->start - prev_end >= len) return prev_end;
        if (v->end > prev_end) prev_end = v->end;
    }
    return prev_end;
}

int mm_range_overlaps(mm_struct_t *mm, vaddr_t start, vaddr_t len,
                      vm_area_t *ignore) {
    vaddr_t end = start + len;
    if (end < start) return 1;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v == ignore) continue;
        if (v->start < end && v->end > start)
            return 1;
        if (v->start >= end) break;
    }
    return 0;
}

// Defer vma_release()/kfree() until mm->lock is dropped: release may run
// block I/O (vfs_close -> page cache writeback) which must not sleep while
// the mm lock is held.
void mm_vma_defer(mm_struct_t *mm, vm_area_t *vma)
{
    if (!mm || !vma) return;
    vma->next = mm->deferred_vma;
    mm->deferred_vma = vma;
}

void mm_vma_flush_deferred(mm_struct_t *mm)
{
    if (!mm) return;

    /*
     * Writers append to deferred_vma while holding mm->lock, but callers must
     * drop that lock before releasing the backing resources.  Detach the
     * complete list under the same lock so two threads sharing an mm cannot
     * both observe and free the same VMA chain.  The detached list is private
     * to this flusher; potentially sleeping vma_release() work remains outside
     * the spinlock.
     */
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    vm_area_t *v = mm->deferred_vma;
    mm->deferred_vma = NULL;
    spin_unlock_irqrestore(&mm->lock, flags);

    while (v) {
        vm_area_t *next = v->next;
        vma_release(v);
        kfree(v);
        v = next;
    }
}

// 插入一个 VMA 到链表中，并尝试合并相邻的相同权限区域
void mm_insert_vma(mm_struct_t *mm, vm_area_t *newv) {
    vm_area_t **pp = &mm->mmap;
    vm_area_t *prev = NULL;
    while (*pp && (*pp)->start < newv->start) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    newv->next = *pp;
    newv->prev = prev;
    if (*pp) (*pp)->prev = newv;
    *pp = newv;

    // 尝试与后一个 VMA 合并
    if (vma_can_merge(newv, newv->next)) {
        vm_area_t *nxt = newv->next;
        newv->end = nxt->end;
        newv->next = nxt->next;
        if (nxt->next) nxt->next->prev = newv;
        mm_vma_defer(mm, nxt);
    }
    // 尝试与前一个 VMA 合并
    if (vma_can_merge(newv->prev, newv)) {
        vm_area_t *prv = newv->prev;
        prv->end = newv->end;
        prv->next = newv->next;
        if (newv->next) newv->next->prev = prv;
        mm_vma_defer(mm, newv);
    }
}

int mm_split_vma_at(mm_struct_t *mm, vaddr_t addr) {
    vm_area_t *v = mm_find_vma(mm, addr);
    if (!v || addr <= v->start || addr >= v->end)
        return 0;

    vm_area_t *tail = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!tail)
        return -ENOMEM;

    *tail = *v;
    tail->start = addr;
    tail->file_offset += addr - v->start;
    int fr = vma_ref_aux(tail);
    if (fr < 0) {
        kfree(tail);
        return fr;
    }
    tail->prev = v;
    tail->next = v->next;
    if (tail->next)
        tail->next->prev = tail;

    v->end = addr;
    v->next = tail;
    return 0;
}

vm_area_t *vma_split(vm_area_t *vma, vaddr_t split) {
    if (!vma) return NULL;
    if (split <= vma->start || split >= vma->end) return vma;

    vm_area_t *tail = kcalloc_atomic(1, sizeof(vm_area_t));
    if (!tail) return NULL;

    *tail = *vma;
    tail->start = split;
    tail->file_offset += split - vma->start;
    if (vma_ref_aux(tail) < 0) {
        kfree(tail);
        return NULL;
    }
    tail->prev = vma;
    tail->next = vma->next;
    if (tail->next) tail->next->prev = tail;

    vma->end = split;
    vma->next = tail;
    return tail;
}

vm_area_t *vma_try_merge(mm_struct_t *mm, vm_area_t *vma) {
    if (!vma) return NULL;

    if (vma_can_merge(vma->prev, vma)) {
        vm_area_t *prev = vma->prev;
        prev->end = vma->end;
        prev->next = vma->next;
        if (vma->next) vma->next->prev = prev;
        mm_vma_defer(mm, vma);
        vma = prev;
    }

    if (vma_can_merge(vma, vma->next)) {
        vm_area_t *next = vma->next;
        vma->end = next->end;
        vma->next = next->next;
        if (next->next) next->next->prev = vma;
        mm_vma_defer(mm, next);
    }
    return vma;
}

// 释放 VMA 对应的物理页面
void free_vma_pages(mm_struct_t *mm, vm_area_t *vma)
{
#ifdef CONFIG_NOMMU
    (void)mm;
    if (vma->nommu_alloc) {
        kfree(vma->nommu_alloc);
        vma->nommu_alloc = NULL;
    }
    /* In NOMMU, we now track kmalloc allocations in mm->nommu_allocs.
     * We don't free vma->start directly here to avoid double-frees and freeing
     * invalid pointers if the VMA was split or modified by mprotect/munmap. */
#else
    if (!mm->pgdir) return;
    int shared_file = (vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED);
    for (uint64_t va = vma->start; va < vma->end; ) {
        /* 检查当前映射是否为跨越 VMA 边界的大页，
         * 若是则先降级为普通页再逐个释放。 */
        int level = 0;
        vaddr_t base = 0;
        size_t size = 0;
        pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
        if (pte && (*pte & PTE_V) && level > 0 &&
            (base < vma->start || base + size > vma->end)) {
            mm_demote_huge_page(mm, va);
            continue;
        }

        if (pte && (*pte & PTE_V) && shared_file && vma->file_vnode &&
            (*pte & PTE_D)) {
            uint64_t idx = vma->file_offset + (va - vma->start);
            idx /= PAGE_SIZE;
            page_cache_page_t *pcp = page_cache_get(vma->file_vnode, idx, 0);
            if (pcp) {
                page_cache_mark_dirty(pcp);
                page_cache_put(pcp);
            }
        }

        paddr_t pa = 0;
        base = 0;
        size = 0;
        if (pt_unmap_leaf(mm->pgdir, va, &pa, &base, &size, NULL) == 0) {
            if (pa) {
                pfn_t pfn = phys_to_pfn(pa);
                if (shared_file && vma->file_vnode) {
                    uint64_t idx = vma->file_offset + (va - vma->start);
                    idx /= PAGE_SIZE;
                    page_cache_page_t *pcp = page_cache_get(vma->file_vnode, idx, 0);
                    if (pcp) {
                        page_cache_put(pcp);
                        page_cache_put(pcp);
                    }
                } else if (!(vma->vm_flags & VM_PFNMAP)) {
                    frame_put(pfn);
                }
            }
            va = base + size;
        } else {
            va += PAGE_SIZE;
        }
    }
#endif
}
