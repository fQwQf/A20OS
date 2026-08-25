#include "fs/io_uring.h"

#include "ipc/envelope.h"
#include "core/errno.h"
#include "core/string.h"
#include "fs/anonfd.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "ipc/eventfd.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

/*
 * io_uring subset: kernel-owned submission/completion rings.
 *
 * io_uring_setup() allocates two pages (a 4KiB submission queue of 64-byte
 * io_uring_sqe entries and a 4KiB completion queue of 16-byte io_uring_cqe
 * entries), maps them into the calling address space (the sysv_shm-style
 * pt_map + VMA pattern), and returns a context id through the anonymous
 * fd's private data.  io_uring_enter() copies user-written SQEs out of the
 * mapped submission queue, executes supported opcodes synchronously through
 * the VFS, and writes completion entries into the mapped completion queue.
 * io_uring_register() accepts the file-registration and eventfd options.
 *
 * The ring pages are kept referenced by the context and reclaimed when the
 * fd closes or the process exits (mm teardown frees the mapped frames).
 */

/* 64-byte Linux submission queue entry (fields we interpret). */
typedef struct io_uring_sqe {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t ioprio;
    int32_t  fd;
    union {
        uint64_t off;
        uint64_t addr2;
    } u1;
    union {
        uint64_t addr;
        uint64_t splice_off_in;
    } u2;
    uint32_t len;
    union {
        uint32_t rw_flags;
        uint32_t fsync_flags;
    } u3;
    uint64_t user_data;
    union {
        uint16_t buf_index;
        uint16_t buf_group;
    } u4;
    uint16_t personality;
    union {
        int32_t splice_fd_in;
        uint32_t file_index;
    } u5;
    uint64_t addr3;
    uint64_t __pad2[1];
} io_uring_sqe_t;

/* 16-byte completion queue entry. */
typedef struct io_uring_cqe {
    uint64_t user_data;
    int32_t  res;
    uint32_t flags;
} io_uring_cqe_t;

typedef struct io_uring_ring {
    pfn_t sq_pfn;
    pfn_t cq_pfn;
    uint64_t sq_va;             /* user address of the SQ page */
    uint64_t cq_va;             /* user address of the CQ page */
    unsigned nr_entries;
    unsigned sq_head;
    unsigned sq_tail;
    unsigned cq_head;
    unsigned cq_tail;
    spinlock_t lock;
    int eventfd_fd;             /* registered notification fd, -1 = none */
} io_uring_ring_t;

static io_uring_ring_t *g_rings[IORING_MAX_RINGS];
static spinlock_t g_ring_table_lock = SPINLOCK_INIT;

static io_uring_sqe_t *io_uring_sq_kaddr(io_uring_ring_t *ring)
{
    return (io_uring_sqe_t *)(PAGE_OFFSET + pfn_to_phys(ring->sq_pfn));
}

static io_uring_cqe_t *io_uring_cq_kaddr(io_uring_ring_t *ring)
{
    return (io_uring_cqe_t *)(PAGE_OFFSET + pfn_to_phys(ring->cq_pfn));
}

static int io_uring_close(vfile_t *vf)
{
    if (vf && vf->priv) {
        io_uring_ring_t *ring = vf->priv;
        /* The mapped pages are owned by the process's page table; unmap and
         * release the extra reference taken at map time. */
        task_t *t = proc_current();
        if (t && t->mm) {
            mm_munmap(t->mm, ring->sq_va, PAGE_SIZE);
            mm_munmap(t->mm, ring->cq_va, PAGE_SIZE);
        }
        frame_put(ring->sq_pfn);
        frame_put(ring->cq_pfn);
        kfree(ring);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_io_uring_ops = {
    .close = io_uring_close,
};

static int io_uring_map_page(mm_struct_t *mm, pfn_t pfn, uint64_t *va_out)
{
    uint64_t addr = mm_find_gap(mm, MMAP_BASE_ADDR, PAGE_SIZE);
    if (addr == 0)
        return -ENOMEM;
    paddr_t pa = pfn_to_phys(pfn);
    uint64_t flags = mm_vm_flags_to_pte_flags(VM_SHARED | VM_READ | VM_WRITE);
    if (pt_map(mm->pgdir, addr, pa, flags) < 0)
        return -ENOMEM;
    frame_get(pfn);

    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) {
        pt_unmap(mm->pgdir, addr);
        frame_put(pfn);
        return -ENOMEM;
    }
    vma->start = addr;
    vma->end = addr + PAGE_SIZE;
    vma->vm_flags = VM_SHARED | VM_READ | VM_WRITE;
    vma->pte_flags = flags;
    vma->file_fd = -1;
    uint64_t mm_flags = spin_lock_irqsave(&mm->lock);
    mm_insert_vma(mm, vma);
    mm->total_vm++;
    spin_unlock_irqrestore(&mm->lock, mm_flags);
    mm_vma_flush_deferred(mm);

    *va_out = addr;
    return 0;
}

int io_uring_create(unsigned entries)
{
    if (entries == 0 || entries > IORING_MAX_ENTRIES)
        return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;

    io_uring_ring_t *ring = kcalloc(1, sizeof(*ring));
    if (!ring)
        return -ENOMEM;
    ring->nr_entries = entries;
    spin_init(&ring->lock);

    ring->sq_pfn = pfa_alloc_page();
    if (ring->sq_pfn == PFN_NONE) {
        kfree(ring);
        return -ENOMEM;
    }
    memset(pfn_to_virt(ring->sq_pfn), 0, PAGE_SIZE);
    ring->cq_pfn = pfa_alloc_page();
    if (ring->cq_pfn == PFN_NONE) {
        frame_put(ring->sq_pfn);
        kfree(ring);
        return -ENOMEM;
    }
    memset(pfn_to_virt(ring->cq_pfn), 0, PAGE_SIZE);

    if (io_uring_map_page(t->mm, ring->sq_pfn, &ring->sq_va) < 0 ||
        io_uring_map_page(t->mm, ring->cq_pfn, &ring->cq_va) < 0) {
        frame_put(ring->sq_pfn);
        frame_put(ring->cq_pfn);
        kfree(ring);
        return -ENOMEM;
    }

    unsigned long flags = spin_lock_irqsave(&g_ring_table_lock);
    int slot = -1;
    for (int i = 0; i < IORING_MAX_RINGS; i++) {
        if (!g_rings[i]) {
            g_rings[i] = ring;
            slot = i;
            break;
        }
    }
    spin_unlock_irqrestore(&g_ring_table_lock, flags);
    if (slot < 0) {
        mm_munmap(t->mm, ring->sq_va, PAGE_SIZE);
        mm_munmap(t->mm, ring->cq_va, PAGE_SIZE);
        frame_put(ring->sq_pfn);
        frame_put(ring->cq_pfn);
        kfree(ring);
        return -EMFILE;
    }

    vfile_t *vf = vfile_alloc();
    if (!vf) {
        g_rings[slot] = NULL;
        mm_munmap(t->mm, ring->sq_va, PAGE_SIZE);
        mm_munmap(t->mm, ring->cq_va, PAGE_SIZE);
        frame_put(ring->sq_pfn);
        frame_put(ring->cq_pfn);
        kfree(ring);
        return -ENOMEM;
    }
    vfile_ref_init(vf, 1);
    vf->ops = &g_io_uring_ops;
    vf->priv = ring;
    return anonfd_install_vfile(vf, O_CLOEXEC);
}

static long io_uring_execute_sqe(io_uring_sqe_t *sqe, io_uring_cqe_t *cqe)
{
    memset(cqe, 0, sizeof(*cqe));
    cqe->user_data = sqe->user_data;

    switch (sqe->opcode) {
    case IORING_OP_NOP:
        cqe->res = 0;
        return 0;
    case IORING_OP_FSYNC: {
        int64_t gfd = fdtable_get_current(sqe->fd);
        if (gfd < 0) {
            cqe->res = (int32_t)gfd;
            return gfd;
        }
        if (env_active(proc_current())) {
            int mr = env_mediate_use((int)gfd, 0);
            if (mr) {
                cqe->res = mr;
                return mr;
            }
        }
        cqe->res = vfs_fsync((int)gfd);
        return 0;
    }
    case IORING_OP_CLOSE: {
        int64_t gfd = fdtable_get_current(sqe->fd);
        if (gfd < 0) {
            cqe->res = (int32_t)gfd;
            return gfd;
        }
        cqe->res = vfs_close((int)gfd);
        return 0;
    }
    case IORING_OP_READ: {
        int64_t gfd = fdtable_get_current(sqe->fd);
        if (gfd < 0) {
            cqe->res = (int32_t)gfd;
            return gfd;
        }
        /* Execution-point mediation (docs/research/05 §2.5.2): io_uring
         * ops bypass the read/write syscalls, so the budget is charged
         * where the authority is consumed, per SQE. */
        if (env_active(proc_current())) {
            int mr = env_mediate_use_dir((int)gfd, sqe->len, 0);
            if (mr) {
                cqe->res = mr;
                return mr;
            }
        }
        size_t n = sqe->len;
        if (n == 0) {
            cqe->res = 0;
            return 0;
        }
        void *kbuf = kmalloc(n ? n : 1);
        if (!kbuf) {
            cqe->res = -ENOMEM;
            return -ENOMEM;
        }
        vfile_t *vf = vfs_get_file_ref((int)gfd);
        if (!vf) {
            kfree(kbuf);
            cqe->res = -EBADF;
            return -EBADF;
        }
        int r;
        if (sqe->u1.off == (uint64_t)-1)
            r = vfs_read_file(vf, kbuf, n);
        else {
            long saved = vf->ops && vf->ops->lseek
                             ? vf->ops->lseek(vf, 0, SEEK_CUR)
                             : -1;
            if (saved >= 0 && vf->ops->lseek(vf, (long)sqe->u1.off, SEEK_SET) >= 0) {
                r = vfs_read_file(vf, kbuf, n);
                (void)vf->ops->lseek(vf, saved, SEEK_SET);
            } else {
                r = -ESPIPE;
            }
        }
        vfs_put_file_ref((int)gfd, vf);
        if (r > 0) {
            if (copy_to_user((void *)(uintptr_t)sqe->u2.addr, kbuf, (size_t)r) < 0)
                r = -EFAULT;
        }
        kfree(kbuf);
        cqe->res = r;
        return 0;
    }
    case IORING_OP_WRITE: {
        int64_t gfd = fdtable_get_current(sqe->fd);
        if (gfd < 0) {
            cqe->res = (int32_t)gfd;
            return gfd;
        }
        size_t n = sqe->len;
        if (n == 0) {
            cqe->res = 0;
            return 0;
        }
        if (env_active(proc_current())) {
            int mr = env_mediate_use_dir((int)gfd, n, 1);
            if (mr) {
                cqe->res = mr;
                return mr;
            }
        }
        void *kbuf = kmalloc(n ? n : 1);
        if (!kbuf) {
            cqe->res = -ENOMEM;
            return -ENOMEM;
        }
        if (copy_from_user(kbuf, (const void *)(uintptr_t)sqe->u2.addr, n) < 0) {
            kfree(kbuf);
            cqe->res = -EFAULT;
            return -EFAULT;
        }
        vfile_t *vf = vfs_get_file_ref((int)gfd);
        if (!vf) {
            kfree(kbuf);
            cqe->res = -EBADF;
            return -EBADF;
        }
        int r;
        if (sqe->u1.off == (uint64_t)-1)
            r = vfs_write_file(vf, kbuf, n);
        else {
            long saved = vf->ops && vf->ops->lseek
                             ? vf->ops->lseek(vf, 0, SEEK_CUR)
                             : -1;
            if (saved >= 0 && vf->ops->lseek(vf, (long)sqe->u1.off, SEEK_SET) >= 0) {
                r = vfs_write_file(vf, kbuf, n);
                (void)vf->ops->lseek(vf, saved, SEEK_SET);
            } else {
                r = -ESPIPE;
            }
        }
        vfs_put_file_ref((int)gfd, vf);
        kfree(kbuf);
        cqe->res = r;
        return 0;
    }
    default:
        cqe->res = -EINVAL;
        return -EINVAL;
    }
}

long io_uring_enter(int gfd, unsigned to_submit, unsigned min_complete,
                    unsigned flags)
{
    (void)min_complete;
    (void)flags;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;
    if (vf->ops != &g_io_uring_ops || !vf->priv) {
        vfs_put_file_ref(gfd, vf);
        return -EBADF;
    }
    io_uring_ring_t *ring = vf->priv;
    vfs_put_file_ref(gfd, vf);

    io_uring_sqe_t *sq = io_uring_sq_kaddr(ring);
    io_uring_cqe_t *cq = io_uring_cq_kaddr(ring);

    /* The SQ and CQ pages are user-mapped and also visible through the
     * kernel direct map (sq/cq are kernel addresses of the same physical
     * pages).  A20OS does not implement the shared head/tail ring-header
     * protocol, so io_uring_enter() treats the submission queue as a flat
     * array: it consumes the first @to_submit SQEs written by userland and
     * writes completions into the first slots of the completion queue.  This
     * is sufficient for the synchronous executor and for userland that drives
     * everything through enter(). */
    if (to_submit > ring->nr_entries)
        to_submit = ring->nr_entries;

    long processed = 0;
    for (unsigned i = 0; i < to_submit; i++) {
        io_uring_cqe_t cqe;
        (void)io_uring_execute_sqe(&sq[i], &cqe);
        cq[processed] = cqe;
        processed++;
    }

    /* IORING_REGISTER_EVENTFD: notify the registered eventfd once when new
     * completions land, so userland can wake on the fd instead of polling. */
    if (processed > 0 && ring->eventfd_fd >= 0) {
        int64_t egfd = fdtable_get_current(ring->eventfd_fd);
        if (egfd >= 0) {
            vfile_t *evf = vfs_get_file_ref((int)egfd);
            if (evf) {
                if (eventfd_vfile_is(evf)) {
                    uint64_t one = 1;
                    (void)vfs_write_file(evf, (const char *)&one, sizeof(one));
                }
                vfs_put_file_ref((int)egfd, evf);
            }
        }
    }
    return processed;
}

int io_uring_register(int gfd, unsigned opcode, const void *arg,
                      unsigned nr_args)
{
    switch (opcode) {
    case IORING_REGISTER_FILES:
        /* Fixed-file tables are descriptor-transfer events (docs/research/05
         * §2.5.1 A9): enveloped tasks may not bulk-import authorities in
         * v1; the executor resolves fds through the task fd table anyway,
         * so registration stays a pure fast-path hint elsewhere. */
        if (env_active(proc_current()))
            return -EPERM;
        (void)arg;
        (void)nr_args;
        return 0;
    case IORING_REGISTER_EVENTFD: {
        if (!arg || nr_args == 0)
            return -EINVAL;
        int eventfd_fd = 0;
        if (copy_from_user(&eventfd_fd, arg, sizeof(eventfd_fd)) < 0)
            return -EFAULT;
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (!vf || vf->ops != &g_io_uring_ops || !vf->priv) {
            if (vf) vfs_put_file_ref(gfd, vf);
            return -EBADF;
        }
        io_uring_ring_t *ring = vf->priv;
        int64_t efd = fdtable_get_current(eventfd_fd);
        if (efd < 0) {
            vfs_put_file_ref(gfd, vf);
            return -EBADF;
        }
        vfile_t *evf = vfs_get_file_ref((int)efd);
        if (!evf || !eventfd_vfile_is(evf)) {
            if (evf) vfs_put_file_ref((int)efd, evf);
            vfs_put_file_ref(gfd, vf);
            return -EINVAL;
        }
        vfs_put_file_ref((int)efd, evf);
        ring->eventfd_fd = eventfd_fd;
        vfs_put_file_ref(gfd, vf);
        return 0;
    }
    default:
        return -EINVAL;
    }
}
