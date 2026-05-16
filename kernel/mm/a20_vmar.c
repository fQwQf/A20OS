/*
 * A20OS Native ABI — VMAR implementation.
 * Design reference: docs/native-abi/04-memory.md §3
 *
 * Phase 1: VMAR ops delegate to existing proc_mmap/proc_munmap.
 * Phase 2: full VMO-backed VMAR with independent page table management.
 */
#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/frame.h"
#include "core/arch.h"
#include "abi/current.h"
#include "abi/native/vmar.h"
#include "abi/native/vmo.h"
#include "abi/native/errno.h"
#include "abi/native/types.h"

uint64_t a20_vmar_find_free(uint64_t hint, uint64_t length)
{
    task_t *cur = proc_current();
    if (!cur || !cur->mm) return 0;

    if (hint != 0) {
        vm_area_t *v = mm_find_vma(cur->mm, hint);
        if (!v || (hint + length <= v->start))
            return hint;
    }

    return proc_mmap(0, (size_t)length, PROT_READ,
                     MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
}

int64_t a20_vmar_map(struct a20_vmo *vmo, uint64_t vmo_offset, uint64_t length,
                     uint32_t prot, uint32_t flags, uint64_t hint, uint64_t *out_addr)
{
    if (!out_addr || length == 0) return -A20_ERR_INVALID_ARGUMENT;

    int mmap_prot = 0;
    if (prot & A20_PROT_READ)    mmap_prot |= PROT_READ;
    if (prot & A20_PROT_WRITE)   mmap_prot |= PROT_WRITE;
    if (prot & A20_PROT_EXEC)   mmap_prot |= PROT_EXEC;

    uint64_t addr = proc_mmap(hint, (size_t)length, mmap_prot,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (addr == 0) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    if (vmo && cur && cur->mm && cur->mm->pgdir) {
        a20_vmo_ref(vmo);
        vm_area_t *vma = mm_find_vma(cur->mm, addr);
        if (vma && addr >= vma->start && addr < vma->end) {
            vma->vm_flags |= VM_VMO;
            vma->vmo = vmo;
            vma->vmo_offset = vmo_offset;
        }

        uint64_t pte_flags = PTE_V | PTE_U;
        if (prot & A20_PROT_READ)  pte_flags |= PTE_R;
        if (prot & A20_PROT_WRITE) pte_flags |= PTE_W;
        if (prot & A20_PROT_EXEC)  pte_flags |= PTE_X;

        uint32_t start_idx = (uint32_t)(vmo_offset / PAGE_SIZE);
        uint64_t npages = (length + PAGE_SIZE - 1) / PAGE_SIZE;
        for (uint64_t i = 0; i < npages; i++) {
            pfn_t pfn = a20_vmo_get_page(vmo, start_idx + (uint32_t)i);
            if (pfn == PFN_NONE) break;
            paddr_t pa = pfn_to_phys(pfn);
            pt_map(cur->mm->pgdir, addr + i * PAGE_SIZE, pa, pte_flags);
        }
        arch_tlb_flush();
    }

    *out_addr = addr;
    return A20_OK;
}

int64_t a20_vmar_unmap(uint64_t addr, uint64_t length)
{
    if (length == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (cur && cur->mm) {
        vm_area_t *vma = mm_find_vma(cur->mm, addr);
        if (vma && (vma->vm_flags & VM_VMO) && vma->vmo) {
            a20_vmo_release(vma->vmo);
            vma->vmo = NULL;
            vma->vm_flags &= ~VM_VMO;
        }
    }

    return proc_munmap(addr, (size_t)length);
}

int64_t a20_vmar_protect(uint64_t addr, uint64_t length, uint32_t new_prot)
{
    if (length == 0) return -A20_ERR_INVALID_ARGUMENT;

    int mmap_prot = 0;
    if (new_prot & A20_PROT_READ)    mmap_prot |= PROT_READ;
    if (new_prot & A20_PROT_WRITE)   mmap_prot |= PROT_WRITE;
    if (new_prot & A20_PROT_EXEC)    mmap_prot |= PROT_EXEC;

    return proc_mmap(addr, (size_t)length, mmap_prot,
                     MAP_FIXED | MAP_PRIVATE, -1, 0) == addr
           ? A20_OK : -A20_ERR_INVALID_ARGUMENT;
}
