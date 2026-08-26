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
#include "core/mman.h"
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
#include "mm/vmar.h"
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
    /* EXEC is tightened against the handle's EXEC right (04-memory §4.4):
     * no handle right, no execute mapping. */
    if ((prot & A20_PROT_EXEC) && (rights & A20_RIGHT_EXEC))
        eff |= A20_PROT_EXEC;
    return eff;
}

int64_t sys_a20_vm_map(const a20_syscall_args_t *args)
{
    a20_vm_map_args_t *uargs = (a20_vm_map_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_vm_map_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.length == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;

    /* Map through a VMAR reservation when one is supplied (04-memory §3):
     * the range must land inside the node and prot must fit its ceiling. */
    vmar_t *route = NULL;
    if (kargs.vmar != A20_HANDLE_NULL) {
        struct a20_ht_internal *ht0 = task_get_a20_ht(cur);
        if (!ht0) return -A20_ERR_BAD_HANDLE;
        a20_handle_entry_t ve;
        int64_t vr = a20_handle_lookup_ref_internal(
            ht0, kargs.vmar, A20_OBJ_VMAR, A20_RIGHT_MAP, &ve);
        if (vr < 0) return vr;
        route = (vmar_t *)ve.object; /* ref held until after install */
    }

    /* Ceiling checks for maps routed through a VMAR (04-memory §3):
     * requested prot bits must fit the node's ceiling and fixed-address
     * placement needs VMAR_CAN_MAP_SPECIFIC.  After success the ceiling is
     * stamped into the VMA's vmar_cap so later protect() stays within what
     * authorized the mapping. */
    uint32_t route_can = 0;
    if (route) {
        if (kargs.prot & A20_PROT_READ)  route_can |= VMAR_CAN_MAP_READ;
        if (kargs.prot & A20_PROT_WRITE) route_can |= VMAR_CAN_MAP_WRITE;
        if (kargs.prot & A20_PROT_EXEC)  route_can |= VMAR_CAN_MAP_EXEC;
        if ((kargs.flags & MAP_FIXED) &&
            !(route->cap & VMAR_CAN_MAP_SPECIFIC)) {
            a20_object_release(route, A20_OBJ_VMAR);
            return -A20_ERR_ACCESS;
        }
        if (!vmar_cap_allows(route, route_can)) {
            a20_object_release(route, A20_OBJ_VMAR);
            return -A20_ERR_ACCESS;
        }
    }

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
            if (mm_addr_is_error((vaddr_t)addr))
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

    if (route && !vmar_contains(route, addr, kargs.length)) {
        a20_vmar_unmap(addr, kargs.length);
        a20_object_release(route, A20_OBJ_VMAR);
        return -A20_ERR_NO_SPACE;
    }
    if (route) {
        vm_area_t *nv = mm_find_vma(cur->mm, (vaddr_t)addr);
        if (nv && nv->start == (vaddr_t)addr) {
            uint32_t cap_bits = 0;
            if (route_can & VMAR_CAN_MAP_READ)  cap_bits |= A20_PROT_READ;
            if (route_can & VMAR_CAN_MAP_WRITE) cap_bits |= A20_PROT_WRITE;
            if (route_can & VMAR_CAN_MAP_EXEC)  cap_bits |= A20_PROT_EXEC;
            nv->vmar_cap = nv->vmar_cap ? (nv->vmar_cap & cap_bits) : cap_bits;
        }
        a20_object_release(route, A20_OBJ_VMAR);
    }

    kargs.out_addr = addr;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_vmar_unmap(addr, kargs.length);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
}

/* vm_create_vmar — sub-allocate an address-range reservation under a
 * parent (or create a root when parent is NULL).  Ceilings narrow
 * monotonically down the tree; ranges must be page-aligned, inside the
 * parent, and disjoint from siblings (docs/native-abi/04-memory.md §3). */
int64_t sys_a20_vm_create_vmar(const a20_syscall_args_t *args)
{
    a20_vm_create_vmar_args_t *uargs =
        (a20_vm_create_vmar_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_vm_create_vmar_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    const uint64_t known = A20_VMAR_CAN_MAP_READ | A20_VMAR_CAN_MAP_WRITE |
                           A20_VMAR_CAN_MAP_EXEC | A20_VMAR_CAN_MAP_SPECIFIC;
    if (kargs.flags & ~known) return -A20_ERR_INVALID_ARGUMENT;
    if (kargs.length == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    vmar_t *parent = NULL;
    if (kargs.parent != A20_HANDLE_NULL) {
        a20_handle_entry_t pe;
        int64_t pr = a20_handle_lookup_ref_internal(
            ht, kargs.parent, A20_OBJ_VMAR, A20_RIGHT_MAP, &pe);
        if (pr < 0) return pr;
        parent = (vmar_t *)pe.object;
    }

    vmar_t *v = NULL;
    int64_t r;
    if (parent) {
        r = vmar_create_child(parent, kargs.base, kargs.length,
                              (uint32_t)kargs.flags, &v);
        a20_object_release(parent, A20_OBJ_VMAR);
    } else {
        v = vmar_create_root(cur->mm, kargs.base, kargs.length,
                             (uint32_t)kargs.flags);
        r = v ? A20_OK : -A20_ERR_NO_MEMORY;
    }
    if (r < 0) return r;

    int64_t h = a20_handle_install(ht, v, A20_OBJ_VMAR,
                                   A20_RIGHT_MAP | A20_RIGHT_STAT |
                                   A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                   A20_RIGHT_CONTROL);
    if (h < 0) { vmar_release(v); return h; }

    kargs.out_vmar = (a20_handle_t)h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)h); /* drops our reference */
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
        r = a20_handle_lookup_task_like(ht, target_h,
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

/*
 * vm_share_region — export an address range of the caller's own address space
 * as a new MEMORY handle (docs/native-abi/09-… §7).  The range must be fully
 * covered by a single VM_VMO VMA (a VMO-backed mapping created by vm_map /
 * vm_alloc); the exported handle references the same canonical VMO frames.
 */
int64_t sys_a20_vm_share_region(const a20_syscall_args_t *args)
{
    a20_vm_share_args_t *uargs = (a20_vm_share_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_vm_share_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (kargs.length == 0 || (kargs.addr & (PAGE_SIZE - 1)) ||
        (kargs.length & (PAGE_SIZE - 1)))
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;
    if (kargs.addr >= USER_VA_LIMIT ||
        kargs.length > USER_VA_LIMIT - kargs.addr)
        return -A20_ERR_INVALID_ARGUMENT;

    uint32_t prot = 0;
    struct vmo *vmo = mm_lookup_vmo_region(cur->mm, (vaddr_t)kargs.addr,
                                           (size_t)kargs.length, &prot);
    if (!vmo)
        return -A20_ERR_NOT_SUPPORTED;

    a20_rights_t rights = 0;
    if (prot & A20_PROT_READ)  rights |= A20_RIGHT_READ;
    if (prot & A20_PROT_WRITE) rights |= A20_RIGHT_WRITE;
    if (prot & A20_PROT_EXEC)  rights |= A20_RIGHT_EXEC;
    rights |= A20_RIGHT_MAP | A20_RIGHT_STAT | A20_RIGHT_DUP |
              A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    rights &= kargs.rights;
    if (rights == 0) {
        vmo_release(vmo);
        return -A20_ERR_ACCESS;
    }

    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vmo_release(vmo); return -A20_ERR_BAD_HANDLE; }
    int64_t h = a20_handle_install(ht, vmo, A20_OBJ_MEMORY, rights);
    if (h < 0) { vmo_release(vmo); return h; }

    kargs.out_handle = (a20_handle_t)h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)h);
        return -A20_ERR_FAULT;
    }
    return A20_OK;
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
    if (flags & A20_FLUSH_CLEAN) {
        /* Range writeback for file-backed mappings; VMO frames are owned by
         * the VMO and have no per-range writeback path, so a full cache sync
         * covers the file range (docs/native-abi/09-… §7). */
        return vfs_sync();
    }
    if (flags & A20_FLUSH_INVALIDATE)
        arch_tlb_flush();
    return A20_OK;
}

int64_t sys_a20_vm_advise(const a20_syscall_args_t *args)
{
    uint64_t addr = A20_ARG(0);
    uint64_t len = A20_ARG(1);

    if (len == 0) return A20_OK;
    if (addr & 4095) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;

    int r = mm_madvise_dontneed(cur->mm, (vaddr_t)addr, (size_t)len);
    if (r == -ENOMEM) return -A20_ERR_NO_MEMORY;
    if (r < 0) return -A20_ERR_INVALID_ARGUMENT;
    return A20_OK;
}

/* A20_ERR <-> Linux errno mapping for core mm calls (mm/mremap.c et al). */
static int64_t a20_mm_errno_map(int r)
{
    switch (r) {
    case -EINVAL: return -A20_ERR_INVALID_ARGUMENT;
    case -ENOMEM: return -A20_ERR_NO_MEMORY;
    case -EFAULT: return -A20_ERR_FAULT;
    case -EPERM:  return -A20_ERR_PERM;
    default:      return -A20_ERR_INVALID_ARGUMENT;
    }
}

/*
 * vm_remap is VMA-aware: grow/shrink/move preserve file mappings and
 * shared VMO frames (docs/native-abi/09-… §8 vm_remap row).  prot, when
 * non-zero, re-applies protection over the resulting range.
 */
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

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;
    if (!a20_mm_user_range_ok(old_addr, (size_t)old_len))
        return -A20_ERR_FAULT;

    vaddr_t out = 0;
    int r = mm_mremap(cur->mm, (vaddr_t)old_addr, (size_t)old_len,
                      (size_t)new_len, MREMAP_MAYMOVE,
                      (vaddr_t)new_addr_hint, &out);
    if (r < 0)
        return a20_mm_errno_map(r);

    if (prot && new_len) {
        int pr = mm_mprotect(cur->mm, out, (size_t)new_len, (int)prot);
        if (pr < 0)
            return a20_mm_errno_map(pr);
    }
    return (int64_t)out;
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

    int r = mm_vma_set_lock(cur->mm, (vaddr_t)start, (vaddr_t)end,
                            (flags & 0x01) ? 1 : 0);
    if (r == -ENOMEM) return -A20_ERR_NO_MEMORY;
    return r < 0 ? -A20_ERR_INVALID_ARGUMENT : A20_OK;
}

int64_t sys_a20_vm_create_object(const a20_syscall_args_t *args)
{
    uint64_t size = A20_ARG(0);
    uint32_t options = (uint32_t)A20_ARG(1);

    if (size == 0) return -A20_ERR_INVALID_ARGUMENT;

    uint32_t vmo_type = VMO_ANONYMOUS;
    if (options & A20_VMO_PAGED)
        vmo_type = VMO_PAGED;
    if (options & ~(A20_VMO_PAGED))
        return -A20_ERR_INVALID_ARGUMENT;

    struct vmo *vmo = vmo_create(vmo_type, size, options);
    if (!vmo) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vmo_release(vmo); return -A20_ERR_BAD_HANDLE; }

    int64_t h = a20_handle_install(ht, vmo, A20_OBJ_MEMORY,
                                    A20_RIGHT_READ | A20_RIGHT_WRITE |
                                    A20_RIGHT_EXEC |
                                    A20_RIGHT_MAP | A20_RIGHT_STAT | A20_RIGHT_DUP |
                                    A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL);
    if (h < 0) vmo_release(vmo);
    return h;
}
