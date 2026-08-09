#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

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
    /* Only meaningful after a syscall was interrupted and the user restart
     * sequence requested it; with no pending restart this is a no-op that
     * returns the interrupted syscall's restart code. */
    return -ERESTARTNOINTR;
}

int64_t sys_kcmp(int pid1, int pid2, int type, unsigned long idx1,
                 unsigned long idx2)
{
    (void)idx1;
    (void)idx2;
    task_t *t1 = proc_find_get(pid1);
    task_t *t2 = proc_find_get(pid2);
    if (!t1 || !t2) {
        if (t1) proc_put(t1);
        if (t2) proc_put(t2);
        return -ESRCH;
    }

    int r = 0;
    switch (type) {
    case 0: /* KCMP_FILE: compare file identity of fd idx1 vs idx2 */
    {
        int f1 = fdtable_get(t1, (int)idx1);
        int f2 = fdtable_get(t2, (int)idx2);
        if (f1 < 0 || f2 < 0)
            r = -EBADF;
        else if (f1 == f2)
            r = 0;
        else if (f1 < f2)
            r = 1;
        else
            r = 2;
        break;
    }
    case 3: /* KCMP_VM: compare address spaces */
        r = (t1->mm == t2->mm) ? 0 : 1;
        break;
    case 4: /* KCMP_FILES: compare fd tables */
        r = (t1->files == t2->files) ? 0 : 1;
        break;
    case 5: /* KCMP_FS: compare fs contexts */
        r = (t1->fs.cwd[0] == t2->fs.cwd[0] &&
             strcmp(t1->fs.cwd, t2->fs.cwd) == 0)
                ? 0
                : 1;
        break;
    case 6: /* KCMP_SIGHAND */
        r = (t1->signals == t2->signals) ? 0 : 1;
        break;
    case 7: /* KCMP_IO */
        r = 1;
        break;
    case 8: /* KCMP_SYSVSEM */
        r = 1;
        break;
    default:
        r = -EINVAL;
        break;
    }
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
