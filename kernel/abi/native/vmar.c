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
        vm_area_t *v = mm_find_vma(cur->mm, hint);
        if (!v || (hint + length <= v->start))
            return hint;
    }

    vaddr_t addr = mm_find_gap(cur->mm, MMAP_BASE_ADDR, length);
    if (addr == 0 || addr + length < addr || addr + length > USER_VA_LIMIT)
        return 0;
    return addr;
}

int64_t a20_vmar_map(struct vmo *vmo, uint64_t vmo_offset, uint64_t length,
                     uint32_t prot, uint32_t flags, uint64_t hint, uint64_t *out_addr)
{
    (void)flags;
    if (!out_addr || length == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;

    uint64_t addr = mm_mmap_vmo(cur->mm, hint, length, a20_prot_to_mmap(prot),
                                MAP_ANONYMOUS | MAP_PRIVATE, vmo, vmo_offset);
    if ((int64_t)addr < 0) {
        int err = (int)(int64_t)addr;
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
    if (length == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;

    return mm_mprotect(cur->mm, addr, length, a20_prot_to_mmap(new_prot));
}
