/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * This file is part of the mechanically split Native Phase 2 ABI.
 * See sys_phase2.c for shared helpers and forward declarations.
 */
#include "core/types.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/version.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/random.h"
#include "trap_frame.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "fs/xattr.h"
#include "net/socket.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern struct a20_ht_internal *task_get_a20_ht_ref(task_t *t);
extern void a20_ht_put_ref(struct a20_ht_internal *ht);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);
extern int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                           uint16_t type, a20_rights_t rights,
                                           uint64_t expiry_tick, uint32_t remaining_ops,
                                           uint32_t temporal_flags, uint8_t security_label);
extern int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                           uint16_t expected_type, a20_rights_t required_rights,
                                           a20_handle_entry_t *out);
extern int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                               a20_handle_t h,
                                               uint16_t expected_type,
                                               a20_rights_t required_rights,
                                               a20_handle_entry_t *out);
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern void a20_object_ref(void *object, uint16_t type);
extern void a20_object_release(void *object, uint16_t type);

extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);

static int a20_mm_user_range_ok(uint64_t va, size_t n)
{
    va = (uint64_t)(vaddr_t)va;
    if (n == 0) return 1;
    if (va >= USER_VA_LIMIT) return 0;
    return n <= USER_VA_LIMIT - va;
}

/* ===== Memory (0x0300) continued ===== */

int64_t sys_a20_vm_protect(const a20_syscall_args_t *args)
{
    uint64_t addr = A20_ARG(0);
    uint64_t len = A20_ARG(1);
    uint32_t prot = (uint32_t)A20_ARG(2);
    if (len == 0) return -A20_ERR_INVALID_ARGUMENT;
    return a20_vmar_protect(addr, len, prot);
}

/*
 * prot_eff — requested protection intersected with the handle's rights
 * (docs/native-abi/04-memory.md §4.2: prot_eff = prot_req ∩ prot_handle).
 */
static uint32_t a20_vm_prot_eff(uint32_t prot, a20_rights_t rights)
{
    uint32_t eff = 0;
    if ((prot & A20_PROT_READ) && (rights & A20_RIGHT_READ))
        eff |= A20_PROT_READ;
    if ((prot & A20_PROT_WRITE) && (rights & A20_RIGHT_WRITE))
        eff |= A20_PROT_WRITE;
    /* No EXEC right exists for MEMORY/FILE handles; EXEC passes through. */
    if (prot & A20_PROT_EXEC)
        eff |= A20_PROT_EXEC;
    return eff;
}

int64_t sys_a20_vm_map(const a20_syscall_args_t *args)
{
    a20_vm_map_args_t *uargs = (a20_vm_map_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_vm_map_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.length == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;

    uint64_t addr = 0;
    int64_t r = A20_OK;

    if (kargs.source != A20_HANDLE_NULL) {
        struct a20_ht_internal *ht = task_get_a20_ht(cur);
        if (!ht) return -A20_ERR_BAD_HANDLE;

        a20_handle_entry_t src;
        r = a20_handle_lookup_ref_internal(ht, kargs.source, A20_OBJ_INVALID,
                                           A20_RIGHT_MAP, &src);
        if (r < 0) return r;

        if (src.type == A20_OBJ_MEMORY) {
            /* Map an existing VMO: shared canonical frames, prot intersected
             * with the handle's rights.  The VMA takes its own VMO reference. */
            struct vmo *vmo = (struct vmo *)src.object;
            uint32_t prot_eff = a20_vm_prot_eff(kargs.prot, src.rights);
            if (kargs.offset & (PAGE_SIZE - 1)) {
                r = -A20_ERR_INVALID_ARGUMENT;
            } else if (kargs.offset >= vmo->size ||
                       kargs.length > vmo->size - kargs.offset) {
                r = -A20_ERR_INVALID_ARGUMENT;
            } else {
                r = a20_vmar_map(vmo, kargs.offset, kargs.length, prot_eff,
                                 kargs.flags, kargs.addr_hint, &addr);
            }
            a20_object_release(src.object, src.type);
            if (r < 0) return r;
        } else if (src.type == A20_OBJ_FILE || src.type == A20_OBJ_DEVICE) {
            if (!(src.rights & A20_RIGHT_READ)) {
                a20_object_release(src.object, src.type);
                return -A20_ERR_ACCESS;
            }
            uint32_t prot_eff = a20_vm_prot_eff(kargs.prot, src.rights);
            int gfd = (int)(uintptr_t)src.object;
            /* Demand-paged file mapping through the core page cache.  The VMA
             * holds its own fd reference; no eager anonymous VMO is created. */
            if (kargs.offset & (PAGE_SIZE - 1)) {
                a20_object_release(src.object, src.type);
                return -A20_ERR_INVALID_ARGUMENT;
            }
            addr = mm_mmap_file(cur->mm, kargs.addr_hint, kargs.length,
                                a20_prot_to_mmap(prot_eff), MAP_PRIVATE,
                                gfd, kargs.offset);
            a20_object_release(src.object, src.type);
            if ((int64_t)addr < 0)
                return -A20_ERR_NO_MEMORY;
        } else {
            a20_object_release(src.object, src.type);
            return -A20_ERR_INVALID_ARGUMENT;
        }
    } else {
        struct vmo *vmo = vmo_create(VMO_ANONYMOUS, kargs.length, 0);
        if (!vmo) return -A20_ERR_NO_MEMORY;
        r = a20_vmar_map(vmo, 0, kargs.length, kargs.prot, kargs.flags,
                         kargs.addr_hint, &addr);
        vmo_release(vmo);
        if (r < 0) return r;
    }

    kargs.out_addr = addr;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_vmar_unmap(addr, kargs.length);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

int64_t sys_a20_vm_share(const a20_syscall_args_t *args)
{
    a20_handle_t vmo_h = (a20_handle_t)A20_ARG(0);
    a20_handle_t target_h = (a20_handle_t)A20_ARG(1);
    a20_rights_t rights = (a20_rights_t)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t vmo_entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, vmo_h, A20_OBJ_MEMORY,
                                               A20_RIGHT_READ | A20_RIGHT_TRANSFER,
                                               &vmo_entry);
    if (r < 0) return r;

    task_t *target_task = NULL;
    struct a20_ht_internal *target_ht = ht;
    if (target_h != A20_HANDLE_NULL) {
        a20_handle_entry_t tgt_entry;
        r = a20_handle_lookup_internal(ht, target_h, A20_OBJ_TASK,
                                       A20_RIGHT_CONTROL, &tgt_entry);
        if (r < 0) goto out_vmo;

        target_task = proc_find_get((int)(uintptr_t)tgt_entry.object);
        if (!target_task) {
            r = -A20_ERR_BAD_HANDLE;
            goto out_vmo;
        }
        target_ht = task_get_a20_ht_ref(target_task);
        if (!target_ht) {
            proc_put(target_task);
            r = -A20_ERR_BAD_HANDLE;
            goto out_vmo;
        }
    }

    /* Bell-LaPadula No Read Up for either the current or target process. */
    if (a20_ht_get_label(target_ht) < vmo_entry.security_label) {
        r = -A20_ERR_ACCESS;
        goto out_target;
    }

    a20_rights_t child_rights = rights & vmo_entry.rights;
    if (child_rights == 0) {
        r = -A20_ERR_ACCESS;
        goto out_target;
    }

    int64_t new_h = a20_handle_install_temporal(target_ht, vmo_entry.object,
                                                vmo_entry.type, child_rights,
                                                vmo_entry.expiry_tick,
                                                vmo_entry.remaining_ops,
                                                vmo_entry.temporal_flags,
                                                vmo_entry.security_label);
    if (new_h < 0) {
        r = new_h;
        goto out_target;
    }
    a20_object_ref(vmo_entry.object, vmo_entry.type);
    r = new_h;

out_target:
    if (target_task) {
        a20_ht_put_ref(target_ht);
        proc_put(target_task);
    }
out_vmo:
    a20_object_release(vmo_entry.object, vmo_entry.type);
    return r;
}


int64_t sys_a20_vm_flush(const a20_syscall_args_t *args)
{
    uint64_t addr = A20_ARG(0);
    uint64_t len  = A20_ARG(1);
    uint32_t flags = (uint32_t)A20_ARG(2);

    if (len == 0) return A20_OK;
    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;

    uint64_t end = (addr + len + 4095) & ~(uint64_t)4095;
    for (uint64_t va = addr & ~(uint64_t)4095; va < end; va += 4096) {
        vm_area_t *vma = mm_find_vma(cur->mm, va);
        if (!vma || va >= vma->end) return -A20_ERR_NO_MEMORY;
    }

    if (flags & A20_FLUSH_SYNC)
        return vfs_sync();
    if (flags & A20_FLUSH_INVALIDATE)
        arch_tlb_flush();
    return A20_OK;
}

int64_t sys_a20_vm_advise(const a20_syscall_args_t *args)
{
    uint64_t addr = A20_ARG(0);
    uint64_t len = A20_ARG(1);
    uint32_t advice = (uint32_t)A20_ARG(2);

    if (len == 0) return A20_OK;
    if (addr & 4095) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;

    uint64_t end = (addr + len + 4095) & ~(uint64_t)4095;
    for (uint64_t va = addr; va < end; va += 4096) {
        vm_area_t *vma = mm_find_vma(cur->mm, va);
        if (!vma || va >= vma->end) return -A20_ERR_NO_MEMORY;
    }

#ifndef CONFIG_NOMMU
    if (advice == 4 /* MADV_DONTNEED */ || advice == 8 /* MADV_FREE */) {
        for (uint64_t va = addr; va < end; ) {
            int level = 0;
            vaddr_t base = 0;
            size_t leaf_size = 0;
            pte_t *pte = pt_lookup_leaf(cur->mm->pgdir, va, &level, &base, &leaf_size);
            if (!pte || !(*pte & PTE_V)) { va += 4096; continue; }
            vm_area_t *vma = mm_find_vma(cur->mm, va);
            if (vma && (vma->vm_flags & VM_VMO)) {
                /* VMO frames are owned by the VMO; unmapping a PTE must not
                 * frame_put() them.  Drop the PTE and let the VMO keep the
                 * canonical frame. */
                paddr_t dummy = 0;
                pt_unmap_leaf(cur->mm->pgdir, va, &dummy, &base, &leaf_size, NULL);
                cur->mm->rss = (cur->mm->rss > leaf_size / 4096)
                                   ? cur->mm->rss - leaf_size / 4096 : 0;
                va = base + leaf_size;
                continue;
            }
            paddr_t pa = 0;
            if (pt_unmap_leaf(cur->mm->pgdir, va, &pa, &base, &leaf_size, NULL) == 0) {
                if (pa) {
                    frame_put(phys_to_pfn(pa));
                    size_t pages = leaf_size / 4096;
                    cur->mm->rss = (cur->mm->rss > pages) ? cur->mm->rss - pages : 0;
                }
                va = base + leaf_size;
            } else {
                va += 4096;
            }
        }
        arch_tlb_flush();
    }
#endif
    return A20_OK;
}

int64_t sys_a20_vm_remap(const a20_syscall_args_t *args)
{
    uint64_t old_addr = A20_ARG(0);
    uint64_t old_len = A20_ARG(1);
    uint64_t new_len = A20_ARG(2);
    uint32_t prot = (uint32_t)A20_ARG(3);
    uint64_t new_addr_hint = A20_ARG(4);

    if (old_len == 0 && new_len == 0) return -A20_ERR_INVALID_ARGUMENT;
    if (new_len > (256ULL << 20) || old_len > (256ULL << 20))
        return -A20_ERR_INVALID_ARGUMENT;

    if (new_len <= old_len) {
        if (!a20_mm_user_range_ok(old_addr, (size_t)old_len))
            return -A20_ERR_FAULT;
        if (new_len < old_len)
            proc_munmap(old_addr + new_len, (size_t)(old_len - new_len));
        return (int64_t)old_addr;
    }

    if (!a20_mm_user_range_ok(old_addr, (size_t)old_len))
        return -A20_ERR_FAULT;

    uint64_t new_addr = proc_mmap(new_addr_hint, (size_t)new_len,
                                   (int)prot ? (int)prot : 3,
                                   0x20 /* MAP_ANONYMOUS */, -1, 0);
    if (new_addr == 0) return -A20_ERR_NO_MEMORY;

    char kbuf[512];
    uint64_t done = 0;
    while (done < old_len) {
        size_t chunk = old_len - done;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        if (copy_from_user(kbuf, (const void *)(old_addr + done), chunk) < 0 ||
            copy_to_user((void *)(new_addr + done), kbuf, chunk) < 0) {
            proc_munmap(new_addr, (size_t)new_len);
            return -A20_ERR_FAULT;
        }
        done += chunk;
    }
    if (old_len > 0)
        proc_munmap(old_addr, (size_t)old_len);

    return (int64_t)new_addr;
}


int64_t sys_a20_vm_lock(const a20_syscall_args_t *args)
{
    uint64_t addr = A20_ARG(0);
    uint64_t len = A20_ARG(1);
    uint32_t flags = (uint32_t)A20_ARG(2);

    if (len == 0) return A20_OK;
    uint64_t start = addr & ~(uint64_t)4095;
    uint64_t end = (addr + len + 4095) & ~(uint64_t)4095;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;

    for (uint64_t va = start; va < end; va += 4096) {
        vm_area_t *vma = mm_find_vma(cur->mm, va);
        if (!vma) return -A20_ERR_NO_MEMORY;
        if (flags & 0x01)
            vma->vm_flags |= 0x08000000;
        else
            vma->vm_flags &= ~(uint64_t)0x08000000;
    }
    return A20_OK;
}

int64_t sys_a20_vm_create_object(const a20_syscall_args_t *args)
{
    uint64_t size = A20_ARG(0);
    uint32_t options = (uint32_t)A20_ARG(1);

    if (size == 0) return -A20_ERR_INVALID_ARGUMENT;

    struct vmo *vmo = vmo_create(VMO_ANONYMOUS, size, options);
    if (!vmo) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vmo_release(vmo); return -A20_ERR_BAD_HANDLE; }

    int64_t h = a20_handle_install(ht, vmo, A20_OBJ_MEMORY,
                                    A20_RIGHT_READ | A20_RIGHT_WRITE |
                                    A20_RIGHT_MAP | A20_RIGHT_STAT | A20_RIGHT_DUP |
                                    A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL);
    if (h < 0) vmo_release(vmo);
    return h;
}
