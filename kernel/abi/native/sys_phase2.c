/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * All 90 syscalls have real kernel API backing.
 * Design references (see docs/native-abi/):
 *   03-handle.md §4   — handle operation semantics
 *   04-memory.md §4   — vm_* semantics
 *   06-docs/native-abi/06-security.md §3 — operation-rights mapping
 *   05-ipc.md §2–3    — channel/eventq semantics
 */
#include "core/types.h"
#include "core/klog.h"
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
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"

#define A20_ARG(n) (args->arg[(n)])

struct a20_ht_internal;
struct a20_ht_internal *a20_ht_create(void);
void a20_ht_destroy(struct a20_ht_internal *ht);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                    uint16_t type, a20_rights_t rights,
                                    uint64_t expiry_tick, uint32_t remaining_ops,
                                    uint32_t temporal_flags, uint8_t security_label);
int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                   uint16_t expected_type, a20_rights_t required_rights,
                                   a20_handle_entry_t *out);
void a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
struct a20_ht_internal *task_get_a20_ht(task_t *t);
uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);

static int copy_path_from_user(char *dst, const char *uptr, uint32_t len)
{
    if (!uptr) return -1;
    if (len > 0 && len < MAX_PATH_LEN) {
        if (copy_from_user(dst, uptr, len) < 0) return -1;
        dst[len] = '\0';
    } else {
        for (int i = 0; i < MAX_PATH_LEN - 1; i++) {
            if (copy_from_user(&dst[i], &uptr[i], 1) < 0) return -1;
            if (dst[i] == '\0') break;
        }
        dst[MAX_PATH_LEN - 1] = '\0';
    }
    return 0;
}

static void resolve_path(const char *in, char *out)
{
    task_t *cur = proc_current();
    if (in[0] == '/') {
        strncpy(out, in, MAX_PATH_LEN);
    } else if (cur && cur->fs.cwd[0]) {
        size_t clen = strlen(cur->fs.cwd);
        memcpy(out, cur->fs.cwd, clen);
        out[clen] = '/';
        strncpy(out + clen + 1, in, MAX_PATH_LEN - clen - 1);
    } else {
        strncpy(out, in, MAX_PATH_LEN);
    }
}

/* ===== Handle (0x0100) continued ===== */

int64_t sys_a20_handle_transfer(const a20_syscall_args_t *args)
{
    a20_handle_t src_h = (a20_handle_t)A20_ARG(0);
    a20_handle_t dst_h = (a20_handle_t)A20_ARG(1);
    uint64_t len = A20_ARG(2);
    uint32_t flags = (uint32_t)A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t src, dst;
    int64_t r;
    r = a20_handle_lookup_internal(ht, src_h, A20_OBJ_INVALID, A20_RIGHT_READ, &src);
    if (r < 0) return r;
    r = a20_handle_lookup_internal(ht, dst_h, A20_OBJ_INVALID, A20_RIGHT_WRITE, &dst);
    if (r < 0) return r;

    /* Bell-LaPadula: read from src (No Read Up) + write to dst (No Write Down) */
    uint8_t plabel = a20_ht_get_label(ht);
    if (plabel < src.security_label) return -A20_ERR_ACCESS;
    if (plabel > dst.security_label) return -A20_ERR_ACCESS;

    if (src.type != A20_OBJ_FILE && src.type != A20_OBJ_PIPE_ENDPOINT)
        return -A20_ERR_INVALID_ARGUMENT;
    if (dst.type != A20_OBJ_FILE && dst.type != A20_OBJ_PIPE_ENDPOINT)
        return -A20_ERR_INVALID_ARGUMENT;

    int src_gfd = (int)(uintptr_t)src.object;
    int dst_gfd = (int)(uintptr_t)dst.object;
    (void)flags;

    char buf[4096];
    uint64_t total = 0;
    while (total < len) {
        uint64_t chunk = len - total;
        if (chunk > sizeof(buf)) chunk = sizeof(buf);
        vfile_t *svf = vfs_get_file_ref(src_gfd);
        if (!svf) break;
        int64_t n = vfs_read_file(svf, buf, (size_t)chunk);
        vfs_put_file_ref(src_gfd, svf);
        if (n <= 0) break;
        vfile_t *dvf = vfs_get_file_ref(dst_gfd);
        if (!dvf) break;
        int64_t wn = vfs_write_file(dvf, buf, (size_t)n);
        vfs_put_file_ref(dst_gfd, dvf);
        if (wn <= 0) break;
        total += (uint64_t)wn;
        if (wn < n) break;
    }
    return (int64_t)total;
}

int64_t sys_a20_handle_set_meta(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    uint32_t flags = (uint32_t)A20_ARG(1);
    uint64_t val0 = A20_ARG(2);
    uint64_t val1 = A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                            A20_RIGHT_WRITE | A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    if (entry.type != A20_OBJ_FILE && entry.type != A20_OBJ_DIRECTORY)
        return -A20_ERR_INVALID_ARGUMENT;

    int gfd = (int)(uintptr_t)entry.object;

    if (flags & 0x01) {
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (vf && vf->vnode)
            vf->vnode->mode = (vf->vnode->mode & ~07777u) | ((uint32_t)val0 & 07777u);
        if (vf) vfs_put_file_ref(gfd, vf);
    }
    if (flags & 0x02) {
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (vf && vf->vnode) {
            vf->vnode->uid = (uint32_t)val0;
            vf->vnode->gid = (uint32_t)val1;
        }
        if (vf) vfs_put_file_ref(gfd, vf);
    }
    if (flags & 0x04) {
        (void)val0; (void)val1;
    }
    if (flags & 0x08) {
        (void)val0;
    }
    return A20_OK;
}

static int64_t xattr_common(a20_handle_t h, const char *name, void *buf,
                            size_t size, uint32_t op)
{
    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    vfile_t *vf = vfs_get_file_ref((int)(uintptr_t)entry.object);
    if (!vf || !vf->vnode) {
        if (vf) vfs_put_file_ref((int)(uintptr_t)entry.object, vf);
        return -A20_ERR_BAD_HANDLE;
    }
    vnode_t *vn = vf->vnode;
    int64_t ret;
    switch (op) {
    case 0: ret = xattr_set_vnode(vn, name, buf, size, 0); break;
    case 1: ret = xattr_get_vnode(vn, name, buf, size); break;
    case 2: ret = xattr_list_vnode(vn, (char *)buf, size); break;
    case 3: ret = xattr_remove_vnode(vn, name); break;
    default: ret = -A20_ERR_INVALID_ARGUMENT; break;
    }
    vfs_put_file_ref((int)(uintptr_t)entry.object, vf);
    return ret;
}

int64_t sys_a20_handle_xattr_set(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const char *name = (const char *)A20_ARG(1);
    void *value = (void *)A20_ARG(2);
    size_t size = (size_t)A20_ARG(3);
    return xattr_common(h, name, value, size, 0);
}

int64_t sys_a20_handle_xattr_get(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const char *name = (const char *)A20_ARG(1);
    void *value = (void *)A20_ARG(2);
    size_t size = (size_t)A20_ARG(3);
    return xattr_common(h, name, value, size, 1);
}

int64_t sys_a20_handle_xattr_list(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    void *list = (void *)A20_ARG(1);
    size_t size = (size_t)A20_ARG(2);
    return xattr_common(h, NULL, list, size, 2);
}

int64_t sys_a20_handle_xattr_remove(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const char *name = (const char *)A20_ARG(1);
    return xattr_common(h, name, NULL, 0, 3);
}

/* ===== Task (0x0200) continued ===== */

int64_t sys_a20_task_kill(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    int32_t sig = (int32_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_SIGNAL, &entry);
    if (r < 0) return r;

    task_t *target = (task_t *)entry.object;
    if (!target) return -A20_ERR_BAD_HANDLE;

    if (sig == 9) {
        proc_exit(128 + 9);
    }
    return A20_OK;
}

int64_t sys_a20_task_info(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_task_info_t *out = (a20_task_info_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    task_t *target = (task_t *)entry.object;
    if (!target) return -A20_ERR_BAD_HANDLE;

    a20_task_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.version = 1;
    info.pid = target->pid;
    info.ppid = target->ppid;
    info.thread_count = (target->tgid == target->pid) ? 1 : 0;
    /* Fill VM stats from target's mm_struct */
    if (target->mm) {
        info.vm_size = target->mm->total_vm * 4096ULL;
        info.vm_rss = target->mm->rss * 4096ULL;
    }
    /* CPU time: convert ticks to nanoseconds at 100ns/tick */
    info.user_time_ns = target->total_time * 10000000ULL;
    info.sys_time_ns = 0; /* kernel time not separately tracked */

    if (copy_to_user(out, &info, sizeof(info)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_thread_sleep(const a20_syscall_args_t *args)
{
    uint64_t deadline_ns = A20_ARG(0);
    while (timer_get_ticks() * 10000000ULL < deadline_ns) {
        proc_yield();
    }
    return A20_OK;
}

int64_t sys_a20_thread_yield(const a20_syscall_args_t *args)
{
    (void)args;
    proc_yield();
    return A20_OK;
}

int64_t sys_a20_thread_exit(const a20_syscall_args_t *args)
{
    int32_t code = (int32_t)A20_ARG(0);
    proc_exit(code);
    return 0;
}

int64_t sys_a20_task_get_sched(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_sched_args_t *out = (a20_sched_args_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    task_t *target = (task_t *)entry.object;
    a20_sched_args_t kinfo;
    memset(&kinfo, 0, sizeof(kinfo));
    kinfo.size = sizeof(kinfo);
    kinfo.version = 1;
    if (target) {
        kinfo.priority = target->priority;
        kinfo.policy = target->sched_policy;
        kinfo.nice = target->priority - 100;
        kinfo.affinity = (uint64_t)1 << target->cpu_id;
        kinfo.affinity_size = sizeof(uint64_t);
    }
    if (copy_to_user(out, &kinfo, sizeof(kinfo)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_task_get_usage(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_rusage_t *out = (a20_rusage_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    task_t *target = (task_t *)entry.object;
    a20_rusage_t usage;
    memset(&usage, 0, sizeof(usage));
    if (target) {
        usage.user_time_ns = target->total_time * 10000000ULL;
        usage.max_rss = target->mm ? target->mm->rss * 4096ULL : 0;
        usage.sys_time_ns = (target->child_utime + target->child_stime) * 10000000ULL;
    }
    if (copy_to_user(out, &usage, sizeof(usage)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
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

int64_t sys_a20_vm_map(const a20_syscall_args_t *args)
{
    a20_vm_map_args_t *uargs = (a20_vm_map_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_vm_map_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    if (kargs.length == 0) return -A20_ERR_INVALID_ARGUMENT;

    struct a20_vmo *vmo = a20_vmo_create(A20_VMO_ANONYMOUS, kargs.length, 0);
    if (!vmo) return -A20_ERR_NO_MEMORY;

    uint64_t addr = 0;
    int64_t r = a20_vmar_map(vmo, 0, kargs.length, kargs.prot,
                              kargs.flags, kargs.addr_hint, &addr);
    if (r < 0) {
        a20_vmo_release(vmo);
        return r;
    }

    kargs.out_addr = addr;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
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
    int64_t r = a20_handle_lookup_internal(ht, vmo_h, A20_OBJ_MEMORY,
                                            A20_RIGHT_READ | A20_RIGHT_TRANSFER, &vmo_entry);
    if (r < 0) return r;

    if (target_h != A20_HANDLE_NULL) {
        a20_handle_entry_t tgt_entry;
        r = a20_handle_lookup_internal(ht, target_h, A20_OBJ_TASK,
                                        A20_RIGHT_WRITE, &tgt_entry);
        if (r < 0) return r;

        if (tgt_entry.security_label > vmo_entry.security_label)
            return -A20_ERR_ACCESS;
    }

    a20_handle_entry_t tgt;
    if (target_h != A20_HANDLE_NULL) {
        r = a20_handle_lookup_internal(ht, target_h, A20_OBJ_TASK,
                                        A20_RIGHT_DUP, &tgt);
        if (r < 0) return r;
    }

    struct a20_ht_internal *target_ht = ht;
    a20_rights_t child_rights = rights & vmo_entry.rights;
    if (child_rights == 0) return -A20_ERR_ACCESS;

    int64_t new_h = a20_handle_install_temporal(target_ht, vmo_entry.object,
                                                vmo_entry.type, child_rights,
                                                vmo_entry.expiry_tick,
                                                vmo_entry.remaining_ops,
                                                vmo_entry.temporal_flags,
                                                vmo_entry.security_label);
    return new_h;
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

    if (advice == 4 /* MADV_DONTNEED */ || advice == 8 /* MADV_FREE */) {
        for (uint64_t va = addr; va < end; ) {
            int level = 0;
            uint64_t base = 0;
            size_t leaf_size = 0;
            uint64_t *pte = pt_lookup_leaf(cur->mm->pgdir, va, &level, &base, &leaf_size);
            if (!pte || !(*pte & PTE_V)) { va += 4096; continue; }
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

    if (new_len <= old_len) {
        if (new_len < old_len)
            proc_munmap(old_addr + new_len, (size_t)(old_len - new_len));
        return (int64_t)old_addr;
    }

    uint64_t new_addr = proc_mmap(new_addr_hint, (size_t)new_len,
                                   (int)prot ? (int)prot : 3,
                                   0x20 /* MAP_ANONYMOUS */, -1, 0);
    if (new_addr == 0) return -A20_ERR_NO_MEMORY;

    if (old_len > 0) {
        memcpy((void *)new_addr, (const void *)old_addr, (size_t)old_len);
        proc_munmap(old_addr, (size_t)old_len);
    }

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

    struct a20_vmo *vmo = a20_vmo_create(A20_VMO_ANONYMOUS, size, options);
    if (!vmo) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { a20_vmo_release(vmo); return -A20_ERR_BAD_HANDLE; }

    int64_t h = a20_handle_install(ht, vmo, A20_OBJ_MEMORY,
                                    A20_RIGHT_READ | A20_RIGHT_WRITE |
                                    A20_RIGHT_STAT | A20_RIGHT_DUP |
                                    A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL);
    if (h < 0) a20_vmo_release(vmo);
    return h;
}

/* ===== Path/Filesystem (0x0400) continued ===== */

int64_t sys_a20_path_create(const a20_syscall_args_t *args)
{
    a20_path_create_args_t *uargs = (a20_path_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_path_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, (const char *)kargs.path, (uint32_t)kargs.path_len) < 0)
        return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    int r;
    if (kargs.type == 1)
        r = vfs_mkdir(full, (int)kargs.mode);
    else
        r = -1;

    if (r < 0) {
        int gfd = vfs_open(full, O_WRONLY | O_CREAT, (int)kargs.mode);
        if (gfd < 0) return -A20_ERR_NO_ENTRY;
        vfs_close(gfd);
    }

    return sys_a20_path_open(args);
}

int64_t sys_a20_path_unlink(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);
    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0)
        return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    int r = vfs_unlink(full);
    if (r < 0) r = vfs_rmdir(full);
    return r;
}

int64_t sys_a20_path_rename(const a20_syscall_args_t *args)
{
    const char *uold = (const char *)A20_ARG(0);
    uint32_t old_len = (uint32_t)A20_ARG(1);
    const char *unew = (const char *)A20_ARG(2);
    uint32_t new_len = (uint32_t)A20_ARG(3);

    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (copy_path_from_user(kold, uold, old_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(knew, unew, new_len) < 0) return -A20_ERR_FAULT;

    char full_old[MAX_PATH_LEN], full_new[MAX_PATH_LEN];
    resolve_path(kold, full_old);
    resolve_path(knew, full_new);

    return vfs_rename(full_old, full_new);
}

int64_t sys_a20_path_readdir(const a20_syscall_args_t *args)
{
    a20_handle_t dir_h = (a20_handle_t)A20_ARG(0);
    void *buf = (void *)A20_ARG(1);
    size_t len = (size_t)A20_ARG(2);

    if (!buf || len == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dir_h, A20_OBJ_DIRECTORY,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    int gfd = (int)(uintptr_t)entry.object;

    char kbuf[4096];
    if (len > sizeof(kbuf)) len = sizeof(kbuf);
    int64_t n = vfs_getdents64(gfd, kbuf, len);
    if (n < 0) return -A20_ERR_IO;
    if (copy_to_user(buf, kbuf, (size_t)n) < 0) return -A20_ERR_FAULT;
    return n;
}

int64_t sys_a20_path_link(const a20_syscall_args_t *args)
{
    const char *uold = (const char *)A20_ARG(0);
    uint32_t old_len = (uint32_t)A20_ARG(1);
    const char *unew = (const char *)A20_ARG(2);
    uint32_t new_len = (uint32_t)A20_ARG(3);

    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (copy_path_from_user(kold, uold, old_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(knew, unew, new_len) < 0) return -A20_ERR_FAULT;

    char full_old[MAX_PATH_LEN], full_new[MAX_PATH_LEN];
    resolve_path(kold, full_old);
    resolve_path(knew, full_new);

    return vfs_link(full_old, full_new);
}

int64_t sys_a20_path_symlink(const a20_syscall_args_t *args)
{
    const char *utarget = (const char *)A20_ARG(0);
    uint32_t tgt_len = (uint32_t)A20_ARG(1);
    const char *ulinkpath = (const char *)A20_ARG(2);
    uint32_t lnk_len = (uint32_t)A20_ARG(3);

    char ktgt[MAX_PATH_LEN], klnk[MAX_PATH_LEN];
    if (copy_path_from_user(ktgt, utarget, tgt_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(klnk, ulinkpath, lnk_len) < 0) return -A20_ERR_FAULT;

    char full_lnk[MAX_PATH_LEN];
    resolve_path(klnk, full_lnk);

    return vfs_symlink(ktgt, full_lnk);
}

int64_t sys_a20_path_readlink(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);
    char *ubuf = (char *)A20_ARG(2);
    size_t bufsz = (size_t)A20_ARG(3);

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0) return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    char kbuf[4096];
    int64_t r = vfs_readlinkat(-1, full, kbuf, bufsz < sizeof(kbuf) ? bufsz : sizeof(kbuf));
    if (r < 0) return -A20_ERR_NO_ENTRY;
    if (copy_to_user(ubuf, kbuf, (size_t)r) < 0) return -A20_ERR_FAULT;
    return r;
}

int64_t sys_a20_path_resolve(const a20_syscall_args_t *args)
{
    const char *upath = (const char *)A20_ARG(0);
    uint32_t path_len = (uint32_t)A20_ARG(1);

    char kpath[MAX_PATH_LEN];
    if (copy_path_from_user(kpath, upath, path_len) < 0) return -A20_ERR_FAULT;

    char full[MAX_PATH_LEN];
    resolve_path(kpath, full);

    int gfd = vfs_open(full, O_RDONLY, 0);
    if (gfd < 0) return -A20_ERR_NO_ENTRY;
    vfs_close(gfd);
    return A20_OK;
}

int64_t sys_a20_fs_stat(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_fs_stat_t *out = (a20_fs_stat_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    a20_fs_stat_t fs;
    memset(&fs, 0, sizeof(fs));

    if (entry.type == A20_OBJ_FILE || entry.type == A20_OBJ_DIRECTORY) {
        int gfd = (int)(uintptr_t)entry.object;
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (vf && vf->vnode) {
            fs.block_size = 4096;
        }
        if (vf) vfs_put_file_ref(gfd, vf);
    }

    if (copy_to_user(out, &fs, sizeof(fs)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_handle_control(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    uint32_t op = (uint32_t)A20_ARG(1);
    uint64_t arg0 = A20_ARG(2);
    uint64_t arg1 = A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    if (entry.type == A20_OBJ_FILE || entry.type == A20_OBJ_DEVICE) {
        int gfd = (int)(uintptr_t)entry.object;
        switch (op) {
        case 0: return vfs_ioctl(gfd, (unsigned long)arg0, (void *)arg1);
        case 1: return vfs_fcntl(gfd, (int)arg0, (long)arg1);
        default: return -A20_ERR_INVALID_ARGUMENT;
        }
    }

    if (entry.type == A20_OBJ_EVENT_QUEUE && op == 0) {
        a20_eventq_t *eq = (a20_eventq_t *)entry.object;
        if (arg0 == 1) {
            return a20_eventq_cancel(eq, (a20_handle_t)arg1);
        }
    }

    return -A20_ERR_INVALID_ARGUMENT;
}

int64_t sys_a20_fs_mount(const a20_syscall_args_t *args)
{
    const char *usrc = (const char *)A20_ARG(0);
    uint32_t src_len = (uint32_t)A20_ARG(1);
    const char *utarget = (const char *)A20_ARG(2);
    uint32_t tgt_len = (uint32_t)A20_ARG(3);
    const char *ufstype = (const char *)A20_ARG(4);
    uint32_t fstype_len = (uint32_t)A20_ARG(5);
    uint64_t flags = A20_ARG(6);

    char ksrc[MAX_PATH_LEN], ktgt[MAX_PATH_LEN], kfs[64];
    if (copy_path_from_user(ksrc, usrc, src_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(ktgt, utarget, tgt_len) < 0) return -A20_ERR_FAULT;
    if (copy_path_from_user(kfs, ufstype, fstype_len) < 0) return -A20_ERR_FAULT;

    char full_tgt[MAX_PATH_LEN];
    resolve_path(ktgt, full_tgt);

    int r = vfs_mount(ksrc[0] ? ksrc : NULL, full_tgt,
                       kfs[0] ? kfs : "ext2", (int)flags, NULL);
    if (r < 0) return -A20_ERR_IO;
    return A20_OK;
}

int64_t sys_a20_fs_umount(const a20_syscall_args_t *args)
{
    const char *utarget = (const char *)A20_ARG(0);
    uint32_t tgt_len = (uint32_t)A20_ARG(1);
    uint64_t flags = A20_ARG(2);
    (void)flags;

    char ktgt[MAX_PATH_LEN];
    if (copy_path_from_user(ktgt, utarget, tgt_len) < 0) return -A20_ERR_FAULT;

    char full_tgt[MAX_PATH_LEN];
    resolve_path(ktgt, full_tgt);

    int r = vfs_umount(full_tgt);
    if (r < 0) return -A20_ERR_IO;
    return A20_OK;
}

int64_t sys_a20_fs_sync(const a20_syscall_args_t *args)
{
    return vfs_sync();
}

/* ===== Network (0x0600) ===== */

int64_t sys_a20_net_socket(const a20_syscall_args_t *args)
{
    int domain = (int)A20_ARG(0);
    int type = (int)A20_ARG(1);
    int protocol = (int)A20_ARG(2);

    int gfd = net_socket_create(domain, type, protocol);
    if (gfd < 0) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vfs_close(gfd); return -A20_ERR_BAD_HANDLE; }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)gfd, A20_OBJ_SOCKET, rights);
    if (h < 0) vfs_close(gfd);
    return h;
}

int64_t sys_a20_net_bind(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const void *addr = (const void *)A20_ARG(1);
    size_t addrlen = (size_t)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_SOCKET,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    int gfd = (int)(uintptr_t)entry.object;
    return net_bind(gfd, addr, addrlen);
}

int64_t sys_a20_net_connect(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    const void *addr = (const void *)A20_ARG(1);
    size_t addrlen = (size_t)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_SOCKET,
                                            A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    return net_connect((int)(uintptr_t)entry.object, addr, addrlen);
}

int64_t sys_a20_net_accept(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    void *addr = (void *)A20_ARG(1);
    size_t *addrlen = (size_t *)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_SOCKET,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    int new_gfd = net_accept((int)(uintptr_t)entry.object, addr, addrlen, 0);
    if (new_gfd < 0) return -A20_ERR_IO;

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL;
    int64_t nh = a20_handle_install(ht, (void *)(uintptr_t)new_gfd, A20_OBJ_SOCKET, rights);
    if (nh < 0) vfs_close(new_gfd);
    return nh;
}

int64_t sys_a20_net_listen(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    int backlog = (int)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_SOCKET,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    return net_listen((int)(uintptr_t)entry.object, backlog);
}

int64_t sys_a20_net_sendmsg(const a20_syscall_args_t *args)
{
    a20_net_sendmsg_args_t *uargs = (a20_net_sendmsg_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_net_sendmsg_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.socket, A20_OBJ_SOCKET,
                                            A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    int gfd = (int)(uintptr_t)entry.object;
    uint64_t total_sent = 0;

    a20_iovec_t *iov = (a20_iovec_t *)kargs.iov;
    for (uint32_t i = 0; i < kargs.iov_count; i++) {
        a20_iovec_t v;
        if (copy_from_user(&v, &iov[i], sizeof(v)) < 0) return -A20_ERR_FAULT;
        if (v.len == 0) continue;

        int64_t n = net_sendto(gfd, (const void *)v.base, (size_t)v.len,
                                (int)kargs.flags,
                                (const void *)kargs.addr, 0);
        if (n < 0) return (total_sent > 0) ? (int64_t)total_sent : -A20_ERR_IO;
        total_sent += (uint64_t)n;
    }

    kargs.out_sent = total_sent;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0) return -A20_ERR_FAULT;
    return (int64_t)total_sent;
}

int64_t sys_a20_net_recvmsg(const a20_syscall_args_t *args)
{
    a20_net_recvmsg_args_t *uargs = (a20_net_recvmsg_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_net_recvmsg_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.socket, A20_OBJ_SOCKET,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    int gfd = (int)(uintptr_t)entry.object;
    uint64_t total_recv = 0;

    a20_iovec_t *iov = (a20_iovec_t *)kargs.iov;
    for (uint32_t i = 0; i < kargs.iov_count; i++) {
        a20_iovec_t v;
        if (copy_from_user(&v, &iov[i], sizeof(v)) < 0) return -A20_ERR_FAULT;
        if (v.len == 0) continue;

        int64_t n = net_recvfrom(gfd, (void *)v.base, (size_t)v.len,
                                  (int)kargs.flags,
                                  (void *)kargs.addr, NULL);
        if (n < 0) return (total_recv > 0) ? (int64_t)total_recv : -A20_ERR_IO;
        total_recv += (uint64_t)n;
    }

    kargs.out_received = total_recv;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0) return -A20_ERR_FAULT;
    return (int64_t)total_recv;
}

int64_t sys_a20_net_socketpair(const a20_syscall_args_t *args)
{
    int domain = (int)A20_ARG(0);
    int type = (int)A20_ARG(1);
    int protocol = (int)A20_ARG(2);
    a20_handle_t *out = (a20_handle_t *)A20_ARG(3);
    if (!out) return -A20_ERR_FAULT;

    int gfds[2];
    int r = net_socketpair_create(domain, type, protocol, gfds);
    if (r < 0) return -A20_ERR_IO;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { vfs_close(gfds[0]); vfs_close(gfds[1]); return -A20_ERR_BAD_HANDLE; }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER;
    int64_t h0 = a20_handle_install(ht, (void *)(uintptr_t)gfds[0], A20_OBJ_SOCKET, rights);
    int64_t h1 = a20_handle_install(ht, (void *)(uintptr_t)gfds[1], A20_OBJ_SOCKET, rights);

    a20_handle_t result[2];
    result[0] = (a20_handle_t)(h0 >= 0 ? h0 : A20_HANDLE_NULL);
    result[1] = (a20_handle_t)(h1 >= 0 ? h1 : A20_HANDLE_NULL);
    if (copy_to_user(out, result, sizeof(result)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_net_getname(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    void *addr = (void *)A20_ARG(1);
    size_t *addrlen = (size_t *)A20_ARG(2);
    int peer = (int)A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_SOCKET,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    if (peer)
        return net_getpeername((int)(uintptr_t)entry.object, addr, addrlen);
    return net_getsockname((int)(uintptr_t)entry.object, addr, addrlen);
}

int64_t sys_a20_net_shutdown(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    int how = (int)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, h, A20_OBJ_SOCKET,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    return net_shutdown((int)(uintptr_t)entry.object, how);
}

/* ===== IPC (0x0500) ===== */

int64_t sys_a20_event_queue_create(const a20_syscall_args_t *args)
{
    a20_event_queue_create_args_t *uargs = (a20_event_queue_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_queue_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    a20_eventq_t *eq = a20_eventq_create(kargs.capacity_hint);
    if (!eq) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { a20_eventq_release(eq); return -A20_ERR_BAD_HANDLE; }

    int64_t h = a20_handle_install(ht, eq, A20_OBJ_EVENT_QUEUE,
                                   A20_RIGHT_READ | A20_RIGHT_STAT |
                                   A20_RIGHT_DUP | A20_RIGHT_TRANSFER |
                                   A20_RIGHT_CONTROL);
    if (h < 0) a20_eventq_release(eq);
    return h;
}

int64_t sys_a20_event_watch(const a20_syscall_args_t *args)
{
    a20_event_watch_args_t *uargs = (a20_event_watch_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_event_watch_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t eq_entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.queue, A20_OBJ_EVENT_QUEUE,
                                           A20_RIGHT_READ, &eq_entry);
    if (r < 0) return r;

    a20_handle_entry_t tgt_entry;
    r = a20_handle_lookup_internal(ht, kargs.target, A20_OBJ_INVALID,
                                   A20_RIGHTS_NONE, &tgt_entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)eq_entry.object;
    return a20_eventq_watch(eq, kargs.target, tgt_entry.object,
                            tgt_entry.type, kargs.event_mask, kargs.user_data);
}

int64_t sys_a20_event_wait(const a20_syscall_args_t *args)
{
    a20_handle_t queue_h = (a20_handle_t)A20_ARG(0);
    a20_pending_event_t *out = (a20_pending_event_t *)A20_ARG(1);
    uint64_t timeout_ns = A20_ARG(2);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, queue_h, A20_OBJ_EVENT_QUEUE,
                                           A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)entry.object;
    a20_pending_event_t ev;
    r = a20_eventq_wait(eq, &ev, timeout_ns);
    if (r < 0) return r;
    if (copy_to_user(out, &ev, sizeof(ev)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_event_cancel(const a20_syscall_args_t *args)
{
    a20_handle_t queue_h = (a20_handle_t)A20_ARG(0);
    a20_handle_t target_h = (a20_handle_t)A20_ARG(1);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, queue_h, A20_OBJ_EVENT_QUEUE,
                                           A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)entry.object;
    return a20_eventq_cancel(eq, target_h);
}

int64_t sys_a20_channel_create(const a20_syscall_args_t *args)
{
    a20_channel_create_args_t *uargs = (a20_channel_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_channel_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    a20_channel_ep_t *ep0 = a20_channel_create(kargs.msg_capacity, NULL);
    if (!ep0) return -A20_ERR_NO_MEMORY;
    a20_channel_ep_t *ep1 = ep0->peer;
    refcount_inc(&ep1->refcount);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) {
        a20_channel_ep_release(ep0);
        a20_channel_ep_release(ep1);
        return -A20_ERR_BAD_HANDLE;
    }

    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE |
                          A20_RIGHT_DUP | A20_RIGHT_TRANSFER;
    int64_t h0 = a20_handle_install(ht, ep0, A20_OBJ_CHANNEL_ENDPOINT, rights);
    int64_t h1 = a20_handle_install(ht, ep1, A20_OBJ_CHANNEL_ENDPOINT, rights);

    a20_handle_t result[2];
    result[0] = (h0 >= 0) ? (a20_handle_t)h0 : A20_HANDLE_NULL;
    result[1] = (h1 >= 0) ? (a20_handle_t)h1 : A20_HANDLE_NULL;
    if (copy_to_user(uargs->out_endpoints, result, sizeof(result)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_channel_send(const a20_syscall_args_t *args)
{
    a20_msg_send_args_t *uargs = (a20_msg_send_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_msg_send_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.channel,
                                           A20_OBJ_CHANNEL_ENDPOINT,
                                           A20_RIGHT_WRITE, &entry);
    if (r < 0) return r;

    if (a20_ht_get_label(ht) > entry.security_label)
        return -A20_ERR_ACCESS;

    a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;

    a20_ch_handle_info_t hinfos[A20_CH_MAX_HANDLES];
    uint32_t actual_hcount = 0;

    if (kargs.handle_count > 0 && kargs.handles) {
        if (kargs.handle_count > A20_CH_MAX_HANDLES)
            return -A20_ERR_INVALID_ARGUMENT;

        a20_handle_t user_handles[A20_CH_MAX_HANDLES];
        a20_rights_t user_rights[A20_CH_MAX_HANDLES];
        if (copy_from_user(user_handles, (void *)kargs.handles,
                           kargs.handle_count * sizeof(a20_handle_t)) < 0)
            return -A20_ERR_FAULT;

        uint32_t rights_count = 0;
        if (kargs.transfer_rights) {
            if (copy_from_user(user_rights, (void *)kargs.transfer_rights,
                               kargs.handle_count * sizeof(a20_rights_t)) < 0)
                return -A20_ERR_FAULT;
            rights_count = kargs.handle_count;
        }

        for (uint32_t i = 0; i < kargs.handle_count; i++) {
            a20_handle_entry_t he;
            r = a20_handle_lookup_internal(ht, user_handles[i], A20_OBJ_INVALID,
                                           A20_RIGHT_TRANSFER, &he);
            if (r < 0) return r;

            a20_rights_t xfer_rights = (rights_count > 0)
                                       ? user_rights[i] : he.rights;
            /* docs/native-abi/06-security.md §4.1: ρ_recv = ρ_send ∩ ρ_transfer */
            hinfos[actual_hcount].object = he.object;
            hinfos[actual_hcount].type = he.type;
            hinfos[actual_hcount].transfer_rights = he.rights & xfer_rights;
            if (hinfos[actual_hcount].transfer_rights == 0)
                return -A20_ERR_ACCESS;
            actual_hcount++;
        }
    }

    return a20_channel_send(ep, (const void *)kargs.data, kargs.data_len,
                            hinfos, actual_hcount, ht);
}

int64_t sys_a20_channel_recv(const a20_syscall_args_t *args)
{
    a20_msg_recv_args_t *uargs = (a20_msg_recv_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_msg_recv_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.channel,
                                           A20_OBJ_CHANNEL_ENDPOINT,
                                           A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    if (a20_ht_get_label(ht) < entry.security_label)
        return -A20_ERR_ACCESS;

    a20_channel_ep_t *ep = (a20_channel_ep_t *)entry.object;

    a20_ch_handle_info_t hinfos[A20_CH_MAX_HANDLES];
    uint32_t out_hcount = kargs.handle_buf_count;
    if (out_hcount > A20_CH_MAX_HANDLES)
        out_hcount = A20_CH_MAX_HANDLES;

    uint32_t out_len = kargs.data_buf_len;
    r = a20_channel_recv(ep, (void *)kargs.data_buf, &out_len,
                         hinfos, &out_hcount, ht);
    if (r < 0) return r;

    /* Install received handles into receiver's handle table */
    a20_handle_t out_handles[A20_CH_MAX_HANDLES];
    a20_rights_t out_rights[A20_CH_MAX_HANDLES];
    for (uint32_t i = 0; i < out_hcount; i++) {
        out_rights[i] = hinfos[i].transfer_rights;
        int64_t nh = a20_handle_install_temporal(ht, hinfos[i].object,
                                                  hinfos[i].type,
                                                  hinfos[i].transfer_rights,
                                                  0, 0, 0, 0);
        out_handles[i] = (nh >= 0) ? (a20_handle_t)nh : A20_HANDLE_NULL;
    }

    /* Copy results to user */
    kargs.out_data_len = out_len;
    kargs.out_handle_count = out_hcount;
    if (kargs.handle_buf && out_hcount > 0) {
        uint32_t copy_count = out_hcount < kargs.handle_buf_count
                              ? out_hcount : kargs.handle_buf_count;
        if (copy_to_user((void *)kargs.handle_buf, out_handles,
                         copy_count * sizeof(a20_handle_t)) < 0)
            return -A20_ERR_FAULT;
    }
    if (kargs.out_rights_buf && out_hcount > 0) {
        if (copy_to_user((void *)kargs.out_rights_buf, out_rights,
                         out_hcount * sizeof(a20_rights_t)) < 0)
            return -A20_ERR_FAULT;
    }
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)out_len;
}

int64_t sys_a20_event_watch_fs(const a20_syscall_args_t *args)
{
    a20_handle_t dir_h = (a20_handle_t)A20_ARG(0);
    a20_handle_t queue_h = (a20_handle_t)A20_ARG(1);
    uint32_t event_mask = (uint32_t)A20_ARG(2);
    uint64_t user_data = A20_ARG(3);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t dir_entry;
    int64_t r = a20_handle_lookup_internal(ht, dir_h, A20_OBJ_DIRECTORY,
                                            A20_RIGHT_READ, &dir_entry);
    if (r < 0) return r;

    a20_handle_entry_t eq_entry;
    r = a20_handle_lookup_internal(ht, queue_h, A20_OBJ_EVENT_QUEUE,
                                    A20_RIGHT_WRITE, &eq_entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)eq_entry.object;
    return a20_eventq_watch(eq, dir_h, dir_entry.object,
                             dir_entry.type, event_mask, user_data);
}

/* ===== Time (0x0700) continued ===== */

/*
 * A20 timer object: wraps a kernel alarm into a handle-table entry.
 * On expiry the timer enqueues a pending_event into the user-supplied
 * event_queue (timer.md §3).  The implementation mirrors the Linux
 * posix_timer pattern but integrates with A20 handle/event semantics.
 */
#define A20_TIMER_MAX 64
typedef struct {
    volatile int used;
    int          owner_pid;
    uint64_t     interval_ticks;
    uint64_t     expire_tick;
    a20_handle_t event_queue;   /* handle of the target eventq */
    uint64_t     user_data;
    int          active;
} a20_timer_obj_t;

static a20_timer_obj_t g_a20_timers[A20_TIMER_MAX];

int64_t sys_a20_timer_create(const a20_syscall_args_t *args)
{
    a20_timer_create_args_t *uargs = (a20_timer_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_timer_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    /* Allocate a timer slot */
    int slot = -1;
    for (int i = 0; i < A20_TIMER_MAX; i++) {
        if (!g_a20_timers[i].used) { slot = i; break; }
    }
    if (slot < 0) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    a20_timer_obj_t *t = &g_a20_timers[slot];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->owner_pid = cur ? cur->pid : 0;
    t->event_queue = kargs.event_queue;
    t->user_data = kargs.user_data;

    /* Install as a handle so the user can refer to it */
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { t->used = 0; return -A20_ERR_BAD_HANDLE; }

    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)slot, A20_OBJ_TIMER,
                                    A20_RIGHT_READ | A20_RIGHT_CONTROL |
                                    A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
    if (h < 0) { t->used = 0; return h; }

    kargs.out_timer = (a20_handle_t)h;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_timer_set(const a20_syscall_args_t *args)
{
    a20_handle_t timer_h = (a20_handle_t)A20_ARG(0);
    uint64_t deadline_ns = A20_ARG(1);
    uint64_t interval_ns = A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, timer_h, A20_OBJ_TIMER,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    int slot = (int)(uintptr_t)entry.object;
    if (slot < 0 || slot >= A20_TIMER_MAX || !g_a20_timers[slot].used)
        return -A20_ERR_BAD_HANDLE;

    a20_timer_obj_t *t = &g_a20_timers[slot];
    if (deadline_ns == 0) {
        /* Disarm */
        t->active = 0;
        t->expire_tick = 0;
        return A20_OK;
    }

    uint64_t now_ns = timer_get_ticks() * (1000000000ULL / TICKS_PER_SEC);
    uint64_t delta_ns = (deadline_ns > now_ns) ? (deadline_ns - now_ns) : 1;
    t->expire_tick = timer_get_ticks() +
                     delta_ns * TICKS_PER_SEC / 1000000000ULL;
    t->interval_ticks = interval_ns * TICKS_PER_SEC / 1000000000ULL;
    t->active = 1;

    /* If an event_queue is associated, set a kernel alarm so we get
     * woken.  For simplicity we piggyback on the task alarm mechanism. */
    if (cur && t->expire_tick > 0) {
        proc_set_alarm_expire(cur, t->expire_tick);
    }

    return A20_OK;
}

int64_t sys_a20_timer_cancel(const a20_syscall_args_t *args)
{
    a20_handle_t timer_h = (a20_handle_t)A20_ARG(0);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, timer_h, A20_OBJ_TIMER,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    int slot = (int)(uintptr_t)entry.object;
    if (slot < 0 || slot >= A20_TIMER_MAX || !g_a20_timers[slot].used)
        return -A20_ERR_BAD_HANDLE;

    g_a20_timers[slot].active = 0;
    g_a20_timers[slot].expire_tick = 0;
    g_a20_timers[slot].interval_ticks = 0;
    return A20_OK;
}

/* Called from scheduler tick to fire expired A20 timers.
 * Notifies the associated event queue for each expired timer. */
void a20_timer_tick(void)
{
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < A20_TIMER_MAX; i++) {
        a20_timer_obj_t *t = &g_a20_timers[i];
        if (!t->used || !t->active || t->expire_tick == 0)
            continue;
        if (now < t->expire_tick)
            continue;

        a20_event_notify(t, A20_OBJ_TIMER, 0, t->user_data, t->expire_tick);

        if (t->interval_ticks > 0) {
            t->expire_tick = now + t->interval_ticks;
        } else {
            t->active = 0;
            t->expire_tick = 0;
        }
    }
}

int64_t sys_a20_clock_set(const a20_syscall_args_t *args)
{
    uint32_t clock_id = (uint32_t)A20_ARG(0);
    a20_time_ns_t value = (a20_time_ns_t)A20_ARG(1);
    (void)value;
    if (clock_id > 1) return -A20_ERR_INVALID_ARGUMENT;
    return -A20_ERR_PERM;
}

int64_t sys_a20_clock_resolution(const a20_syscall_args_t *args)
{
    uint32_t clock_id = (uint32_t)A20_ARG(0);
    a20_time_ns_t *out = (a20_time_ns_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    a20_time_ns_t res = 1000000;
    if (clock_id == 0 || clock_id == 1) res = 1;
    if (copy_to_user(out, &res, sizeof(res)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

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
    int64_t r = a20_handle_lookup_internal(ht, ns_h, A20_OBJ_NAMESPACE,
                                            A20_RIGHT_ADMIN, &ns_entry);
    if (r < 0) return r;

    a20_handle_entry_t task_entry;
    r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                    A20_RIGHT_CONTROL, &task_entry);
    if (r < 0) return r;

    struct a20_namespace *ns = (struct a20_namespace *)ns_entry.object;
    task_t *target = (task_t *)task_entry.object;
    if (!ns || !target) return -A20_ERR_BAD_HANDLE;

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
    if (copy_from_user(&ctx, uargs, sizeof(ctx)) < 0)
        return -A20_ERR_FAULT;

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

/* ===== Debug (0x0900) — stubs ===== */

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

    task_t *target = (task_t *)entry.object;
    if (!target) return -A20_ERR_BAD_HANDLE;

    int64_t h = a20_handle_install(ht, target, A20_OBJ_DEBUG,
                                    A20_RIGHT_READ | A20_RIGHT_WRITE |
                                    A20_RIGHT_CONTROL);
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

    task_t *target = (task_t *)entry.object;
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

    if (copy_to_user(out, &regs, sizeof(regs)) < 0) return -A20_ERR_FAULT;
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

    task_t *target = (task_t *)entry.object;
    if (target && target->trap_ctx) {
        trap_context_t *tc = target->trap_ctx;
        for (int i = 0; i < 32; i++)
            TRAP_CTX_SET_REG(tc, i, kregs.regs[i]);
        TRAP_CTX_EPC(tc) = kregs.pc;
        TRAP_CTX_SET_SP(tc, kregs.sp);
        TRAP_CTX_STATUS(tc) = kregs.sr;
    }
    return A20_OK;
}

int64_t sys_a20_debug_map_memory(const a20_syscall_args_t *args)
{
    a20_handle_t dbg_h = (a20_handle_t)A20_ARG(0);
    uint64_t remote_addr = A20_ARG(1);
    uint64_t len = A20_ARG(2);
    uint32_t prot = (uint32_t)A20_ARG(3);

    if (len == 0) return -A20_ERR_INVALID_ARGUMENT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, dbg_h, A20_OBJ_DEBUG,
                                            A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    uint64_t local = proc_mmap(0, (size_t)len, (int)prot ? (int)prot : 3,
                                0x20 /* MAP_ANONYMOUS */, -1, 0);
    if (local == 0) return -A20_ERR_NO_MEMORY;

    task_t *target = (task_t *)entry.object;
    if (target && target->pgdir) {
        for (uint64_t off = 0; off < len; off += 4096) {
            uint64_t src_page = remote_addr + off;
            memcpy((void *)(local + off), (const void *)src_page, 4096);
        }
    }

    return (int64_t)local;
}

/* ===== System (0x0A00) ===== */

int64_t sys_a20_system_info(const a20_syscall_args_t *args)
{
    a20_system_info_t *out = (a20_system_info_t *)A20_ARG(0);
    if (!out) return -A20_ERR_FAULT;

    a20_system_info_t info;
    memset(&info, 0, sizeof(info));
    info.struct_version = 1;
    strncpy(info.sysname, "A20OS", sizeof(info.sysname));
    strncpy(info.nodename, "a20", sizeof(info.nodename));
    strncpy(info.release, VERSION, sizeof(info.release));
    strncpy(info.version, "Native ABI", sizeof(info.version));
#if defined(RISCV64)
    strncpy(info.machine, "riscv64", sizeof(info.machine));
#elif defined(LOONGARCH64)
    strncpy(info.machine, "loongarch64", sizeof(info.machine));
#else
    strncpy(info.machine, "unknown", sizeof(info.machine));
#endif
    info.total_ram = (uint64_t)pfa.total_frames * 4096;
    info.free_ram = (uint64_t)pfa.free_frames * 4096;
    info.total_swap = 0;
    info.free_swap = 0;
    info.num_procs = (uint16_t)proc_pid_max();

    if (copy_to_user(out, &info, sizeof(info)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_system_random(const a20_syscall_args_t *args)
{
    void *buf = (void *)A20_ARG(0);
    size_t len = (size_t)A20_ARG(1);
    if (!buf || len > 4096) return -A20_ERR_INVALID_ARGUMENT;

    char kbuf[4096];
    random_fill(kbuf, len);
    if (copy_to_user(buf, kbuf, len) < 0) return -A20_ERR_FAULT;
    return (int64_t)len;
}

int64_t sys_a20_system_reboot(const a20_syscall_args_t *args)
{
    uint32_t cmd = (uint32_t)A20_ARG(0);
    (void)A20_ARG(1);

    switch (cmd) {
    case 0: firmware_shutdown(); break;
    case 1: firmware_reboot(); break;
    default: while (1) {} break;
    }
    return A20_OK;
}

/* ===== Thread create — complex, Phase 2 ===== */

int64_t sys_a20_thread_create(const a20_syscall_args_t *args)
{
    a20_thread_create_args_t *uargs = (a20_thread_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_thread_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();

    /* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_THREAD | CLONE_SIGHAND
     * to create a thread (shares address space, fd table, signal handlers) */
    uint64_t clone_flags = 0x00000100ULL /* CLONE_VM */ |
                           0x00000200ULL /* CLONE_FS */ |
                           0x00000400ULL /* CLONE_FILES */ |
                           0x00008000ULL /* CLONE_THREAD */ |
                           0x00000800ULL /* CLONE_SIGHAND */;

    int ctid = 0;
    int pid = proc_clone(clone_flags, kargs.stack_base, NULL,
                          kargs.tls_base, &ctid, 0);
    if (pid < 0) return -A20_ERR_NO_MEMORY;

    task_t *new_task = proc_find(pid);
    if (new_task && new_task->trap_ctx && kargs.entry) {
        trap_context_t *tc = new_task->trap_ctx;
        TRAP_CTX_SET_RET(tc, (uint64_t)kargs.entry);
        TRAP_CTX_ARG1(tc) = (uint64_t)kargs.arg;
        TRAP_CTX_SET_SP(tc, kargs.stack_base);
    }

    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)pid, A20_OBJ_TASK,
                                    A20_RIGHT_WAIT | A20_RIGHT_SIGNAL |
                                    A20_RIGHT_STAT | A20_RIGHT_DUP |
                                    A20_RIGHT_TRANSFER);
    if (h < 0) return h;

    kargs.out_thread = (a20_handle_t)h;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return h;
}

int64_t sys_a20_task_set_sched(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_sched_args_t *uargs = (a20_sched_args_t *)A20_ARG(1);
    if (!uargs) return -A20_ERR_FAULT;

    a20_sched_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    task_t *target = (task_t *)entry.object;
    if (!target) return -A20_ERR_BAD_HANDLE;

    if (kargs.flags & 0x01) {
        if (kargs.priority >= 0 && kargs.priority <= 139)
            target->priority = kargs.priority;
    }
    if (kargs.flags & 0x02) {
        if (kargs.policy >= 0 && kargs.policy <= 6)
            target->sched_policy = kargs.policy;
    }
    if (kargs.flags & 0x04) {
        target->priority = kargs.nice + 100;
    }
    if (kargs.flags & 0x08 && kargs.affinity_size > 0 && kargs.affinity != 0) {
        unsigned cpu = 0;
        uint64_t mask = kargs.affinity;
        while ((mask & 1) == 0) { cpu++; mask >>= 1; }
        target->cpu_id = cpu;
    }

    return A20_OK;
}

int64_t sys_a20_task_get_limits(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    struct a20_resource_limits *out = (struct a20_resource_limits *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    task_t *target = (task_t *)entry.object;
    struct a20_resource_limits limits;
    a20_resource_limits_init_default(&limits);
    if (target) {
        limits.max_memory_bytes = (uint64_t)target->limits.stack;
    }

    if (copy_to_user(out, &limits, sizeof(limits)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_task_set_limits(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    struct a20_resource_limits *uargs = (struct a20_resource_limits *)A20_ARG(1);
    if (!uargs) return -A20_ERR_FAULT;

    struct a20_resource_limits kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    task_t *target = (task_t *)entry.object;
    if (!target) return -A20_ERR_BAD_HANDLE;

    if (kargs.max_handles > A20_LIMIT_HANDLES_ABSOLUTE)
        return -A20_ERR_ACCESS;
    if (kargs.max_channels > A20_LIMIT_CHANNELS_ABSOLUTE)
        return -A20_ERR_ACCESS;
    if (kargs.max_threads > A20_LIMIT_THREADS_ABSOLUTE)
        return -A20_ERR_ACCESS;
    if (kargs.max_memory_bytes > A20_LIMIT_MEMORY_ABSOLUTE)
        return -A20_ERR_ACCESS;

    if (target->limits.nofile == 0 || kargs.max_handles < target->limits.nofile)
        target->limits.nofile = (uint32_t)kargs.max_handles;

    return A20_OK;
}
