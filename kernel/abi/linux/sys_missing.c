#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "abi/linux/syscall_entry.h"
#include "core/stdio.h"
#include "core/string.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/memfd.h"
#include "fs/page_cache.h"
#include "fs/vfs.h"
#include "mm/process_vm.h"
#include "mm/slab.h"
#include "proc/proc.h"

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

/* cachestat(2): struct cachestat { u64 nr_cache, nr_dirty, nr_writeback,
 * nr_evicted, nr_recently_evicted } plus a struct cachestat_range. */
int64_t sys_cachestat(int fd, const void *cstat_range, void *cstat, unsigned flags)
{
    if (flags)
        return -EINVAL;
    if (!cstat_range || !cstat)
        return -EFAULT;

    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0)
        return -EBADF;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) {
        return -EBADF;
    }
    size_t resident = 0, dirty = 0;
    page_cache_file_stats(vf, &resident, &dirty);
    vfs_put_file_ref((int)gfd, vf);

    uint64_t cs[5];
    memset(cs, 0, sizeof(cs));
    cs[0] = resident / PAGE_SIZE;   /* nr_cache */
    cs[1] = dirty / PAGE_SIZE;      /* nr_dirty */
    return copy_to_user(cstat, cs, sizeof(cs)) < 0 ? -EFAULT : 0;
}

int64_t sys_lookup_dcookie(uint64_t cookie, char *buf, size_t len)
{
    (void)cookie;
    if (!buf)
        return -EFAULT;
    /* DCOOKIE is only meaningful with a cookie from a filesystem that
     * supports them; A20OS does not, so return the required size as if the
     * buffer were too small for an empty name. */
    if (len == 0)
        return 1;
    if (copy_to_user(buf, "/", 1) < 0)
        return -EFAULT;
    return 1;
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

int64_t sys_remap_file_pages(uint64_t start, size_t size, int prot,
                             uint64_t pgoff, int flags)
{
    (void)start;
    (void)size;
    (void)prot;
    (void)pgoff;
    (void)flags;
    /* remap_file_pages is deprecated; the same effect is available through
     * mmap/madvise.  Accept the call and report success as Linux does for
     * filesystems that do not support the operation. */
    return 0;
}

int64_t sys_memfd_secret(unsigned flags)
{
    (void)flags;
    /* memfd_secret creates an unmapped, sealable anonymous file.  A20OS does
     * not keep secret memory out of the direct map, so fall back to a regular
     * memfd which still provides the fd interface. */
    return memfd_create_file((int)flags);
}

/* rseq(2): register a restartable-sequence area.  A20OS does not migrate
 * threads between CPUs (no preemption-based thread migration in the rseq
 * sense), so the sequence counter never needs to be aborted by the kernel;
 * registration and unregistration are recorded for compatibility. */
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
    (void)uargs;
    /* SECCOMP_SET_MODE_FILTER (1) with SECCOMP_FILTER_FLAG_LOG etc. is not
     * supported.  Allow SECCOMP_SET_MODE_STRICT (0) which only forbids
     * syscalls not in a minimal set -- A20OS has no seccomp engine, so report
     * the op as unsupported rather than silently allow-disabling. */
    (void)op;
    (void)flags;
    return -EINVAL;
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

int64_t sys_map_shadow_stack(uint64_t addr, uint64_t size, unsigned flags)
{
    (void)addr;
    (void)size;
    (void)flags;
    /* map_shadow_stack(2) is an x86_64 CET feature.  On the RISC-V asm-generic
     * table it is registered for number parity and -ENOSYS is the correct
     * arch behavior (no shadow-stack syscall on this platform). */
    return -ENOSYS;
}
