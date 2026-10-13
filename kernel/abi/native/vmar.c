/*
 * A20OS Native ABI — VMAR (Virtual Memory Address Region) wrapper.
 *
 * VMAR operations are thin wrappers over the core MM layer: mapping is
 * mm_mmap_vmo() (a VM_VMO VMA backed by the VMO's canonical frames),
 * unmapping is mm_munmap(), and protection changes are mm_mprotect().  The
 * Native ABI adds handle/rights semantics on top (prot_eff computed by the
 * syscall layer); it does not reimplement memory management.
 */
#include "core/types.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/vmo.h"
#include "core/arch.h"
#include "core/mman.h"
#include "abi/native/errno.h"
#include "abi/native/types.h"
#include "abi/native/vmar.h"

/* Convert Native A20_PROT_* to the core Linux-style PROT_* bits. */
int a20_prot_to_mmap(uint32_t prot)
{
    int mmap_prot = 0;
    if (prot & A20_PROT_READ)  mmap_prot |= PROT_READ;
    if (prot & A20_PROT_WRITE) mmap_prot |= PROT_WRITE;
    if (prot & A20_PROT_EXEC)  mmap_prot |= PROT_EXEC;
    return mmap_prot;
}

uint64_t a20_vmar_find_free(uint64_t hint, uint64_t length)
{
    task_t *cur = proc_current();
    if (!cur || !cur->mm) return 0;
    if (length == 0) return 0;

    if (hint != 0 && hint < USER_VA_LIMIT) {
        mm_seg_t *v = mm_seg_find(cur->mm, hint);
        if (!v || (hint + length <= v->start))
            return hint;
    }

    vaddr_t addr = mm_find_gap(cur->mm, cur->mm->mmap_base ? cur->mm->mmap_base
                                                           : MMAP_BASE_ADDR, length);
    if (addr == 0 || addr + length < addr || addr + length > USER_VA_LIMIT)
        return 0;
    return addr;
}

int64_t a20_vmar_map(struct vmo *vmo, uint64_t vmo_offset, uint64_t length,
                     uint32_t prot, uint32_t flags, uint64_t hint, uint64_t *out_addr)
{
    if (!out_addr || !length || length > SIZE_MAX - (PAGE_SIZE - 1) ||
        (prot & ~7U) || (flags & ~(MAP_FIXED | MAP_FIXED_NOREPLACE)))
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;

    mm_tlb_invalidate_begin(cur->mm);
    uint64_t lock_flags = spin_lock_irqsave(&cur->mm->lock);
    uint64_t addr = mm_mmap_vmo_cap_locked(cur->mm, hint, length, a20_prot_to_mmap(prot),
                                MAP_ANONYMOUS | MAP_SHARED |
                                (flags & (MAP_FIXED | MAP_FIXED_NOREPLACE)),
                                vmo, vmo_offset, prot, NULL);
    spin_unlock_irqrestore(&cur->mm->lock, lock_flags);
    mm_tlb_invalidate_finish(cur->mm);
    if (mm_addr_is_error((vaddr_t)addr)) {
        int err = mm_addr_error((vaddr_t)addr);
        if (err == -EINVAL)
            return -A20_ERR_INVALID_ARGUMENT;
        if (err == -EEXIST)
            return -A20_ERR_ACCESS;
        return -A20_ERR_NO_MEMORY;
    }

    *out_addr = addr;
    return A20_OK;
}

int64_t a20_vmar_unmap(uint64_t addr, uint64_t length)
{
    if (length == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;

    return mm_munmap(cur->mm, addr, length);
}

int64_t a20_vmar_protect(uint64_t addr, uint64_t length, uint32_t new_prot)
{
    if (length == 0 || length > SIZE_MAX - (PAGE_SIZE - 1) ||
        addr >= USER_VA_LIMIT || length > USER_VA_LIMIT - addr ||
        (new_prot & ~(A20_PROT_READ | A20_PROT_WRITE | A20_PROT_EXEC)))
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;

    /* Core checks the ceiling under mm->lock with the permission update. */
    int r = mm_mprotect(cur->mm, addr, length, a20_prot_to_mmap(new_prot));
    if (r == -EACCES || r == -EPERM) return -A20_ERR_ACCESS;
    if (r == -EINVAL) return -A20_ERR_INVALID_ARGUMENT;
    if (r < 0) return -A20_ERR_NO_MEMORY;
    return A20_OK;
}
