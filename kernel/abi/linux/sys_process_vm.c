#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

#include "mm/process_vm.h"
#include "proc/proc.h"

/*
 * process_vm_readv(2)/process_vm_writev(2) and process_madvise(2).
 *
 * process_vm_* copy iovec lists between the caller and a target task using
 * the target's page table directly (kernel/mm/process_vm.c).  process_madvise
 * applies an madvise hint to a target task's address range.
 */

struct linux_iovec {
    void *iov_base;
    size_t iov_len;
};

#define PROCESS_VM_MAX_IOV 32

/* Stack-sized iovec storage; Linux caps iovcnt at UIO_MAXIOV (1024) but the
 * syscall layer limits it to a value safe for a kernel stack buffer. */
#define PROCESS_VM_IOV_STACK 32

static int process_vm_check_target(task_t *self, task_t *target)
{
    if (!target)
        return -ESRCH;
    if (target->state == PROC_ZOMBIE)
        return -ESRCH;
    /* A process always has access to its own memory. */
    if (self && self != target &&
        !proc_has_cap(self, CAP_SYS_PTRACE) &&
        !proc_task_may_access(self, target))
        return -EPERM;
    if (!target->mm)
        return -EPERM;
    return 0;
}

static int64_t process_vm_io_common(int pid, const void *riov,
                                    unsigned long riovcnt,
                                    const void *wiov, unsigned long wiovcnt,
                                    unsigned long flags, int to_target)
{
    (void)flags;
    if (riovcnt > PROCESS_VM_MAX_IOV || wiovcnt > PROCESS_VM_MAX_IOV)
        return -EINVAL;

    /* Copy iovec arrays into stack buffers (the scratch buffer is reused by
     * the per-chunk copy below, so it must not hold the arrays). */
    struct linux_iovec rlocal[PROCESS_VM_IOV_STACK];
    struct linux_iovec wlocal[PROCESS_VM_IOV_STACK];
    if (riovcnt) {
        if (copy_from_user(rlocal, riov,
                           riovcnt * sizeof(struct linux_iovec)) < 0)
            return -EFAULT;
    }
    if (wiovcnt) {
        if (copy_from_user(wlocal, wiov,
                           wiovcnt * sizeof(struct linux_iovec)) < 0)
            return -EFAULT;
    }

    task_t *self = proc_current();
    task_t *target = proc_find_get(pid);
    int perr = process_vm_check_target(self, target);
    if (perr < 0) {
        if (target) proc_put(target);
        return perr;
    }

    long total = 0;
    unsigned long ri = 0, wi = 0;
    size_t roff = 0, woff = 0;
    while (ri < riovcnt && wi < wiovcnt) {
        size_t rlen = rlocal[ri].iov_len - roff;
        size_t wlen = wlocal[wi].iov_len - woff;
        if (rlen == 0 && wlen == 0) { ri++; wi++; roff = woff = 0; continue; }
        if (rlen == 0) { ri++; roff = 0; continue; }
        if (wlen == 0) { wi++; woff = 0; continue; }

        size_t n = rlen < wlen ? rlen : wlen;
        /* Chunked copy through a small kernel scratch buffer. */
        size_t done = 0;
        while (done < n) {
            size_t chunk = n - done;
            if (chunk > 65536)
                chunk = 65536;
            char *kbuf = proc_scratch_buffer(chunk);
            if (!kbuf) {
                if (target) proc_put(target);
                return total > 0 ? total : -ENOMEM;
            }
            long r;
            if (to_target) {
                /* Copy caller user buffer -> kernel, then kernel -> target. */
                if (copy_from_user(kbuf,
                                   (const char *)wlocal[wi].iov_base + woff + done,
                                   chunk) < 0) {
                    if (target) proc_put(target);
                    return total > 0 ? total : -EFAULT;
                }
                r = process_vm_write_kernel(
                    target, (char *)rlocal[ri].iov_base + roff + done,
                    kbuf, chunk);
            } else {
                /* Copy target -> kernel, then kernel -> caller user buffer. */
                r = process_vm_read_kernel(
                    target, (const char *)rlocal[ri].iov_base + roff + done,
                    kbuf, chunk);
                if (r > 0 && copy_to_user(
                        (char *)wlocal[wi].iov_base + woff + done,
                        kbuf, (size_t)r) < 0) {
                    if (target) proc_put(target);
                    return total > 0 ? total : -EFAULT;
                }
            }
            if (r <= 0) {
                if (target) proc_put(target);
                return total > 0 ? total : (r < 0 ? r : -EFAULT);
            }
            done += (size_t)r;
        }
        total += (long)n;
        roff += n; woff += n;
        if (roff == rlocal[ri].iov_len) { ri++; roff = 0; }
        if (woff == wlocal[wi].iov_len) { wi++; woff = 0; }
    }
    if (target) proc_put(target);
    return total;
}

int64_t sys_process_vm_readv(int pid, const void *lvec, unsigned long liovcnt,
                             const void *rvec, unsigned long riovcnt,
                             unsigned long flags)
{
    /* Linux: lvec = local (caller) buffers, rvec = remote (target) buffers.
     * We copy from the remote to the local. */
    return process_vm_io_common(pid, rvec, riovcnt, lvec, liovcnt, flags, 0);
}

int64_t sys_process_vm_writev(int pid, const void *lvec, unsigned long liovcnt,
                              const void *rvec, unsigned long riovcnt,
                              unsigned long flags)
{
    return process_vm_io_common(pid, rvec, riovcnt, lvec, liovcnt, flags, 1);
}

int64_t sys_process_madvise(int pid, const void *iov, unsigned long iovcnt,
                            int advice, unsigned long flags)
{
    (void)advice;
    (void)flags;
    if (!iov || iovcnt > PROCESS_VM_MAX_IOV)
        return -EINVAL;

    struct linux_iovec local[PROCESS_VM_IOV_STACK];
    if (copy_from_user(local, iov, iovcnt * sizeof(struct linux_iovec)) < 0)
        return -EFAULT;

    task_t *self = proc_current();
    task_t *target = proc_find_get(pid);
    int perr = process_vm_check_target(self, target);
    if (perr < 0) {
        if (target) proc_put(target);
        return perr;
    }
    /* A20OS's madvise implementation is a compatibility approximation that
     * validates the range and hint but performs no physical action for most
     * hints; apply the same semantics per range. */
    long total = 0;
    for (unsigned long i = 0; i < iovcnt; i++) {
        long r = sys_madvise((uint64_t)(uintptr_t)local[i].iov_base,
                             local[i].iov_len, advice);
        if (r < 0) {
            if (target) proc_put(target);
            return total > 0 ? total : r;
        }
        total += (long)local[i].iov_len;
    }
    if (target) proc_put(target);
    return total;
}

int64_t sys_process_mrelease(int pidfd, unsigned flags)
{
    (void)flags;
    if (pidfd < 0)
        return -EINVAL;
    /* The target is identified by a pidfd; resolve it and, if it is a zombie
     * whose memory is being released, drain the mm.  A20OS reclaims mm on
     * exit automatically, so this is a no-op success for a valid pidfd. */
    int64_t gfd = fdtable_get_current(pidfd);
    if (gfd < 0)
        return -EBADF;
    return 0;
}
