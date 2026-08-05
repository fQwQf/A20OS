#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/fault.h"
#include "mm/swap.h"
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

int mm_mprotect_locked(mm_struct_t *mm, vaddr_t addr, size_t len,
                           int prot) {
    if (!mm || !mm->pgdir) return -EINVAL;
    if (prot & ~(PROT_READ | PROT_WRITE | PROT_EXEC)) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return 0;

    pte_t ptef = mm_prot_to_pte_flags(prot);
    uint64_t vm_prot = 0;
    if (prot & 1) vm_prot |= VM_READ;
    if (prot & 2) vm_prot |= VM_WRITE;
    if (prot & 4) vm_prot |= VM_EXEC;
    vaddr_t end = addr + len;
    if (end < addr || end > USER_VA_LIMIT) return -ENOMEM;
#ifndef CONFIG_NOMMU
    int touched = 0;
#endif

    vaddr_t covered = addr;
    for (vm_area_t *v = mm_find_vma(mm, addr); v && covered < end; v = v->next) {
        if (v->start > covered)
            break;
        if (v->end > covered)
            covered = v->end;
    }
    if (covered < end)
        return -ENOMEM;

#ifdef CONFIG_NOMMU
    /* NOMMU has no page tables. We only update the VMA permission bits without splitting. */
    for (vm_area_t *v = mm_find_vma(mm, addr); v && v->start < end; ) {
        vm_area_t *next = v->next;
        v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef);
        v->vm_flags  = (v->vm_flags & ~(uint64_t)(VM_READ | VM_WRITE | VM_EXEC)) |
                       vm_prot;
        v = next;
    }
    return 0;
#else
    int     r = mm_split_vma_at(mm, addr);
    if (r < 0) return r;
    r = mm_split_vma_at(mm, end);
    if (r < 0) return r;


    for (vm_area_t *v = mm_find_vma(mm, addr); v && v->start < end; ) {
        vm_area_t *next = v->next;
        uint64_t s = v->start < addr ? addr : v->start;
        uint64_t e = v->end > end ? end : v->end;

        if (s > v->start) {
            v = vma_split(v, s);
            if (!v) return -ENOMEM;
            next = v->next;
        }
        if (e < v->end) {
            if (!vma_split(v, e)) return -ENOMEM;
            next = v->next;
        }

        for (uint64_t va = v->start; va < v->end; ) {
            int level = 0;
            vaddr_t base = 0;
            size_t size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (pte && (*pte & PTE_V)) {
                if (level > 0 && (base < v->start || base + size > v->end)) {
                    int dr = mm_demote_huge_page(mm, va);
                    if (dr < 0) return dr;
                    continue;
                }
                uint64_t old_flags = arch_pte_flags(*pte);
                uint64_t flags = mm_pte_flags_apply_prot(*pte, ptef);
                if ((flags & PTE_X) && !(old_flags & PTE_X)) {
                    paddr_t pa = arch_pte_addr(*pte);
                    pfn_t pfn = phys_to_pfn(pa);
                    if (pfn_valid(pfn))
                        arch_flush_icache_range(pfn_to_virt(pfn), PAGE_SIZE);
                }
                *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
                va = base + size;
            } else {
                va += PAGE_SIZE;
            }
        }
        v->pte_flags = mm_pte_flags_apply_prot(v->pte_flags, ptef);
        v->vm_flags  = (v->vm_flags & ~(uint64_t)(VM_READ | VM_WRITE | VM_EXEC)) |
                       vm_prot;
        v = vma_try_merge(mm, v);
        touched = 1;
        v = v ? v->next : next;
    }

    (void)touched;
#endif
    return 0;
}

int mm_mprotect(mm_struct_t *mm, vaddr_t addr, size_t len, int prot)
{
    if (!mm) return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int r = mm_mprotect_locked(mm, addr, len, prot);
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_vma_flush_deferred(mm);
    arch_tlb_flush();  // deferred remote flush
    return r;
}
