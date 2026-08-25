#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "abi/linux/syscall_entry.h"
#include "core/mman.h"
#include "core/stdio.h"
#include "core/string.h"
#include "fs/dcookie.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/memfd.h"
#include "fs/page_cache.h"
#include "fs/vfs.h"
#include "ipc/seccomp.h"
#include "mm/fault.h"
#include "mm/mm.h"
#include "mm/process_vm.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "proc/proc.h"
#include "proc/rseq.h"
#include "sys/usercopy.h"

/*
 * Linux syscalls completed in the "finish the Linux ABI" work: small
 * compatibility and cross-process helpers that have a thin ABI wrapper and
 * delegate to the common layer (mm/process_vm.c, fs/page_cache.c) or to
 * existing process/VFS primitives.
 */

int64_t sys_restart_syscall(void)
{
    task_t *t = proc_current();
    if (!t || !t->restart_active)
        return -ENOSYS;

    linux_syscall_args_t args;
    args.nr = t->restart_nr;
    for (int i = 0; i < 6; i++)
        args.arg[i] = t->restart_args[i];
    args.ctx = t->trap_ctx;

    const linux_syscall_entry_t *entry = linux_syscall_lookup(args.nr);
    if (!entry) {
        t->restart_active = 0;
        return -ENOSYS;
    }
    return entry->handler(&args);
}

/* kcmp(2) type indices, fixed by include/uapi/linux/kcmp.h. */
enum {
    KCMP_FILE = 0,
    KCMP_VM,
    KCMP_FILES,
    KCMP_FS,
    KCMP_SIGHAND,
    KCMP_IO,
    KCMP_SYSVSEM,
    KCMP_EPOLL_TFD,
};

static int64_t kcmp_order(uintptr_t a, uintptr_t b)
{
    if (a == b)
        return 0;
    return a < b ? 1 : 2;
}

int64_t sys_kcmp(int pid1, int pid2, int type, unsigned long idx1,
                 unsigned long idx2)
{
    task_t *t1 = proc_find_get(pid1);
    task_t *t2 = proc_find_get(pid2);
    if (!t1 || !t2) {
        if (t1) proc_put(t1);
        if (t2) proc_put(t2);
        return -ESRCH;
    }

    int64_t r = -EINVAL;
    task_t *self = proc_current();
    if (type < 0 || type >= KCMP_EPOLL_TFD + 1)
        goto out;
    if (self && !proc_has_cap(self, CAP_SYS_PTRACE) &&
        (!proc_task_may_access(self, t1) ||
         !proc_task_may_access(self, t2))) {
        r = -EPERM;
        goto out;
    }

    switch (type) {
    case KCMP_FILE: {
        /* fd slots hold global open-file numbers, so numeric order over
         * gfd values is exactly Linux's open-file-description order. */
        int f1 = fdtable_get(t1, (int)idx1);
        int f2 = fdtable_get(t2, (int)idx2);
        if (f1 < 0 || f2 < 0) {
            r = -EBADF;
            break;
        }
        r = kcmp_order((uintptr_t)f1, (uintptr_t)f2);
        break;
    }
    case KCMP_VM:
        r = kcmp_order((uintptr_t)t1->mm, (uintptr_t)t2->mm);
        break;
    case KCMP_FILES:
        r = kcmp_order((uintptr_t)t1->files, (uintptr_t)t2->files);
        break;
    case KCMP_FS: {
        int c = strncmp(t1->fs.cwd, t2->fs.cwd, MAX_PATH_LEN);
        if (!c)
            c = strncmp(t1->fs.root_path, t2->fs.root_path, MAX_PATH_LEN);
        if (!c)
            c = (t1->fs.umask > t2->fs.umask) -
                (t1->fs.umask < t2->fs.umask);
        r = c ? (c < 0 ? 1 : 2) : 0;
        break;
    }
    case KCMP_SIGHAND:
        r = kcmp_order((uintptr_t)t1->signals, (uintptr_t)t2->signals);
        break;
    case KCMP_IO:
        /* No per-task io_context objects exist; every task compares equal,
         * matching Linux when neither side has an io_context. */
        r = 0;
        break;
    case KCMP_SYSVSEM:
        /* No per-task SysV-sem undo lists are tracked; both sides are
         * indistinguishable, mirroring tasks without sem undo state. */
        r = 0;
        break;
    case KCMP_EPOLL_TFD: {
        int efd = fdtable_get(t1, (int)idx1);
        if (efd < 0) {
            r = -EBADF;
            break;
        }
        vfile_t *evf = vfs_get_file_ref(efd);
        if (!evf) {
            r = -EBADF;
            break;
        }
        uint64_t ident = 0;
        int tfd = fdtable_get(t2, (int)idx2);
        if (tfd >= 0) {
            vfile_t *tvf = vfs_get_file_ref(tfd);
            if (tvf) {
                ident = tvf->identity;
                vfs_put_file_ref(tfd, tvf);
            }
        } else {
            r = -EBADF;
            vfs_put_file_ref(efd, evf);
            break;
        }
        int contains = epoll_slot_contains_file(t1, evf, ident);
        vfs_put_file_ref(efd, evf);
        if (contains < 0) {
            r = contains;
            break;
        }
        r = contains ? 0 : 1;
        break;
    }
    default:
        r = -EINVAL;
        break;
    }

out:
    proc_put(t1);
    proc_put(t2);
    return r;
}

int64_t sys_readahead(int fd, long off, size_t count)
{
    if (off < 0)
        return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return -EBADF;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf)
        return -EBADF;
    int r = page_cache_readahead(vf, (uint64_t)off, count);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}

/* cachestat(2): struct cachestat_range { u64 off, len; } selects the file
 * window; struct cachestat is five u64 counters copied out wholesale. */
int64_t sys_cachestat(int fd, const void *cstat_range, void *cstat,
                      unsigned flags)
{
    if (flags)
        return -EINVAL;
    if (!cstat_range || !cstat)
        return -EFAULT;

    uint64_t range[2];
    if (copy_from_user(range, cstat_range, sizeof(range)) < 0)
        return -EFAULT;
    uint64_t off = range[0];
    size_t len = (size_t)range[1];

    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return -EBADF;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf)
        return -EBADF;

    size_t resident = 0, dirty = 0;
    page_cache_file_range_stats(vf, off, len, &resident, &dirty);
    vfs_put_file_ref((int)gfd, vf);

    /* nr_writeback/evicted/recently_evicted stay zero: writeback here is
     * synchronous under page_cache_mark_dirty ownership and eviction
     * counters are not tracked per vnode. */
    uint64_t cs[5] = {
        resident >> PAGE_SIZE_BITS,
        dirty >> PAGE_SIZE_BITS,
        0,
        0,
        0,
    };
    return copy_to_user(cstat, cs, sizeof(cs)) < 0 ? -EFAULT : 0;
}

int64_t sys_lookup_dcookie(uint64_t cookie, char *buf, size_t len)
{
    if (!buf)
        return -EFAULT;

    char *path = NULL;
    int idx = dcookie_resolve(cookie, &path);
    if (idx < 0)
        return -EINVAL;

    size_t total = strlen(path) + 1;
    int64_t r;
    if (len < total) {
        r = (int64_t)total;   /* buffer too small: report required size */
    } else {
        r = copy_to_user(buf, path, total) < 0 ? -EFAULT : (int64_t)total;
    }
    dcookie_release(idx);
    return r;
}

int64_t sys_quotactl(int cmd, const char *special, int id, void *addr)
{
    (void)cmd;
    (void)id;
    (void)addr;
    if (!special)
        return -EFAULT;
    return -EOPNOTSUPP;
}

int64_t sys_quotactl_fd(int fd, int cmd, int id, void *addr)
{
    (void)fd;
    (void)cmd;
    (void)id;
    (void)addr;
    return -EOPNOTSUPP;
}

/* remap_file_pages(2): rebind [start, start+size) of an existing shared
 * file mapping to file offset pgoff*PAGE_SIZE. */
int64_t sys_remap_file_pages(uint64_t start, size_t size, int prot_unused,
                             uint64_t pgoff, int flags_unused)
{
    (void)prot_unused;
    (void)flags_unused;
    if (((start | (uint64_t)size) & (PAGE_SIZE - 1)))
        return -EINVAL;
    if (size == 0)
        return 0;
    if (pgoff > (~(uint64_t)0 >> PAGE_SIZE_BITS))
        return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;

    int gfd = -1;
    int prot = PROT_READ;
    uint64_t old_off = 0;

    uint64_t mm_flags = spin_lock_irqsave(&t->mm->lock);
    vm_area_t *vma = mm_find_vma(t->mm, (vaddr_t)start);
    if (!vma || vma->start > start ||
        (uint64_t)(vma->end - start) < size) {
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        return -EINVAL;
    }
    if ((vma->vm_flags & (VM_FILE | VM_SHARED)) != (VM_FILE | VM_SHARED)) {
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        return -EINVAL;
    }
    if (mm_mseal_range_is_sealed_locked(t->mm, (vaddr_t)start, size)) {
        spin_unlock_irqrestore(&t->mm->lock, mm_flags);
        return -EPERM;
    }
    gfd = vma->file_fd;
    prot = mm_pte_flags_to_prot(vma->pte_flags);
    old_off = vma->file_offset;
    spin_unlock_irqrestore(&t->mm->lock, mm_flags);

    if (gfd < 0)
        return -EINVAL;

    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;

    int64_t r = mm_munmap(t->mm, (vaddr_t)start, size);
    if (r < 0) {
        vfs_put_file_ref(gfd, vf);
        return r;
    }

    uint64_t new_off = pgoff << PAGE_SIZE_BITS;
    vaddr_t mapped = proc_mmap((vaddr_t)start, size, prot,
                               MAP_SHARED | MAP_FIXED, gfd, (long)new_off);
    if (mm_addr_is_error(mapped)) {
        int64_t err = mm_addr_error(mapped);
        proc_mmap((vaddr_t)start, size, prot, MAP_SHARED | MAP_FIXED, gfd,
                  (long)old_off);
        vfs_put_file_ref(gfd, vf);
        return err;
    }
    vfs_put_file_ref(gfd, vf);
    return 0;
}

int64_t sys_memfd_secret(unsigned flags)
{
    return memfd_secret_file((int)flags);
}

/* rseq(2): register a restartable-sequence area; the kernel publishes
 * cpu_id/cpu_id_start/node_id/mm_cid into it at registration and on every
 * dispatch. */
int64_t sys_rseq(void *rseq, uint32_t rseq_len, int flags, uint32_t sig)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    if (flags & ~(1 | 2 | 4)) /* RSEQ_FLAG_UNREGISTER | RSEQ_FLAG_CPU_ID_ALLOCATION */
        return -EINVAL;
    if (rseq_len < 32)
        return -EINVAL;

    if (flags & 1) { /* RSEQ_FLAG_UNREGISTER */
        t->rseq_area = 0;
        t->rseq_sig = 0;
        return 0;
    }
    if (!rseq)
        return -EFAULT;
    t->rseq_area = (uintptr_t)rseq;
    t->rseq_sig = sig;
    t->rseq_flags = (uint32_t)flags;
    rseq_publish(t);
    return 0;
}

/* ---- ioprio / pkey (scheduler compat layer) ---- */
#include "proc/sched_compat.h"

int64_t sys_ioprio_set(int which, int who, int ioprio)
{
    return ioprio_set_task(proc_current(), which, who, ioprio);
}

int64_t sys_ioprio_get(int which, int who)
{
    return ioprio_get_task(proc_current(), which, who);
}

int64_t sys_pkey_alloc(unsigned flags, uint64_t init_access_rights)
{
    (void)init_access_rights;
    return pkey_alloc(proc_current(), flags);
}

int64_t sys_pkey_free(int pkey)
{
    return pkey_free(proc_current(), pkey);
}

int64_t sys_pkey_mprotect(uint64_t addr, size_t len, int prot, int pkey)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    if (!pkey_valid(t, pkey))
        return -EINVAL;
    return sys_mprotect(addr, len, prot);
}

int64_t sys_mlock2(uint64_t addr, size_t len, int flags)
{
    if (flags)
        return -EINVAL;
    return sys_mlock(addr, len);
}

int64_t sys_mseal(uint64_t addr, size_t len, unsigned flags)
{
    if (flags)
        return -EINVAL;
    if (addr & (PAGE_SIZE - 1))
        return -EINVAL;
    if (len == 0)
        return 0;
    if (addr + len < addr)
        return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;

    /* mseal(2): seal [addr, addr+len) against later layout/protection
     * changes.  The core MM owns the VM_SEALED flag and enforces it in every
     * mutation path; this ABI wrapper only validates the wire format. */
    return mm_mseal(t->mm, addr, len);
}

int64_t sys_seccomp(unsigned op, unsigned flags, const void *uargs)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;

    switch (op) {
    case SECCOMP_SET_MODE_STRICT:
        if (flags)
            return -EINVAL;
        return seccomp_set_strict(t);
    case SECCOMP_SET_MODE_FILTER:
        /* TSYNC/LOG/NEW_LISTENER flags need cross-task or fd plumbing that
         * does not exist yet; plain filters install with flags == 0. */
        if (flags)
            return -EINVAL;
        if (!uargs)
            return -EFAULT;
        return seccomp_install_filter(t, uargs);
    case SECCOMP_GET_ACTION_AVAIL: {
        uint32_t action = 0;
        if (!uargs || copy_from_user(&action, uargs, sizeof(action)) < 0)
            return -EFAULT;
        uint32_t cls = action & SECCOMP_RET_ACTION_FULL;
        int supported = (cls == action) &&
                        (cls == SECCOMP_RET_KILL_PROCESS ||
                         cls == SECCOMP_RET_KILL_THREAD ||
                         cls == SECCOMP_RET_TRAP || cls == SECCOMP_RET_ERRNO ||
                         cls == SECCOMP_RET_TRACE || cls == SECCOMP_RET_LOG ||
                         cls == SECCOMP_RET_ALLOW);
        uint32_t answer = supported ? 1u : 0u;
        return copy_to_user((void *)uargs, &answer,
                            sizeof(answer)) < 0 ? -EFAULT : 0;
    }
    case SECCOMP_GET_NOTIF_SIZES: {
        struct {
            uint16_t notif;
            uint16_t response;
            uint16_t data;
        } sizes = {
            .notif = (uint16_t)sizeof(seccomp_notif_wire_t),
            .response = (uint16_t)sizeof(seccomp_notif_resp_wire_t),
            .data = (uint16_t)sizeof(seccomp_data_t),
        };
        if (!uargs)
            return -EFAULT;
        return copy_to_user((void *)uargs, &sizes,
                            sizeof(sizes)) < 0 ? -EFAULT : 0;
    }
    default:
        return -EINVAL;
    }
}

int64_t sys_kexec_load(uint64_t entry, uint64_t nr_segments,
                       const void *segments, uint64_t flags)
{
    (void)entry;
    (void)nr_segments;
    (void)segments;
    (void)flags;
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    if (!proc_has_cap(t, CAP_SYS_BOOT) && t->cred.euid != 0)
        return -EPERM;
    /* kexec reboots into a new kernel; A20OS does not support image
     * handoff, so refuse rather than fake success. */
    return -EINVAL;
}

int64_t sys_kexec_file_load(int kernel_fd, int initrd_fd,
                            uint64_t cmdline_len, const void *cmdline,
                            uint64_t flags)
{
    (void)kernel_fd;
    (void)initrd_fd;
    (void)cmdline_len;
    (void)cmdline;
    (void)flags;
    return -EINVAL;
}

int64_t sys_nfsservctl(int cmd, const void *arg, void *res)
{
    (void)cmd;
    (void)arg;
    (void)res;
    /* nfsservctl(2) was removed in Linux 4.19; the register number still
     * exists in the syscall table and -ENOSYS is the correct Linux 4.19+
     * behavior for it. */
    return -ENOSYS;
}

/* map_shadow_stack(2): guarded CET shadow-stack allocation.  Only built on
 * x86_64; every other architecture keeps Linux's arch-correct -ENOSYS. */
int64_t sys_map_shadow_stack(uint64_t addr_hint, uint64_t size, unsigned flags)
{
#ifndef CONFIG_X86_64
    (void)addr_hint;
    (void)size;
    (void)flags;
    return -ENOSYS;
#else
    const unsigned SHADOW_STACK_SET_TOKEN = 0x1;
    if (flags & ~SHADOW_STACK_SET_TOKEN)
        return -EINVAL;
    if (size == 0)
        return -EINVAL;
    if (addr_hint & (PAGE_SIZE - 1))
        return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;

    uint64_t len = ROUND_UP(size, (uint64_t)PAGE_SIZE);
    uint64_t total = len + PAGE_SIZE;
    if (total <= len)
        return -ENOMEM;

    vaddr_t base = mm_find_gap(t->mm, (vaddr_t)addr_hint, total);
    if (mm_addr_is_error(base))
        return mm_addr_error(base);

    vaddr_t guard = proc_mmap(base, PAGE_SIZE, PROT_NONE,
                              MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mm_addr_is_error(guard))
        return mm_addr_error(guard);

    vaddr_t stk = proc_mmap(base + PAGE_SIZE, len, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (mm_addr_is_error(stk)) {
        mm_munmap(t->mm, base, PAGE_SIZE);
        return mm_addr_error(stk);
    }

    uint64_t top = (uint64_t)stk + len;
    if (flags & SHADOW_STACK_SET_TOKEN) {
        uint64_t tok_va = top - 8;
        handle_demand_fault(t, tok_va);
        paddr_t pa = pt_translate(t->mm->pgdir, (vaddr_t)tok_va);
        if (pa) {
            volatile uint64_t *tok =
                (volatile uint64_t *)(PAGE_OFFSET + pa +
                                      (tok_va & (PAGE_SIZE - 1)));
            *tok = tok_va | 0x1ull;   /* self-referential, not-busy */
            return (int64_t)(tok_va & ~0x7ull);
        }
    }
    return (int64_t)(top & ~0x7ull);
#endif
}
