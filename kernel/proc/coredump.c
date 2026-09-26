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
#include <stdint.h>
#include "proc/proc.h"
#include "proc/signal.h"
#include "proc/debug_regs.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "core/fcntl.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "sys/usercopy.h"

/* 64-bit ELF only.  32-bit ports keep the no-op stub (documented boundary). */
#if defined(CONFIG_RISCV64) || defined(CONFIG_X86_64) ||   \
    defined(CONFIG_AARCH64) || defined(CONFIG_LOONGARCH64) || \
    defined(CONFIG_PPC64LE)
#define COREDUMP_SUPPORTED 1
#endif

#define CORE_PATTERN_MAX 128
#define CORE_VMA_SNAPSHOT_MAX 1024

#define CORE_EM_X86_64    62
#define CORE_EM_AARCH64   183
#define CORE_EM_RISCV     243
#define CORE_EM_LOONGARCH 258
#define CORE_EM_PPC64     21

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

#ifdef COREDUMP_SUPPORTED

/* ---- ELF64 wire structures ---- */

typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} core_ehdr_t;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} core_phdr_t;

_Static_assert(sizeof(core_ehdr_t) == 64, "ELF64 ehdr size");
_Static_assert(sizeof(core_phdr_t) == 56, "ELF64 phdr size");

/* Linux elf_prstatus (64-bit): siginfo + cursig + ids + 4 timevals +
 * gregset[32] + fpvalid. */
typedef struct {
    int32_t  si_signo;
    int32_t  si_code;
    int32_t  si_errno;
    int16_t  pr_cursig;
    uint16_t _pad0;
    uint64_t pr_sigpend;
    uint64_t pr_sighold;
    int32_t  pr_pid;
    int32_t  pr_ppid;
    int32_t  pr_pgrp;
    int32_t  pr_sid;
    uint64_t pr_utime[2];
    uint64_t pr_stime[2];
    uint64_t pr_cutime[2];
    uint64_t pr_cstime[2];
    uint64_t pr_reg[32];
    int32_t  pr_fpvalid;
} core_prstatus_t;

/* Linux elf_prpsinfo (128 bytes). */
typedef struct {
    uint8_t  pr_state;
    uint8_t  pr_sname;
    uint8_t  pr_zomb;
    uint8_t  pr_nice;
    uint32_t pr_flag;
    uint32_t pr_uid;
    uint32_t pr_gid;
    int32_t  pr_pid;
    int32_t  pr_ppid;
    int32_t  pr_pgrp;
    int32_t  pr_sid;
    char     pr_fname[16];
    char     pr_psargs[80];
} core_prpsinfo_t;

_Static_assert(sizeof(core_prpsinfo_t) == 128, "elf_prpsinfo size");

/* riscv64 FP regset: 32 double FP registers + fcsr. */
typedef struct {
    uint64_t f[32];
    uint32_t fcsr;
} core_fpregset_t;

typedef struct {
    vaddr_t  start;
    vaddr_t  end;
    uint64_t flags;
} cd_vma_t;

static uint16_t coredump_e_machine(void)
{
#if defined(CONFIG_RISCV64)
    return CORE_EM_RISCV;
#elif defined(CONFIG_X86_64)
    return CORE_EM_X86_64;
#elif defined(CONFIG_AARCH64)
    return CORE_EM_AARCH64;
#elif defined(CONFIG_LOONGARCH64)
    return CORE_EM_LOONGARCH;
#elif defined(CONFIG_PPC64LE)
    return CORE_EM_PPC64;
#else
    return 0;
#endif
}

/*
 * Translate the generic debug register file (filled by the shared
 * arch_ptrace_export_regs(), the same export ptrace uses) into the ELF
 * gregset array.  Exact for riscv64 (pc followed by x1..x31, which is both
 * Linux's user_regs_struct order and this kernel's regs[] order); the generic
 * order is a best-effort fallback for the other 64-bit ports.
 */
static void coredump_fill_gregset(uint64_t out[32],
                                  const proc_debug_regs_t *regs)
{
    out[0] = regs->pc;
    for (int i = 1; i < 32; i++)
        out[i] = regs->regs[i];
}

static uint32_t coredump_vma_pflags(uint64_t vm_flags)
{
    uint32_t f = 0;
    if (vm_flags & VM_READ)
        f |= CORE_PF_R;
    if (vm_flags & VM_WRITE)
        f |= CORE_PF_W;
    if (vm_flags & VM_EXEC)
        f |= CORE_PF_X;
    return f;
}

static uint64_t coredump_align_up(uint64_t v, uint64_t a)
{
    return (v + a - 1) & ~(a - 1);
}

/* ---- core_pattern expansion (%p %e %s %u %g %t %%) ---- */

static void coredump_expand_pattern(const char *pattern, task_t *t, int sig,
                                    char *out, size_t outsz)
{
    size_t o = 0;
    for (size_t i = 0; pattern[i] && o + 1 < outsz; i++) {
        if (pattern[i] != '%' || !pattern[i + 1]) {
            out[o++] = pattern[i];
            continue;
        }
        char tmp[32];
        const char *ins = NULL;
        char c = pattern[++i];
        switch (c) {
        case '%': ins = "%"; break;
        case 'p':
            snprintf(tmp, sizeof(tmp), "%d", t->tgid > 0 ? t->tgid : t->pid);
            ins = tmp;
            break;
        case 'e':
            snprintf(tmp, sizeof(tmp), "%s", t->name);
            ins = tmp;
            break;
        case 's':
            snprintf(tmp, sizeof(tmp), "%d", sig);
            ins = tmp;
            break;
        case 'u':
            snprintf(tmp, sizeof(tmp), "%d", t->cred.uid);
            ins = tmp;
            break;
        case 'g':
            snprintf(tmp, sizeof(tmp), "%d", t->cred.gid);
            ins = tmp;
            break;
        case 't':
            snprintf(tmp, sizeof(tmp), "%llu",
                     (unsigned long long)(timer_get_ticks() / TICKS_PER_SEC));
            ins = tmp;
            break;
        default:
            break; /* unsupported placeholder: drop, per documented subset */
        }
        if (ins) {
            for (const char *p = ins; *p && o + 1 < outsz; p++)
                out[o++] = *p;
        }
    }
    out[o] = '\0';
}

/* ---- note blob assembly ---- */

typedef struct {
    char  *buf;
    size_t cap;
    size_t len;
    int    overflow;
} cd_blob_t;

static void cd_blob_put(cd_blob_t *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        b->overflow = 1;
        return;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
}

static void cd_blob_pad4(cd_blob_t *b)
{
    static const char zero[3] = {0, 0, 0};
    size_t pad = (4 - (b->len & 3)) & 3;
    if (pad)
        cd_blob_put(b, zero, pad);
}

static void cd_blob_note(cd_blob_t *b, const char *name, uint32_t type,
                         const void *desc, size_t descsz)
{
    uint32_t hdr[3];
    hdr[0] = (uint32_t)strlen(name) + 1; /* namesz includes the NUL */
    hdr[1] = (uint32_t)descsz;
    hdr[2] = type;
    cd_blob_put(b, hdr, sizeof(hdr));
    cd_blob_put(b, name, hdr[0]);
    cd_blob_pad4(b);
    cd_blob_put(b, desc, descsz);
    cd_blob_pad4(b);
}

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
static int coredump_snapshot_vmas(mm_struct_t *mm, cd_vma_t **out)
{
    *out = NULL;
    uint64_t flags = spin_lock_irqsave(&mm->lock);
    int count = 0;
    for (vm_area_t *vma = mm->mmap; vma; vma = vma->next) {
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

    cd_vma_t *snap = (cd_vma_t *)kmalloc(sizeof(cd_vma_t) * (size_t)count);
    if (!snap)
        return -ENOMEM;

    /* Refill under the lock; a concurrent mmap by a sibling thread can only
     * change the tail beyond our capacity, which we truncate. */
    flags = spin_lock_irqsave(&mm->lock);
    int n = 0;
    for (vm_area_t *vma = mm->mmap; vma && n < count; vma = vma->next) {
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

static void coredump_build_notes(task_t *t, int sig, trap_context_t *ctx,
                                 cd_blob_t *blob)
{
    proc_debug_regs_t regs;
    memset(&regs, 0, sizeof(regs));
    if (ctx)
        arch_ptrace_export_regs(ctx, &regs);

    core_prstatus_t prs;
    memset(&prs, 0, sizeof(prs));
    prs.si_signo = sig;
    prs.si_code = SI_KERNEL;
    prs.pr_cursig = (int16_t)sig;
    prs.pr_pid = t->pid;
    prs.pr_ppid = t->ppid;
    prs.pr_pgrp = t->pgid;
    prs.pr_sid = t->sid;
    coredump_fill_gregset(prs.pr_reg, &regs);
    prs.pr_fpvalid = 1;
    cd_blob_note(blob, "CORE", CORE_NT_PRSTATUS, &prs, sizeof(prs));

    core_fpregset_t fprs;
    memset(&fprs, 0, sizeof(fprs));
    for (int i = 0; i < 32; i++)
        fprs.f[i] = regs.fp[i];
    fprs.fcsr = (uint32_t)regs.fcsr;
    cd_blob_note(blob, "CORE", CORE_NT_FPREGSET, &fprs, sizeof(fprs));

    core_prpsinfo_t psi;
    memset(&psi, 0, sizeof(psi));
    psi.pr_state = 0;
    psi.pr_sname = 'R';
    psi.pr_uid = (uint32_t)t->cred.uid;
    psi.pr_gid = (uint32_t)t->cred.gid;
    psi.pr_pid = t->pid;
    psi.pr_ppid = t->ppid;
    psi.pr_pgrp = t->pgid;
    psi.pr_sid = t->sid;
    strncpy(psi.pr_fname, t->name, sizeof(psi.pr_fname) - 1);
    strncpy(psi.pr_psargs, t->name, sizeof(psi.pr_psargs) - 1);
    cd_blob_note(blob, "CORE", CORE_NT_PRPSINFO, &psi, sizeof(psi));
}

void coredump_on_fatal_signal(int sig, trap_context_t *ctx)
{
    task_t *t = proc_current();
    if (!t || !t->mm || !t->pgdir)
        return;

    /* RLIMIT_CORE == 0 suppresses the dump (Linux semantics). */
    uint64_t rlim = signal_task_rlim_core(t);
    if (rlim == 0)
        return;

    char pattern[CORE_PATTERN_MAX];
    uint64_t pflags = spin_lock_irqsave(&g_core_pattern_lock);
    strncpy(pattern, g_core_pattern, sizeof(pattern) - 1);
    pattern[sizeof(pattern) - 1] = '\0';
    spin_unlock_irqrestore(&g_core_pattern_lock, pflags);

    if (pattern[0] == '|') {
        kwarn("coredump: pid=%d skipped, core_pattern pipe mode unsupported\n",
              t->pid);
        return;
    }

    cd_vma_t *vmas = NULL;
    int nvma = coredump_snapshot_vmas(t->mm, &vmas);
    if (nvma < 0)
        return;

    cd_blob_t notes;
    notes.cap = 2048;
    notes.len = 0;
    notes.overflow = 0;
    notes.buf = (char *)kmalloc(notes.cap);
    if (!notes.buf) {
        kfree(vmas);
        return;
    }
    coredump_build_notes(t, sig, ctx, &notes);
    if (notes.overflow) {
        kfree(notes.buf);
        kfree(vmas);
        return;
    }

    /* Layout: ehdr, phdrs, notes, then page-aligned PT_LOAD data.  Segment
     * data is clamped so the total file stays within a finite RLIMIT_CORE. */
    uint16_t phnum = (uint16_t)(1 + nvma);
    uint64_t notes_off = sizeof(core_ehdr_t) + (uint64_t)phnum * sizeof(core_phdr_t);
    uint64_t data_off = coredump_align_up(notes_off + notes.len, PAGE_SIZE);
    uint64_t limit = rlim;

    core_phdr_t *phdrs =
        (core_phdr_t *)kmalloc((size_t)phnum * sizeof(core_phdr_t));
    if (!phdrs) {
        kfree(notes.buf);
        kfree(vmas);
        return;
    }
    memset(phdrs, 0, (size_t)phnum * sizeof(core_phdr_t));
    phdrs[0].p_type = CORE_PT_NOTE;
    phdrs[0].p_offset = notes_off;
    phdrs[0].p_filesz = notes.len;
    phdrs[0].p_memsz = notes.len;
    phdrs[0].p_align = 4;

    uint64_t off = data_off;
    for (int i = 0; i < nvma; i++) {
        uint64_t size = (uint64_t)(vmas[i].end - vmas[i].start);
        uint64_t filesz = size;
        if (limit != UINT64_MAX && off + filesz > limit)
            filesz = off < limit ? limit - off : 0;
        core_phdr_t *ph = &phdrs[1 + i];
        ph->p_type = CORE_PT_LOAD;
        ph->p_flags = coredump_vma_pflags(vmas[i].flags);
        ph->p_offset = off;
        ph->p_vaddr = vmas[i].start;
        ph->p_filesz = filesz;
        ph->p_memsz = size;
        ph->p_align = PAGE_SIZE;
        off += filesz;
    }

    char path[MAX_PATH_LEN];
    coredump_expand_pattern(pattern, t, sig, path, sizeof(path));
    if (!path[0])
        goto out;

    int fd = vfs_open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        kwarn("coredump: pid=%d open '%s' failed (%d)\n", t->pid, path, fd);
        goto out;
    }

    core_ehdr_t eh;
    memset(&eh, 0, sizeof(eh));
    eh.e_ident[0] = 0x7f;
    eh.e_ident[1] = 'E';
    eh.e_ident[2] = 'L';
    eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2; /* ELFCLASS64 */
    eh.e_ident[5] = 1; /* little-endian */
    eh.e_ident[6] = 1; /* EV_CURRENT */
    eh.e_type = CORE_ET_CORE;
    eh.e_machine = coredump_e_machine();
    eh.e_version = 1;
    eh.e_phoff = sizeof(core_ehdr_t);
    eh.e_ehsize = sizeof(core_ehdr_t);
    eh.e_phentsize = sizeof(core_phdr_t);
    eh.e_phnum = phnum;

    int rc = cd_write_at(fd, 0, &eh, sizeof(eh));
    if (rc == 0)
        rc = cd_write_at(fd, sizeof(core_ehdr_t), phdrs,
                         (size_t)phnum * sizeof(core_phdr_t));
    if (rc == 0)
        rc = cd_write_at(fd, notes_off, notes.buf, notes.len);

    if (rc == 0) {
        char *page = (char *)kmalloc(PAGE_SIZE);
        if (!page)
            rc = -ENOMEM;
        for (int i = 0; i < nvma && rc == 0 && page; i++) {
            uint64_t filesz = phdrs[1 + i].p_filesz;
            uint64_t base = phdrs[1 + i].p_offset;
            if (vfs_lseek(fd, (long)base, SEEK_SET) < 0) {
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
    kfree(phdrs);
    kfree(notes.buf);
    kfree(vmas);
}

#endif /* COREDUMP_SUPPORTED */
