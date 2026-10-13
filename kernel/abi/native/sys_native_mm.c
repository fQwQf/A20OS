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
#include "mm/pt.h"
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

extern int64_t a20_native_vfs_result(int r);
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
    a20_vm_map_args_t *uargs = (void *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;
    a20_vm_map_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    if (!kargs.length || kargs.length > SIZE_MAX - (PAGE_SIZE - 1) ||
        (kargs.prot & ~(A20_PROT_READ | A20_PROT_WRITE | A20_PROT_EXEC)) ||
        (kargs.flags & ~(MAP_FIXED | MAP_FIXED_NOREPLACE)))
        return -A20_ERR_INVALID_ARGUMENT;
    size_t length = ROUND_UP((size_t)kargs.length, PAGE_SIZE);
    if (length > USER_VA_LIMIT || kargs.addr_hint >= USER_VA_LIMIT ||
        (kargs.offset & (PAGE_SIZE - 1)))
        return -A20_ERR_INVALID_ARGUMENT;
    int fixed = kargs.flags & (MAP_FIXED | MAP_FIXED_NOREPLACE);
    if ((kargs.addr_hint & (PAGE_SIZE - 1)) || (fixed && !kargs.addr_hint))
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    vmar_t *route = NULL;
    a20_handle_entry_t src = {0};
    struct vmo *anonymous = NULL;
    int64_t r = A20_OK;
    uint32_t prot = kargs.prot;
    if (kargs.vmar != A20_HANDLE_NULL) {
        a20_handle_entry_t ve;
        r = a20_handle_lookup_ref_internal(ht, kargs.vmar, A20_OBJ_VMAR,
                                           A20_RIGHT_MAP, &ve);
        if (r < 0) return r;
        route = (vmar_t *)ve.object;
        if (!vmar_cap_allows(route, prot) ||
            (fixed && !(route->cap & VMAR_CAN_MAP_SPECIFIC))) {
            r = -A20_ERR_ACCESS;
            goto out;
        }
        /* Check before MAP_FIXED can replace an existing mapping. Rolling
         * back an out-of-region map afterwards cannot restore its victim. */
        if (fixed && !vmar_contains(route, kargs.addr_hint, length)) {
            r = -A20_ERR_NO_SPACE;
            goto out;
        }
    }
    if (kargs.source != A20_HANDLE_NULL) {
        r = a20_handle_lookup_ref_internal(ht, kargs.source, A20_OBJ_INVALID,
                                           A20_RIGHT_MAP, &src);
        if (r < 0) goto out;
        prot = a20_vm_prot_eff(prot, src.rights);
        if (src.type != A20_OBJ_MEMORY && src.type != A20_OBJ_FILE &&
            src.type != A20_OBJ_DEVICE) {
            r = -A20_ERR_INVALID_ARGUMENT;
            goto out;
        }
        if (src.type != A20_OBJ_MEMORY && !(src.rights & A20_RIGHT_READ)) {
            r = -A20_ERR_ACCESS;
            goto out;
        }
    } else {
        anonymous = vmo_create(VMO_ANONYMOUS, length, 0);
        if (!anonymous) {
            r = -A20_ERR_NO_MEMORY;
            goto out;
        }
    }

    mm_struct_t *mm = cur->mm;
    vaddr_t addr = (vaddr_t)kargs.addr_hint;
    int placement = kargs.flags;
    mm_tlb_invalidate_begin(mm);
    uint64_t lock_flags = spin_lock_irqsave(&mm->lock);
    uint64_t limit = cur->limits.as / PAGE_SIZE;
    if (cur->limits.as && (mm->total_vm > limit ||
                          length / PAGE_SIZE > limit - mm->total_vm)) {
        r = -A20_ERR_NO_MEMORY;
        goto unlock;
    }
    if (route && !fixed) {
        /* Gap selection, containment and insertion share mm->lock. A hint
         * never lets the core gap search escape the reservation. */
        if (!addr || (addr & (PAGE_SIZE - 1)) ||
            !vmar_contains(route, addr, length) ||
            mm_range_overlaps(mm, addr, length, NULL))
            addr = mm_find_gap(mm, route->base, length);
        if (!addr || !vmar_contains(route, addr, length)) {
            r = -A20_ERR_NO_SPACE;
            goto unlock;
        }
        placement = MAP_FIXED_NOREPLACE;
    }
    if (anonymous || src.type == A20_OBJ_MEMORY) {
        struct vmo *vmo = anonymous ? anonymous : (struct vmo *)src.object;
        addr = mm_mmap_vmo_cap_locked(mm, addr, length, a20_prot_to_mmap(prot),
                                      MAP_SHARED | placement, vmo, kargs.offset, prot);
    } else {
        addr = mm_mmap_file_cap_locked(mm, addr, length, a20_prot_to_mmap(prot),
                                   MAP_PRIVATE | placement,
                                   (struct vfile *)src.object, kargs.offset, prot);
        if (!mm_addr_is_error(addr))
            src.object = NULL; /* VMA consumes the file lookup reference. */
    }
    if (mm_addr_is_error(addr)) {
        r = a20_native_vfs_result(mm_addr_error(addr));
        goto unlock;
    }
unlock:
    spin_unlock_irqrestore(&mm->lock, lock_flags);
    mm_tlb_invalidate_finish(mm);
    if (r < 0) goto out;
    kargs.out_addr = addr;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_vmar_unmap(addr, length);
        r = -A20_ERR_FAULT;
    }
out:
    if (src.object) a20_object_release(src.object, src.type);
    if (anonymous) vmo_release(anonymous);
    if (route) vmar_release(route);
    return r;
}

/* vm_create_vmar — sub-allocate an address-range reservation under a
 * parent (or create a root when parent is NULL).  Ceilings narrow
 * monotonically down the tree; ranges must be page-aligned, inside the
 * parent, and disjoint from siblings (docs/native-abi/04-memory.md §3). */
int64_t sys_a20_vm_create_vmar(const a20_syscall_args_t *args)
{
    a20_vm_create_vmar_args_t *uargs =
        (a20_vm_create_vmar_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_vm_create_vmar_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    const uint64_t known = A20_VMAR_CAN_MAP_READ | A20_VMAR_CAN_MAP_WRITE |
                           A20_VMAR_CAN_MAP_EXEC | A20_VMAR_CAN_MAP_SPECIFIC;
    if (kargs.flags & ~known) return -A20_ERR_INVALID_ARGUMENT;
    if (!kargs.length || kargs.base >= USER_VA_LIMIT ||
        kargs.length > USER_VA_LIMIT - kargs.base ||
        ((kargs.base | kargs.length) & (PAGE_SIZE - 1)))
        return -A20_ERR_INVALID_ARGUMENT;

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
        if (r < 0)
            return a20_native_vfs_result((int)r);
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
        mm_seg_t *seg = mm_seg_find(cur->mm, va);
        if (!seg || va >= seg->end) return -A20_ERR_NO_MEMORY;
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

/* A20_ERR <-> Linux errno mapping for core mm calls (mm/madvise.c,
 * mm/mremap.c et al). */
static int64_t a20_mm_errno_map(int r)
{
    switch (r) {
    case -EINVAL:  return -A20_ERR_INVALID_ARGUMENT;
    case -ENOMEM:  return -A20_ERR_NO_MEMORY;
    case -EFAULT:  return -A20_ERR_FAULT;
    case -EACCES:  return -A20_ERR_ACCESS;
    case -EPERM:   return -A20_ERR_PERM;
    case -ENOSYS:  return -A20_ERR_NOT_SUPPORTED;
    default:       return -A20_ERR_INVALID_ARGUMENT;
    }
}

int64_t sys_a20_vm_advise(const a20_syscall_args_t *args)
{
    uint64_t addr = A20_ARG(0);
    uint64_t len = A20_ARG(1);
    int advice = (int)(uint32_t)A20_ARG(2);

    if (len == 0) return A20_OK;
    if (addr & 4095) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;

    int r = mm_madvise(cur->mm, (vaddr_t)addr, (size_t)len, advice);
    if (r < 0)
        return a20_mm_errno_map(r);
    return A20_OK;
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
    if (new_len > (256ULL << 20) || old_len > (256ULL << 20) || (prot & ~7U))
        return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_FAULT;
    if (!a20_mm_user_range_ok(old_addr, (size_t)old_len))
        return -A20_ERR_FAULT;

    mm_struct_t *mm = cur->mm;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    mm_seg_t *source = mm_seg_find(mm, old_addr);
    int r = 0;
    vaddr_t out = 0;
    if (!source || source->start > old_addr || source->end <= old_addr) {
        r = -EFAULT;
        goto remap_out;
    }
    /* Reject permission errors before a move can invalidate the old address.
     * Source validation and remap share the lock with protect and unmap. */
    if (prot && (source->vm_flags & VM_NATIVE_CAP) && (prot & ~source->vmar_cap)) {
        r = -EACCES;
        goto remap_out;
    }
    if (prot) {
        int filtered = mm_wx_filter_prot(prot, "native_remap");
        if (filtered < 0) {
            r = filtered;
            goto remap_out;
        }
        prot = (uint32_t)filtered;
    }
    r = mm_mremap_locked(mm, (vaddr_t)old_addr, (size_t)old_len,
                      (size_t)new_len, MREMAP_MAYMOVE,
                      (vaddr_t)new_addr_hint, &out);
    if (r == 0 && prot && new_len)
        r = mm_mprotect_locked(mm, out, (size_t)new_len, (int)prot);
remap_out:
    spin_unlock_irqrestore(&mm->lock, flags);
    mm_tlb_invalidate_finish(mm);
    if (r < 0) return a20_mm_errno_map(r);
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
