/*
 * Checkpoint image restore (restore-as-exec): the CALLING task's address
 * space and fd table are replaced by the checkpointed ones and the task
 * resumes in user mode at the checkpointed instruction.  On any failure the
 * caller keeps its old address space; its fd table may already have been
 * rebuilt, so this is meant for one-shot supervisor tools (svcctl restore).
 * See kernel/include/proc/checkpoint.h for the scope contract.
 */

#include "proc/checkpoint.h"
#include "proc/ckpt_image.h"

#include "core/errno.h"
#include "core/fcntl.h"
#include "core/klog.h"
#include "core/mman.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/trap.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/elf.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/process_vm.h"
#include "mm/slab.h"
#include "mm/vdso.h"
#include "mm/vm.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"


#define CKPT_R_MAX_VMA 1024
#define CKPT_R_MAX_FD  256

static int ckpt_read_full(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t done = 0;
    while (done < len) {
        int n = vfs_read(fd, p + done, len - done);
        if (n <= 0)
            return n < 0 ? n : -EIO;
        done += (size_t)n;
    }
    return 0;
}

static int ckpt_read_at(int fd, uint64_t off, void *buf, size_t len)
{
    if (vfs_lseek(fd, (long)off, SEEK_SET) < 0)
        return -EIO;
    return ckpt_read_full(fd, buf, len);
}

static int ckpt_validate_vma(const a20_ckpt_vma_t *v, uint64_t file_size)
{
    return a20_ckpt_validate_vma(v->start, v->end, v->vm_flags,
                                 v->data_offset, file_size, USER_VA_LIMIT,
                                 PAGE_SIZE);
}

/* Populate one VMA: allocate a frame per page, fill it from the image and
 * map it, then publish the VMA record.  Mirrors elf.c's load recipe. */
static int ckpt_load_vma(int in_fd, mm_struct_t *m, pt_root_t *pgdir,
                         const a20_ckpt_vma_t *v)
{
    uint64_t vm = (v->vm_flags & (VM_READ | VM_WRITE | VM_EXEC | VM_STACK)) |
                  VM_ANON;
    pte_t pf = mm_vm_flags_to_pte_flags(vm);
    for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
        void *frame = frame_alloc();
        if (!frame)
            return -ENOMEM;
        int r = ckpt_read_at(in_fd, v->data_offset + (va - v->start), frame,
                             PAGE_SIZE);
        if (r == 0)
            r = pt_map(pgdir, (vaddr_t)va,
                       (paddr_t)(uintptr_t)va_to_pa(frame), pf);
        if (r < 0) {
            frame_free(frame);
            return r;
        }
        mm_rss_add(m, 1);
    }
    mm_seg_t *vma = mm_seg_new();
    if (!vma)
        return -ENOMEM;
    vma->start = (vaddr_t)v->start;
    vma->end = (vaddr_t)v->end;
    vma->vm_flags = vm;
    vma->pte_flags = pf;
    vma->file = NULL;
    uint64_t flags = spin_lock_irqsave(&m->lock);
    mm_insert_vma(m, vma);
    m->total_vm += (v->end - v->start) / PAGE_SIZE;
    spin_unlock_irqrestore(&m->lock, flags);
    mm_vma_flush_deferred(m);
    return 0;
}

static void ckpt_discard_mm(mm_struct_t *m, pt_root_t *pgdir)
{
    if (!m)
        return;
    /* Route the VMA list + page table through the elf teardown, which knows
     * how to reap mapped frames safely. */
    elf_load_info_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    tmp.pgdir = pgdir;
    tmp.mmap = m->mmap;
    elf_load_info_discard(&tmp);
    kfree(m);
}

typedef struct {
    int tmp_fd;
    int target_fd;
    int cloexec;
} ckpt_fd_map_t;

/* Open every recorded path first (nothing destructive yet), then sweep the
 * caller's table and move the opened files onto their recorded numbers. */
static int ckpt_rebuild_fds(task_t *t, const a20_ckpt_fd_t *recs, uint32_t nr,
                            ckpt_fd_map_t *maps, uint32_t *nr_maps)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < nr; i++) {
        if (recs[i].kind == A20_CKPT_FD_CLOSED)
            continue;
        if (recs[i].kind != A20_CKPT_FD_FILE || recs[i].fd < 0 ||
            recs[i].fd >= MAX_FILES)
            return -ENOEXEC;
        int oflags = recs[i].flags &
                     (O_ACCMODE | O_APPEND | O_NONBLOCK | O_NOFOLLOW);
        int tmp = vfs_open(recs[i].path, oflags, 0);
        if (tmp < 0)
            return tmp;
        if (!(oflags & O_APPEND))
            (void)vfs_lseek(tmp, (long)recs[i].offset, SEEK_SET);
        maps[n].tmp_fd = tmp;
        maps[n].target_fd = recs[i].fd;
        maps[n].cloexec = recs[i].cloexec;
        n++;
    }
    /* Point of no return for the fd table: drop every slot except the
     * freshly opened temporaries, then rename those onto their targets. */
    for (int fd = 0; fd < MAX_FILES; fd++) {
        int keep = 0;
        for (uint32_t i = 0; i < n; i++)
            if (maps[i].tmp_fd == fd)
                keep = 1;
        if (!keep)
            (void)fdtable_close(t, fd);
    }
    for (uint32_t i = 0; i < n; i++) {
        if (maps[i].tmp_fd != maps[i].target_fd) {
            int r = fdtable_dup_to(t, maps[i].tmp_fd, maps[i].target_fd, 0);
            if (r < 0)
                return r;
            (void)fdtable_close(t, maps[i].tmp_fd);
        }
        (void)fdtable_set_cloexec(t, maps[i].target_fd, maps[i].cloexec);
    }
    *nr_maps = n;
    return 0;
}

int proc_ckpt_restore(int in_fd)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    if (t->tg_leader != t || t->tg_next)
        return -EOPNOTSUPP; /* single-threaded images only */

    a20_ckpt_header_t hdr;
    int rc = ckpt_read_full(in_fd, &hdr, sizeof(hdr));
    if (rc < 0)
        return rc;
    rc = a20_ckpt_validate_header(hdr.magic, hdr.version, hdr.arch,
                                  hdr.regset_size, hdr.nr_vma, hdr.nr_fd,
                                  ARCH_ELF_MACHINE);
    if (rc < 0)
        return rc;
    /* Never let a checkpoint smuggle in another identity: the restored task
     * keeps the RESTORER's credentials; the recorded uid is only a guard. */
    if ((uint32_t)t->cred.uid != hdr.uid && t->cred.uid != 0)
        return -EPERM;

    long file_size = vfs_lseek(in_fd, 0, SEEK_END);
    if (file_size < 0)
        return -EIO;

    a20_ckpt_vma_t *vmas =
        (a20_ckpt_vma_t *)kmalloc(hdr.nr_vma * sizeof(*vmas));
    a20_ckpt_fd_t *fds =
        (a20_ckpt_fd_t *)kcalloc(hdr.nr_fd ? hdr.nr_fd : 1, sizeof(*fds));
    ckpt_fd_map_t *maps =
        (ckpt_fd_map_t *)kcalloc(hdr.nr_fd ? hdr.nr_fd : 1, sizeof(*maps));
    if (!vmas || !fds || !maps) {
        rc = -ENOMEM;
        goto out_free;
    }
    /* Read the tables explicitly at their recorded offsets. */
    rc = ckpt_read_at(in_fd, sizeof(hdr), vmas, hdr.nr_vma * sizeof(*vmas));
    if (rc < 0)
        goto out_free;
    rc = ckpt_read_at(in_fd, sizeof(hdr) + hdr.nr_vma * sizeof(*vmas), fds,
                      hdr.nr_fd * sizeof(*fds));
    if (rc < 0)
        goto out_free;
    for (uint32_t i = 0; i < hdr.nr_vma; i++) {
        rc = ckpt_validate_vma(&vmas[i], (uint64_t)file_size);
        if (rc < 0)
            goto out_free;
    }

    /* Build the replacement address space (fallible, caller untouched). */
    pt_root_t *pgdir = pt_create();
    if (!pgdir) {
        rc = -ENOMEM;
        goto out_free;
    }
    pt_map_kernel(pgdir);
    mm_struct_t *m = (mm_struct_t *)kcalloc(1, sizeof(*m));
    if (!m) {
        pt_destroy_user(pgdir);
        rc = -ENOMEM;
        goto out_free;
    }
    m->pgdir = pgdir;
    m->dumpable = 1;
    m->brk = m->start_brk = (vaddr_t)hdr.brk;
    m->mmap_base = mm_aslr_mmap_base();
    mm_rss_set(m, 0);
    spin_init(&m->lock);
    spin_set_debug(&m->lock, "mm", m);
    mutex_init(&m->tlb_lock);
    m->tlb_holds = NULL;
    refcount_set(&m->refcount, 1);
    m->mmap = NULL;
    m->has_vdso = 0;
    mm_arch_context_init(m);

    vaddr_t top = 0;
    for (uint32_t i = 0; i < hdr.nr_vma; i++)
        if ((vaddr_t)vmas[i].end > top)
            top = (vaddr_t)vmas[i].end;
    m->stack_top = top;
    m->stack_bottom = top;

    for (uint32_t i = 0; i < hdr.nr_vma; i++) {
        rc = ckpt_load_vma(in_fd, m, pgdir, &vmas[i]);
        if (rc < 0) {
            ckpt_discard_mm(m, pgdir);
            goto out_free;
        }
    }
    /* VM_PFNMAP windows (vDSO/vvar) are excluded from the image; re-map the
     * current kernel's vDSO so VAs and contents line up again. */
    if (hdr.abi_mode == 0 && vdso_map_image(pgdir, &m->mmap) == 0)
        m->has_vdso = 1;

    uint32_t nr_maps = 0;
    rc = ckpt_rebuild_fds(t, fds, hdr.nr_fd, maps, &nr_maps);
    if (rc < 0) {
        ckpt_discard_mm(m, pgdir);
        goto out_free;
    }

    /* Commit: swap the address space and reload the full register file. */
    uint64_t user_as = mm_address_space_token(m);
    uint64_t iflags = spin_lock_irqsave(&t->park_lock);
    mm_struct_t *old_mm = t->mm;
    pt_root_t *old_pgdir = t->pgdir;
    t->mm = m;
    t->pgdir = pgdir;
    spin_unlock_irqrestore(&t->park_lock, iflags);
    t->entry = (uintptr_t)hdr.regs.pc;
    t->ustack = (uintptr_t)hdr.regs.sp;
    strncpy(t->exec_path, hdr.exec_path, MAX_PATH_LEN - 1);
    t->exec_path[MAX_PATH_LEN - 1] = '\0';
    proc_set_name(t, hdr.name);
    t->pgid = hdr.pgid;
    t->sid = hdr.sid;
    signal_exec_reset(t);

    trap_context_t *trap = t->trap_ctx;
    uint64_t ks_top =
        arch_trap_ctx_get_kernel_stack(trap, (uint64_t)(uintptr_t)trap);
    memset(trap, 0, sizeof(*trap));
    /* The token goes in AFTER the register import: on rv64 the frame's x[0]
     * slot is the address-space-token scratch, not a GPR, and the Rust-owned
     * import (kernel/arch/riscv64/include/trap_frame.h ->
     * kernel/proc/ckpt_image.rs) cannot clobber it by construction.  The C
     * version here once let import_regs() overwrite the token with the
     * checkpointed task's stale one, resuming the restored task on the dead
     * task's page table. */
    arch_ptrace_import_regs(trap, &hdr.regs);
    TRAP_CTX_KScratch0(trap) = user_as;
    trap->kernel_tp = (uint64_t)(uintptr_t)t;
    arch_trap_ctx_set_kernel_stack(trap, ks_top);

    unsigned cpu = cpu_current_id();
    mm_context_enter(m, cpu);
    arch_switch_addr_space_token(user_as);
    if (old_mm && old_mm != m)
        mm_context_leave(old_mm, cpu);
    if (old_mm)
        mm_destroy(old_mm);
    else if (old_pgdir && old_pgdir != proc_kernel_pgdir_shared())
        pt_destroy_user(old_pgdir);
    arch_fence_i();
    kinfo("[CKPT] restored '%s' pid=%d pc=0x%lx vmas=%u fds=%u\n", hdr.name,
          t->pid, (unsigned long)hdr.regs.pc, hdr.nr_vma, hdr.nr_fd);
    rc = 0;

out_free:
    kfree(vmas);
    kfree(fds);
    kfree(maps);
    return rc;
}

int proc_ckpt_restore_path(const char *path)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;
    int rc = proc_ckpt_restore(fd);
    /* On success the caller's table was rebuilt and fd is gone; close only
     * on failure, where the table is untouched by the rebuild. */
    if (rc < 0)
        vfs_close(fd);
    return rc;
}
