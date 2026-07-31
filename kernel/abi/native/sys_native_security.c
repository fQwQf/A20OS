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
#include "abi/native/objects.h"
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
extern void a20_object_release(void *object, uint16_t type);

extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);

/* ===== Security (0x0800) ===== */

int64_t sys_a20_ns_create(const a20_syscall_args_t *args)
{
    uint32_t ns_type = (uint32_t)A20_ARG(0);
    uint32_t flags = (uint32_t)A20_ARG(1);
    (void)flags;

    if (ns_type > 3) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    struct a20_namespace *ns = kmalloc(sizeof(struct a20_namespace));
    if (!ns) return -A20_ERR_NO_MEMORY;
    memset(ns, 0, sizeof(*ns));
    refcount_set(&ns->refcount, 1);
    ns->ns_type = ns_type;
    ns->flags = flags;

    switch (ns_type) {
    case A20_NS_FILESYSTEM:
        if (cur->fs.root_path[0])
            strncpy(ns->root_path, cur->fs.root_path, MAX_PATH_LEN - 1);
        else {
            ns->root_path[0] = '/';
            ns->root_path[1] = '\0';
        }
        break;
    case A20_NS_NETWORK:
        ns->net_ifindex = 0;
        break;
    case A20_NS_PID:
        ns->pid_offset = (uint64_t)cur->pid << 32;
        break;
    case A20_NS_DEVICE:
        ns->dev_access_mask = 0xFFFFFFFF;
        break;
    }

    int64_t h = a20_handle_install(ht, ns, A20_OBJ_NAMESPACE,
                                    A20_RIGHT_CONTROL | A20_RIGHT_DUP |
                                    A20_RIGHT_TRANSFER | A20_RIGHT_STAT |
                                    A20_RIGHT_ADMIN);
    if (h < 0) kfree(ns);
    return h;
}

int64_t sys_a20_ns_apply(const a20_syscall_args_t *args)
{
    a20_handle_t ns_h = (a20_handle_t)A20_ARG(0);
    a20_handle_t task_h = (a20_handle_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t ns_entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, ns_h, A20_OBJ_NAMESPACE,
                                               A20_RIGHT_ADMIN, &ns_entry);

    if (r < 0) return r;

    a20_handle_entry_t task_entry;
    r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                    A20_RIGHT_CONTROL, &task_entry);
    if (r < 0) {
        a20_object_release(ns_entry.object, ns_entry.type);
        return r;
    }

    struct a20_namespace *ns = (struct a20_namespace *)ns_entry.object;
    task_t *target = proc_find_get((int)(uintptr_t)task_entry.object);
    if (!ns || !target) {
        a20_object_release(ns_entry.object, ns_entry.type);
        proc_put(target);
        return -A20_ERR_BAD_HANDLE;
    }


    switch (ns->ns_type) {
    case A20_NS_FILESYSTEM:
        if (ns->root_path[0]) {
            strncpy(target->fs.root_path, ns->root_path, MAX_PATH_LEN - 1);
            strncpy(target->ns_ctx.fs_root, ns->root_path, MAX_PATH_LEN - 1);
        }
        target->ns_ctx.active_ns |= (1U << A20_NS_FILESYSTEM);
        break;
    case A20_NS_NETWORK:
        target->ns_ctx.net_ifindex = ns->net_ifindex;
        target->ns_ctx.active_ns |= (1U << A20_NS_NETWORK);
        break;
    case A20_NS_PID:
        target->ns_ctx.pid_offset = ns->pid_offset;
        target->ns_ctx.active_ns |= (1U << A20_NS_PID);
        break;
    case A20_NS_DEVICE:
        target->ns_ctx.dev_access_mask = ns->dev_access_mask;
        target->ns_ctx.active_ns |= (1U << A20_NS_DEVICE);
        break;
    }

    proc_put(target);
    a20_object_release(ns_entry.object, ns_entry.type);
    return A20_OK;
}

int64_t sys_a20_security_get_context(const a20_syscall_args_t *args)
{
    a20_security_context_t *out = (a20_security_context_t *)A20_ARG(0);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);

    a20_security_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.size = sizeof(ctx);
    ctx.version = 1;
    if (cur) {
        ctx.uid = cur->cred.uid;
        ctx.gid = cur->cred.gid;
        ctx.euid = cur->cred.euid;
        ctx.egid = cur->cred.egid;
    }
    /* docs/native-abi/06-security.md §5.1: label from handle table */
    ctx.label = a20_ht_get_label(ht);
    if (copy_to_user(out, &ctx, sizeof(ctx)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

/*
 * sys_a20_security_set_context — modify security context.
 * docs/native-abi/06-security.md §5: label can only increase (No Write Down
 * for self). A process can raise its own label but never lower it.
 * This enforces the monotonic label property required by BLP.
 */
int64_t sys_a20_security_set_context(const a20_syscall_args_t *args)
{
    a20_security_context_t *uargs = (a20_security_context_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_security_context_t ctx;
    A20_VALIDATE_AND_COPY(uargs, ctx);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    /* Label can only increase: L→M, L→H, M→H, never down */
    uint8_t old_label = a20_ht_get_label(ht);
    if (ctx.label > 2) return -A20_ERR_INVALID_ARGUMENT;
    if (ctx.label < old_label) return -A20_ERR_ACCESS;

    a20_ht_set_label(ht, ctx.label);
    return A20_OK;
}

/* NATIVE_DEBUG_LIMITED_CONTRACT: Debug (0x0900) — limited compatibility
 * implementations without full stop/resume/watchpoint behavior. */

int64_t sys_a20_debug_attach(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    /* docs/native-abi/06-security.md §8.1: attach requires ADMIN right on the target task */
    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_ADMIN, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    if (!target) return -A20_ERR_BAD_HANDLE;

    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)target->pid,
                                    A20_OBJ_DEBUG,
                                    A20_RIGHT_READ | A20_RIGHT_WRITE |
                                    A20_RIGHT_CONTROL);
    proc_put(target);
    return h;
}

int64_t sys_a20_debug_read_regs(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    a20_regs_t *out = (a20_regs_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    a20_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    if (target && target->trap_ctx) {
        trap_context_t *tc = target->trap_ctx;
        for (int i = 0; i < 32; i++)
            regs.regs[i] = TRAP_CTX_REG(tc, i);
        regs.pc = TRAP_CTX_EPC(tc);
        regs.sp = TRAP_CTX_SP(tc);
        regs.sr = TRAP_CTX_STATUS(tc);
    }

    proc_put(target);
    if (copy_to_user(out, &regs, sizeof(regs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;

}

int64_t sys_a20_debug_write_regs(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    const a20_regs_t *uregs = (const a20_regs_t *)A20_ARG(1);
    if (!uregs) return -A20_ERR_FAULT;

    a20_regs_t kregs;
    if (copy_from_user(&kregs, uregs, sizeof(kregs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    if (target && target->trap_ctx) {
        trap_context_t *tc = target->trap_ctx;
        for (int i = 0; i < 32; i++)
            TRAP_CTX_SET_REG(tc, i, kregs.regs[i]);
        TRAP_CTX_EPC(tc) = kregs.pc;
        TRAP_CTX_SET_SP(tc, kregs.sp);
        TRAP_CTX_STATUS(tc) = kregs.sr;
    }
    proc_put(target);
    return A20_OK;
}

static int a20_debug_user_range_ok(uint64_t va, size_t n)
{
    va = (uint64_t)(vaddr_t)va;
    if (n == 0) return 1;
    if (va >= USER_VA_LIMIT) return 0;
    return n <= USER_VA_LIMIT - va;
}

static int a20_debug_copy_from_task(mm_struct_t *mm, uint64_t remote_addr,
                                    void *dst, size_t len)
{
    if (!mm || !mm->pgdir)
        return -A20_ERR_BAD_HANDLE;
    if (!a20_debug_user_range_ok(remote_addr, len))
        return -A20_ERR_FAULT;

    size_t done = 0;
    while (done < len) {
        uint64_t va = remote_addr + done;
        vm_area_t *vma = mm_find_vma(mm, va);
        if (!vma || va < vma->start || va >= vma->end)
            return -A20_ERR_FAULT;
        paddr_t pa = pt_translate(mm->pgdir, va);
        if (!pa)
            return -A20_ERR_FAULT;
        pfn_t pfn = phys_to_pfn(pa);
        if (!pfn_valid(pfn))
            return -A20_ERR_FAULT;
        size_t chunk = PAGE_SIZE - (va & (PAGE_SIZE - 1));
        if (chunk > len - done)
            chunk = len - done;
        memcpy((char *)dst + done,
               (char *)pfn_to_virt(pfn) + (pa & (PAGE_SIZE - 1)), chunk);
        done += chunk;
    }
    return A20_OK;
}

int64_t sys_a20_debug_map_memory(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    uint64_t remote_addr = A20_ARG(1);
    uint64_t len = A20_ARG(2);
    uint32_t prot = (uint32_t)A20_ARG(3);

    if (len == 0 || len > (16ULL << 20)) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    if (!target) return -A20_ERR_BAD_HANDLE;
    mm_struct_t *target_mm = proc_task_get_mm(target);
    if (!target_mm) {
        proc_put(target);
        return -A20_ERR_BAD_HANDLE;
    }

    uint64_t local = proc_mmap(0, (size_t)len, (int)prot ? (int)prot : 3,
                                0x20 /* MAP_ANONYMOUS */, -1, 0);
    if (local == 0) {
        mm_destroy(target_mm);
        proc_put(target);
        return -A20_ERR_NO_MEMORY;
    }

    char kbuf[512];
    uint64_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > sizeof(kbuf)) chunk = sizeof(kbuf);
        r = a20_debug_copy_from_task(target_mm, remote_addr + done, kbuf, chunk);
        if (r < 0) goto out_unmap;
        if (copy_to_user((void *)(local + done), kbuf, chunk) < 0) {
            r = -A20_ERR_FAULT;
            goto out_unmap;
        }
        done += chunk;
    }

    mm_destroy(target_mm);
    proc_put(target);
    return (int64_t)local;

out_unmap:
    proc_munmap(local, (size_t)len);
    mm_destroy(target_mm);
    proc_put(target);
    return r;
}
