/*
 * A20OS — ELF core dump generation for fatal signals.
 *
 * Runs in the crashing task's own context (signal.c fatal-default hook,
 * before proc_exit_group()): the task's mm is still attached and current, so
 * user memory is read with copy_from_user(), which faults in demand pages and
 * fails gracefully on non-present or unreadable ones (those pages are emitted
 * as zeros, matching the "may be absent" semantics of a core snapshot).
 *
 * Concurrency: the VMA list is snapshotted into a kernel array under
 * mm->lock; all VFS writes happen after the lock is dropped (vfs_write may
 * block).  Sibling threads of the thread group keep running until
 * proc_exit_group() kills them after this hook returns, so the dump is a
 * best-effort snapshot of a live address space, like Linux.
 *
 * The core file goes through the regular VFS write path (vfs_open /
 * vfs_lseek / vfs_write), like kernel/proc/acct.c.  A relative core_pattern
 * resolves against the crashing task's cwd (vfs_open uses proc_current()).
 *
 * Format: ELF64 ET_CORE with one PT_NOTE (NT_PRSTATUS + NT_FPREGSET +
 * NT_PRPSINFO) and one PT_LOAD per dumped VMA.  See the header for the
 * documented boundaries (no |pipe mode, 64-bit only, placeholder subset).
 */

#include "proc/coredump.h"
#include "proc/coredump_layout.h"
#include <stdint.h>
#include "proc/proc.h"
#include "proc/signal.h"
#include "proc/debug_regs.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "core/arch.h"
#include "core/fcntl.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "sys/usercopy.h"

/* 64-bit ELF only.  32-bit ports keep the no-op stub (documented boundary).
 *
 * The gate is the same fact the ELF loader already gates on (elf_machine_supported
 * in kernel/mm/elf.c), so a core dump can never describe a machine the loader
 * would reject.  Spelling the architectures out here instead would be a second
 * list to keep in step with Makefile's ELF_CLASS_*, which is where the
 * architecture facts actually live. */
#define COREDUMP_SUPPORTED (ARCH_ELF_CLASS == 2)

#ifndef ARCH_COREDUMP_EXPORT_REGS
#define ARCH_COREDUMP_EXPORT_REGS arch_ptrace_export_regs
#endif

#define CORE_PATTERN_MAX 128
#define CORE_VMA_SNAPSHOT_MAX 1024
#define CORE_NOTES_MAX 2048

#define CORE_ET_CORE      4
#define CORE_PT_LOAD      1
#define CORE_PT_NOTE      4
#define CORE_PF_X         1
#define CORE_PF_W         2
#define CORE_PF_R         4

#define CORE_NT_PRSTATUS  1
#define CORE_NT_FPREGSET  2
#define CORE_NT_PRPSINFO  3

static spinlock_t g_core_pattern_lock = SPINLOCK_INIT;
static char g_core_pattern[CORE_PATTERN_MAX] = "core";

int coredump_set_pattern(const char *buf, size_t len)
{
    if (!buf)
        return -EINVAL;
    while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
        len--;
    if (len == 0 || len >= CORE_PATTERN_MAX)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_core_pattern_lock);
    memcpy(g_core_pattern, buf, len);
    g_core_pattern[len] = '\0';
    spin_unlock_irqrestore(&g_core_pattern_lock, flags);
    return 0;
}

int coredump_get_pattern(char *buf, size_t bufsz)
{
    if (!buf || bufsz < 2)
        return -EINVAL;
    uint64_t flags = spin_lock_irqsave(&g_core_pattern_lock);
    int n = snprintf(buf, bufsz, "%s\n", g_core_pattern);
    spin_unlock_irqrestore(&g_core_pattern_lock, flags);
    return n < 0 ? n : 0;
}

#if COREDUMP_SUPPORTED

/* ---- VFS write helpers ---- */

static int cd_write_full(int fd, const void *buf, size_t len)
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

static int cd_write_at(int fd, uint64_t off, const void *buf, size_t len)
{
    if (vfs_lseek(fd, (long)off, SEEK_SET) < 0)
        return -EIO;
    return cd_write_full(fd, buf, len);
}


/* Snapshot the mm VMA list under mm->lock; callers free with kfree. */
static int coredump_snapshot_vmas(mm_struct_t *mm, coredump_vma_input_t **out)
{
    *out = NULL;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int count = 0;
    for (mm_seg_t *vma = mm->mmap; vma; vma = vma->next) {
        if (vma->vm_flags & VM_GUARD)
            continue;
        if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)))
            continue;
        count++;
        if (count >= CORE_VMA_SNAPSHOT_MAX)
            break;
    }
    spin_unlock_irqrestore(&mm->lock, flags);
    if (count == 0)
        return 0;

    /* Refill under the lock; a concurrent mmap by a sibling thread can only
     * change the tail beyond our capacity, which we truncate. */
    coredump_vma_input_t *snap =
        (coredump_vma_input_t *)kmalloc(sizeof(*snap) * (size_t)count);
    if (!snap)
        return -ENOMEM;

    flags = spin_lock_irqsave(&mm->lock);
    int n = 0;
    for (mm_seg_t *vma = mm->mmap; vma && n < count; vma = vma->next) {
        if (vma->vm_flags & VM_GUARD)
            continue;
        if (!(vma->vm_flags & (VM_READ | VM_WRITE | VM_EXEC)))
            continue;
        snap[n].start = vma->start;
        snap[n].end = vma->end;
        snap[n].flags = vma->vm_flags;
        n++;
    }
    spin_unlock_irqrestore(&mm->lock, flags);
    if (n == 0) {
        kfree(snap);
        return 0;
    }
    *out = snap;
    return n;
}

static void coredump_fill_note_input(task_t *t, int sig, trap_context_t *ctx,
                                     coredump_note_input_t *in)
{
    memset(in, 0, sizeof(*in));
    proc_debug_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    if (ctx)
        ARCH_COREDUMP_EXPORT_REGS(ctx, &regs);

    in->sig = sig;
    in->pid = t->pid;
    in->ppid = t->ppid;
    in->pgid = t->pgid;
    in->sid = t->sid;
    in->uid = (uint32_t)t->cred.uid;
    in->gid = (uint32_t)t->cred.gid;
    /* Preserve the entire generic register file, including rax/x0. The
     * serializer chooses the ELF-machine wire order; pc is not a GPR slot. */
    in->pc = regs.pc;
    in->sp = regs.sp;
    in->status = regs.status;
    in->orig_syscall = regs.orig_syscall;
    for (int i = 0; i < 32; i++)
        in->regs[i] = regs.regs[i];
    for (int i = 0; i < 32; i++)
        in->fp[i] = regs.fp[i];
    in->fcsr = (uint64_t)regs.fcsr;
    strncpy(in->name, t->name, sizeof(in->name) - 1);
}

int coredump_on_fatal_signal(int sig, trap_context_t *ctx)
{
    task_t *t = proc_current();
    if (!t || !t->mm || !t->pgdir || mm_dumpable(t->mm) != 1)
        return 0;

    /* RLIMIT_CORE == 0 suppresses the dump (Linux semantics). */
    uint64_t rlim = signal_task_rlim_core(t);
    if (rlim == 0)
        return 0;

    char pattern[CORE_PATTERN_MAX];
    uint64_t pflags = spin_lock_irqsave(&g_core_pattern_lock);
    strncpy(pattern, g_core_pattern, sizeof(pattern) - 1);
    pattern[sizeof(pattern) - 1] = '\0';
    spin_unlock_irqrestore(&g_core_pattern_lock, pflags);

    if (pattern[0] == '|') {
        kwarn("coredump: pid=%d skipped, core_pattern pipe mode unsupported\n",
              t->pid);
        return 0;
    }

    coredump_vma_input_t *vmas = NULL;
    int nvma = coredump_snapshot_vmas(t->mm, &vmas);
    if (nvma < 0)
        return 0;

    /* Layout: ehdr, phdrs, notes, then page-aligned PT_LOAD data.  Rust owns
     * the shape; this file owns the writes. */
    uint16_t phnum = (uint16_t)(1 + nvma);
    char *notes = (char *)kmalloc(CORE_NOTES_MAX);
    char *ehdr = (char *)kmalloc(COREDUMP_EHDR_SIZE);
    char *phdrs = (char *)kmalloc((size_t)phnum * COREDUMP_PHDR_SIZE);
    coredump_segment_t *segs =
        (coredump_segment_t *)kmalloc(nvma > 0 ? sizeof(*segs) * (size_t)nvma : 1);
    char path[MAX_PATH_LEN];
    int rc = 0;
    if (!notes || !ehdr || !phdrs || !segs) {
        kfree(notes);
        kfree(ehdr);
        kfree(phdrs);
        kfree(segs);
        kfree(vmas);
        return 0;
    }

    coredump_note_input_t nin;
    uint64_t notes_len = 0;
    coredump_fill_note_input(t, sig, ctx, &nin);
    rc = a20_coredump_notes(&nin, ARCH_ELF_MACHINE, notes, CORE_NOTES_MAX, &notes_len);
    if (rc < 0)
        goto out;

    coredump_layout_result_t lr;
    rc = a20_coredump_layout(vmas, (uint32_t)nvma, notes_len, rlim, PAGE_SIZE,
                             ARCH_ELF_MACHINE, ehdr, phdrs,
                             (size_t)phnum * COREDUMP_PHDR_SIZE, segs,
                             (size_t)nvma, &lr);
    if (rc < 0)
        goto out;

    path[0] = '\0';
    rc = a20_coredump_expand_pattern(pattern, t->name, t->pid, t->tgid,
                                     t->cred.uid, t->cred.gid, sig,
                                     timer_get_ticks() / TICKS_PER_SEC,
                                     path, sizeof(path));
    if (rc < 0 || !path[0])
        goto out;

    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        kwarn("coredump: pid=%d open '%s' failed (%d)\n", t->pid, path, fd);
        goto out;
    }

    rc = cd_write_at(fd, 0, ehdr, COREDUMP_EHDR_SIZE);
    if (rc == 0)
        rc = cd_write_at(fd, COREDUMP_EHDR_SIZE, phdrs,
                         (size_t)phnum * COREDUMP_PHDR_SIZE);
    if (rc == 0)
        rc = cd_write_at(fd, lr.notes_off, notes, (size_t)notes_len);

    if (rc == 0) {
        char *page = (char *)kmalloc(PAGE_SIZE);
        if (!page)
            rc = -ENOMEM;
        for (int i = 0; i < nvma && rc == 0 && page; i++) {
            uint64_t filesz = segs[i].filesz;
            if (vfs_lseek(fd, (long)segs[i].offset, SEEK_SET) < 0) {
                rc = -EIO;
                break;
            }
            for (uint64_t d = 0; d < filesz; d += PAGE_SIZE) {
                uint64_t va = (uint64_t)vmas[i].start + d;
                size_t chunk = filesz - d < PAGE_SIZE ? (size_t)(filesz - d)
                                                      : PAGE_SIZE;
                memset(page, 0, chunk);
                /* Non-present/unreadable pages stay zero-filled. */
                (void)copy_from_user(page, (const void *)(uintptr_t)va, chunk);
                rc = cd_write_full(fd, page, chunk);
                if (rc < 0)
                    break;
            }
        }
        if (page)
            kfree(page);
    }

    if (rc == 0)
        (void)vfs_fsync(fd);
    vfs_close(fd);
    if (rc == 0)
        kinfo("coredump: pid=%d sig=%d -> %s\n", t->pid, sig, path);
    else
        kwarn("coredump: pid=%d write failed (%d), partial '%s' kept\n",
              t->pid, rc, path);

out:
    kfree(segs);
    kfree(phdrs);
    kfree(ehdr);
    kfree(notes);
    kfree(vmas);
    return rc == 0;
}

#endif /* COREDUMP_SUPPORTED */
