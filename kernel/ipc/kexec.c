#include "ipc/kexec.h"

#include "core/lock.h"
#include "core/string.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"
#include "mm/elf_rust.h"

/*
 * The published image, the staging transaction and every transition between
 * them are Rust-owned (a20_elf_kexec_*). This file keeps only the primitives:
 * frame allocation/free, usercopy, and the lock that guards the published
 * ledger. Rust decides which chains are still held and in what order they must
 * be released; the adapter only frees the handles it is handed, exactly once.
 */

/* The running-kernel physical extent used for overlap validation, derived from
 * real section symbols through the direct map (PROVIDE absolute symbols are
 * not PC-relative addressable under mcmodel=medany). */
extern char _start[];
extern char _bss_end[];

static a20_elf_kexec_ledger_t g_kexec_image;
static spinlock_t g_kexec_lock;

_Static_assert(KEXEC_MAX_SEGMENTS == A20_ELF_KEXEC_MAX_SEGMENTS,
               "kexec segment capacity must match the Rust ledger");

/* A staged segment is a chain of frames. The node layout is the adapter's
 * physical detail; the Rust ledger only ever sees its head as an opaque
 * handle. */
struct kexec_page_chain {
    struct kexec_page_chain *next;
    uint8_t data[PAGE_SIZE - sizeof(void *)];
};

static void kexec_free_chain(struct kexec_page_chain *p)
{
    while (p) {
        struct kexec_page_chain *n = p->next;
        frame_free(p);
        p = n;
    }
}

typedef struct { const void *user_segments; } kexec_user_context_t;

static int32_t kexec_read_user_segment(void *opaque, uint32_t index,
                                       a20_elf_kexec_segment_t *out)
{
    kexec_user_context_t *ctx = opaque;
    if (!ctx || !out) return -EFAULT;
    return copy_from_user(out, (const char *)ctx->user_segments +
                          (size_t)index * sizeof(*out), sizeof(*out)) < 0
           ? -EFAULT : 0;
}

static int32_t kexec_stage_user_segment(void *opaque,
                                        const a20_elf_kexec_segment_t *wire,
                                        uint64_t *pages_out,
                                        uint64_t *bytes_out)
{
    (void)opaque;
    if (!wire || !pages_out || !bytes_out) return -EFAULT;
    void *kbuf = NULL;
    if (wire->bufsz) {
        kbuf = kmalloc((size_t)wire->bufsz);
        if (!kbuf) return -ENOMEM;
        if (copy_from_user(kbuf, (const void *)(uintptr_t)wire->buf,
                           (size_t)wire->bufsz) < 0) {
            kfree(kbuf);
            return -EFAULT;
        }
    }
    uint64_t copied = 0;
    struct kexec_page_chain *head = NULL;
    struct kexec_page_chain **tail = &head;
    while (copied < wire->memsz) {
        void *frame = frame_alloc();
        if (!frame) { kexec_free_chain(head); kfree(kbuf); return -ENOMEM; }
        struct kexec_page_chain *node = frame;
        node->next = NULL;
        memset(node->data, 0, sizeof(node->data));
        size_t n = wire->memsz - copied;
        if (n > sizeof(node->data)) n = sizeof(node->data);
        if (copied < wire->bufsz) {
            size_t take = wire->bufsz - copied;
            if (take > n) take = n;
            memcpy(node->data, (const char *)kbuf + copied, take);
        }
        *tail = node; tail = &node->next; copied += n;
    }
    kfree(kbuf);
    *pages_out = (uint64_t)(uintptr_t)head;
    *bytes_out = copied;
    return 0;
}

static uint64_t kexec_lock_cb(void *unused)
{ (void)unused; return spin_lock_irqsave(&g_kexec_lock); }
static void kexec_unlock_cb(void *unused, uint64_t flags)
{ (void)unused; spin_unlock_irqrestore(&g_kexec_lock, flags); }
static void kexec_free_pages_cb(void *unused, uint64_t pages)
{ (void)unused; kexec_free_chain((struct kexec_page_chain *)(uintptr_t)pages); }

typedef struct { int local_fd; } kexec_file_context_t;

/* Read through the pinned open-file description. This is the vfs_pread
 * offset protocol applied to the already-referenced vfile, so fd close/reuse
 * cannot redirect the image halfway through a load. */
static int32_t kexec_read_pinned_file(void *opaque, const uint8_t **buffer_out,
                                      uint64_t *size_out)
{
    kexec_file_context_t *ctx = opaque;
    if (!ctx || !buffer_out || !size_out) return -EFAULT;
    vfile_t *vf = fdtable_get_current_file_ref(ctx->local_fd);
    if (!vf) return -EBADF;
    uint64_t size = vf->vnode ? vf->vnode->size : 0;
    if (!size || size > (64UL << 20)) { vfs_put_file(vf); return -EINVAL; }
    if (!vf->ops || !vf->ops->lseek) { vfs_put_file(vf); return -ESPIPE; }
    uint8_t *buf = kmalloc((size_t)size);
    if (!buf) { vfs_put_file(vf); return -ENOMEM; }
    mutex_lock(&vf->offset_lock);
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    long seek = saved < 0 ? saved : vf->ops->lseek(vf, 0, SEEK_SET);
    int got = seek < 0 ? (int)seek : vfs_read_file(vf, (char *)buf, (size_t)size);
    long restore = saved < 0 ? 0 : vf->ops->lseek(vf, saved, SEEK_SET);
    mutex_unlock(&vf->offset_lock);
    vfs_put_file(vf);
    if (got < 0 || (uint64_t)got != size || (restore < 0 && got >= 0)) {
        kfree(buf);
        return got < 0 ? got : (restore < 0 ? (int32_t)restore : -EIO);
    }
    *buffer_out = buf; *size_out = size;
    return 0;
}

static int32_t kexec_stage_kernel_file(void *unused, const uint8_t *buf,
                                       uint64_t size, uint64_t *pages_out,
                                       uint64_t *bytes_out)
{
    (void)unused;
    if (!buf || !pages_out || !bytes_out) return -EFAULT;
    uint64_t copied = 0;
    struct kexec_page_chain *head = NULL, **tail = &head;
    while (copied < size) {
        struct kexec_page_chain *node = frame_alloc();
        if (!node) { kexec_free_chain(head); return -ENOMEM; }
        node->next = NULL;
        memset(node->data, 0, sizeof(node->data));
        size_t n = size - copied;
        if (n > sizeof(node->data)) n = sizeof(node->data);
        memcpy(node->data, buf + copied, n);
        *tail = node; tail = &node->next; copied += n;
    }
    *pages_out = (uint64_t)(uintptr_t)head; *bytes_out = copied;
    return 0;
}
static void kexec_free_buffer(void *unused, const uint8_t *buffer)
{ (void)unused; kfree((void *)buffer); }

int kexec_is_loaded(void)
{
    uint64_t flags = spin_lock_irqsave(&g_kexec_lock);
    int loaded = a20_elf_kexec_loaded(&g_kexec_image);
    spin_unlock_irqrestore(&g_kexec_lock, flags);
    return loaded;
}

void kexec_discard(void)
{
    a20_elf_kexec_load_ops_t ops = { .lock = kexec_lock_cb,
        .unlock = kexec_unlock_cb, .free_pages = kexec_free_pages_cb };
    (void)a20_elf_kexec_discard_transaction(&g_kexec_image, &ops);
}

int kexec_load_segments(uint64_t entry, uint64_t nr_segments,
                        const void *usegments, unsigned long flags)
{
    (void)flags;
    if (!usegments)
        return -EINVAL;

    uint64_t kstart = (uint64_t)va_to_pa((const void *)_start);
    uint64_t kend   = (uint64_t)va_to_pa((const void *)_bss_end);
    kexec_user_context_t ctx = { .user_segments = usegments };
    a20_elf_kexec_load_ops_t ops = { .context = &ctx,
        .read_segment = kexec_read_user_segment,
        .stage_segment = kexec_stage_user_segment,
        .lock = kexec_lock_cb, .unlock = kexec_unlock_cb,
        .free_pages = kexec_free_pages_cb };
    return a20_elf_kexec_load_transaction(&g_kexec_image, entry, nr_segments,
                                           kstart, kend, &ops);
}

int kexec_load_file(int kernel_fd, int initrd_fd,
                    uint64_t cmdline_len, const void *ucmdline,
                    unsigned long flags)
{
    (void)initrd_fd;
    (void)flags;
    (void)ucmdline;
    if (cmdline_len > 4096)
        return -EINVAL;

    kexec_file_context_t ctx = { .local_fd = kernel_fd };
    a20_elf_kexec_file_ops_t ops = { .context = &ctx,
        .read_file = kexec_read_pinned_file,
        .stage_file = kexec_stage_kernel_file,
        .free_buffer = kexec_free_buffer,
        .lock = kexec_lock_cb, .unlock = kexec_unlock_cb,
        .free_pages = kexec_free_pages_cb };
    return a20_elf_kexec_file_transaction(&g_kexec_image, &ops);
}
