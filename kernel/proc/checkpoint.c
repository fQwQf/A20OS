/*
 * Service-level checkpoint writer: freeze a single-threaded user task through
 * the ptrace machinery and serialize its register file, address space and fd
 * table into an A20CKPT1 image.  See kernel/include/proc/checkpoint.h for the
 * scope contract.
 */

#include "proc/checkpoint.h"

#include "core/errno.h"
#include "core/fcntl.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/process_vm.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "proc/debug.h"
#include "proc/pidns.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"

#define CKPT_FREEZE_TIMEOUT_TICKS (5ULL * TICKS_PER_SEC)
#define CKPT_MAX_VMA  256
#define CKPT_MAX_FD   64
#define CKPT_MAX_SIZE (512ULL << 20) /* refuse absurd images */

static int ckpt_write_full(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < len) {
        int n = vfs_write(fd, p + done, len - done);
        if (n <= 0)
            return n < 0 ? n : -EIO;
        done += (size_t)n;
    }
    return 0;
}

static task_t *ckpt_freeze(int pid, int *err)
{
    task_t *cur = proc_current();
    *err = -EPERM;
    if (pid <= 0 || pid == cur->pid) {
        *err = -EINVAL;
        return NULL;
    }
    int rc = proc_debug_attach(pid);
    if (rc < 0) {
        *err = rc;
        return NULL;
    }
    rc = proc_debug_interrupt(pid);
    if (rc < 0) {
        proc_debug_detach(pid, 0);
        *err = rc;
        return NULL;
    }
    task_t *t = proc_find_get_user(pid);
    if (!t) {
        proc_debug_detach(pid, 0);
        *err = -ESRCH;
        return NULL;
    }
    uint64_t start = timer_get_ticks();
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&t->park_lock);
        int stopped = t->ptrace_stop_active && t->state == PROC_STOPPED;
        spin_unlock_irqrestore(&t->park_lock, flags);
        if (stopped)
            return t;
        if (timer_get_ticks() - start > CKPT_FREEZE_TIMEOUT_TICKS) {
            proc_put(t);
            proc_debug_detach(pid, 0);
            *err = -ETIMEDOUT;
            return NULL;
        }
        arch_cpu_relax();
    }
}

static void ckpt_thaw(int pid, task_t *t)
{
    proc_debug_resume(pid, 0, PT_DEBUG_RESUME_CONT);
    proc_debug_detach(pid, 0);
    proc_put(t);
}

static int ckpt_snapshot_vmas(task_t *t, a20_ckpt_vma_t **out, uint32_t *nr_out)
{
    *out = NULL;
    *nr_out = 0;
    /* The target is frozen at a ptrace stop and we hold a task reference, so
     * its mm is stable for the duration. */
    mm_struct_t *mm = t->mm;
    if (!mm || !mm->pgdir)
        return -ESRCH;
    a20_ckpt_vma_t *snap =
        (a20_ckpt_vma_t *)kmalloc(sizeof(*snap) * CKPT_MAX_VMA);
    if (!snap)
        return -ENOMEM;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    uint32_t n = 0;
    int rc = 0;
    for (mm_seg_t *vma = mm->mmap; vma; vma = vma->next) {
        if (vma->vm_flags & VM_GUARD)
            continue;
        if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)))
            continue;
        if (vma->vm_flags & VM_PFNMAP)
            continue; /* vDSO/vvar/device frames: re-mapped fresh at restore */
        if (vma->vm_flags & VM_SHARED) {
            kinfo("[CKPT] unsupported shared vma 0x%lx-0x%lx flags=0x%lx\n",
                  (unsigned long)vma->start, (unsigned long)vma->end,
                  (unsigned long)vma->vm_flags);
            rc = -EOPNOTSUPP; /* shared mappings have cross-process state */
            break;
        }
        if (n == CKPT_MAX_VMA) {
            rc = -E2BIG;
            break;
        }
        snap[n].start = vma->start;
        snap[n].end = vma->end;
        snap[n].vm_flags = vma->vm_flags;
        snap[n].data_offset = 0; /* filled by the writer */
        n++;
    }
    spin_unlock_irqrestore(&mm->lock, flags);
    if (rc < 0) {
        kfree(snap);
        return rc;
    }
    if (!n) {
        kfree(snap);
        return -ENOEXEC;
    }
    *out = snap;
    *nr_out = n;
    return 0;
}

static int ckpt_snapshot_fds(task_t *t, a20_ckpt_fd_t **out, uint32_t *nr_out,
                             uint32_t flags)
{
    *out = NULL;
    *nr_out = 0;
    files_struct_t *files = t->files;
    if (!files)
        return -ESRCH;
    a20_ckpt_fd_t *recs =
        (a20_ckpt_fd_t *)kcalloc(CKPT_MAX_FD, sizeof(*recs));
    vfile_t **held =
        (vfile_t **)kcalloc(CKPT_MAX_FD, sizeof(*held));
    if (!recs || !held) {
        kfree(recs);
        kfree(held);
        return -ENOMEM;
    }
    uint32_t n = 0;
    int rc = 0;
    /* Take references under the spinlock only; path/offset reads and any
     * sleeping happen after the unlock. */
    uint64_t irqf = spin_lock_irqsave(&files->lock);
    for (int fd = 0; fd < MAX_FILES && n < CKPT_MAX_FD; fd++) {
        vfile_t *vf = files->fd[fd];
        if (!vf)
            continue;
        vfile_get(vf);
        held[n] = vf;
        recs[n].fd = fd;
        recs[n].cloexec = files->cloexec[fd] ? 1 : 0;
        n++;
    }
    spin_unlock_irqrestore(&files->lock, irqf);
    uint32_t count = n;
    n = 0;
    for (uint32_t i = 0; i < count; i++) {
        vfile_t *vf = held[i];
        a20_ckpt_fd_t *r = &recs[n];
        if (vf->vnode && vf->path[0] == '/') {
            r->kind = A20_CKPT_FD_FILE;
            r->flags = vf->flags;
            mutex_lock(&vf->offset_lock);
            r->offset = vf->offset;
            mutex_unlock(&vf->offset_lock);
            strncpy(r->path, vf->path, sizeof(r->path) - 1);
            n++;
        } else if (flags & A20_CKPT_F_SKIP_UNSUPPORTED_FDS) {
            r->kind = A20_CKPT_FD_CLOSED;
            n++;
        } else {
            /* Sockets, pipes, channels and anonymous devices cannot be
             * carried across a kernel switch; fail loudly. */
            kinfo("[CKPT] unsupported fd=%d path='%s' vnode=%p\n",
                  recs[i].fd, vf->path, (void *)vf->vnode);
            rc = -EOPNOTSUPP;
            break;
        }
    }
    for (uint32_t i = 0; i < count; i++)
        vfs_put_file(held[i]);
    kfree(held);
    if (rc < 0) {
        kfree(recs);
        return rc;
    }
    *out = recs;
    *nr_out = n;
    return 0;
}

static int ckpt_write_vma_data(int out_fd, task_t *t,
                               const a20_ckpt_vma_t *vma, uint64_t *offset)
{
    uint8_t *page = (uint8_t *)kmalloc(PAGE_SIZE);
    if (!page)
        return -ENOMEM;
    int rc = 0;
    for (uint64_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
        long n = process_vm_read_kernel(t, (const void *)(uintptr_t)va, page,
                                        PAGE_SIZE);
        if (n != (long)PAGE_SIZE)
            memset(page, 0, PAGE_SIZE); /* sparse, never-touched page */
        rc = ckpt_write_full(out_fd, page, PAGE_SIZE);
        if (rc < 0)
            break;
        *offset += PAGE_SIZE;
        if (*offset > CKPT_MAX_SIZE) {
            rc = -E2BIG;
            break;
        }
    }
    kfree(page);
    return rc;
}

int proc_ckpt_checkpoint(int pid, int out_fd, uint32_t flags)
{
    if (flags & ~(A20_CKPT_F_RESUME | A20_CKPT_F_SKIP_UNSUPPORTED_FDS))
        return -EINVAL;
    task_t *t;
    int rc = 0;
    {
        int ferr = 0;
        t = ckpt_freeze(pid, &ferr);
        if (!t)
            return ferr;
    }
    proc_debug_regs_t regs;
    a20_ckpt_vma_t *vmas = NULL;
    a20_ckpt_fd_t *fds = NULL;
    uint32_t nr_vma = 0, nr_fd = 0;

    rc = proc_debug_getregs(pid, &regs);
    if (rc < 0)
        goto out;
    /* v1 scope: single-threaded tasks only. */
    if (t->tg_leader != t || t->tg_next) {
        rc = -EOPNOTSUPP;
        goto out;
    }
    rc = ckpt_snapshot_vmas(t, &vmas, &nr_vma);
    if (rc < 0)
        goto out;
    rc = ckpt_snapshot_fds(t, &fds, &nr_fd, flags);
    if (rc < 0)
        goto out;

    uint64_t off = sizeof(a20_ckpt_header_t) +
                   (uint64_t)nr_vma * sizeof(a20_ckpt_vma_t) +
                   (uint64_t)nr_fd * sizeof(a20_ckpt_fd_t);
    for (uint32_t i = 0; i < nr_vma; i++) {
        vmas[i].data_offset = off;
        off += vmas[i].end - vmas[i].start;
    }

    a20_ckpt_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    hdr.magic = A20_CKPT_MAGIC;
    hdr.version = A20_CKPT_VERSION;
    hdr.arch = ARCH_ELF_MACHINE;
    hdr.abi_mode = t->abi_mode;
    hdr.flags = flags;
    hdr.nr_vma = nr_vma;
    hdr.nr_fd = nr_fd;
    hdr.uid = (uint32_t)t->cred.uid;
    hdr.gid = (uint32_t)t->cred.gid;
    hdr.pgid = t->pgid;
    hdr.sid = t->sid;
    hdr.regset_size = sizeof(proc_debug_regs_t);
    hdr.brk = t->mm ? t->mm->brk : 0;
    strncpy(hdr.name, t->name, sizeof(hdr.name) - 1);
    strncpy(hdr.exec_path, t->exec_path, sizeof(hdr.exec_path) - 1);
    hdr.regs = regs;

    rc = ckpt_write_full(out_fd, &hdr, sizeof(hdr));
    if (rc < 0)
        goto out;
    rc = ckpt_write_full(out_fd, vmas, nr_vma * sizeof(*vmas));
    if (rc < 0)
        goto out;
    rc = ckpt_write_full(out_fd, fds, nr_fd * sizeof(*fds));
    if (rc < 0)
        goto out;

    for (uint32_t i = 0; i < nr_vma && rc == 0; i++) {
        uint64_t cur = vmas[i].data_offset;
        rc = ckpt_write_vma_data(out_fd, t, &vmas[i], &cur);
    }

out:
    if (rc == 0 && (flags & A20_CKPT_F_RESUME))
        ckpt_thaw(pid, t);
    else if (rc < 0) {
        proc_debug_detach(pid, 0);
        proc_put(t);
    } else {
        /* No RESUME: leave the target frozen and attached; the caller is
         * expected to proceed to kexec (or thaw it explicitly). */
        proc_put(t);
    }
    kfree(vmas);
    kfree(fds);
    return rc;
}

int proc_ckpt_checkpoint_path(int pid, const char *path, uint32_t flags)
{
    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0)
        return fd;
    int rc = proc_ckpt_checkpoint(pid, fd, flags);
    vfs_close(fd);
    return rc;
}
