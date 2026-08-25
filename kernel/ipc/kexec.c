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

/* Kernel image physical extent used for overlap validation, derived from
 * real section symbols through the direct map (PROVIDE absolute symbols
 * are not PC-relative addressable under mcmodel=medany). */
extern char _start[];
extern char _bss_end[];

static kexec_image_t g_kexec_image;
static spinlock_t g_kexec_lock;

struct kexec_page_chain {
    struct kexec_page_chain *next;
    uint8_t data[PAGE_SIZE - sizeof(void *)];
};

static void kexec_image_reset_locked(void)
{
    kexec_image_t *img = &g_kexec_image;
    if (!img->valid)
        return;
    for (int i = 0; i < img->nr_segments; i++) {
        struct kexec_page_chain *p = img->seg[i].pages;
        while (p) {
            struct kexec_page_chain *n = p->next;
            frame_free(p);
            p = n;
        }
    }
    memset(img, 0, sizeof(*img));
}

static int kexec_range_overlaps_kernel(uint64_t start, uint64_t size)
{
    uint64_t kstart = (uint64_t)va_to_pa((const void *)_start);
    uint64_t kend = (uint64_t)va_to_pa((const void *)_bss_end);
    return start < kend && kstart < start + size;
}

int kexec_is_loaded(void)
{
    return __atomic_load_n(&g_kexec_image.valid, __ATOMIC_ACQUIRE);
}

void kexec_discard(void)
{
    uint64_t flags = spin_lock_irqsave(&g_kexec_lock);
    kexec_image_reset_locked();
    spin_unlock_irqrestore(&g_kexec_lock, flags);
}

static int kexec_stage_segment(kexec_segment_staged_t *dst,
                               const void *kbuf, uint64_t bufsz,
                               uint64_t memsz)
{
    memset(dst, 0, sizeof(*dst));
    dst->bufsz = bufsz;
    dst->memsz = memsz;

    uint64_t copied = 0;
    struct kexec_page_chain **tail = (struct kexec_page_chain **)&dst->pages;
    while (copied < memsz) {
        void *frame = frame_alloc();
        if (!frame)
            return -ENOMEM;
        struct kexec_page_chain *node = (struct kexec_page_chain *)frame;
        node->next = NULL;
        memset(node->data, 0, sizeof(node->data));
        size_t chunk = memsz - copied;
        if (chunk > sizeof(node->data))
            chunk = sizeof(node->data);
        if (copied < bufsz) {
            size_t n = bufsz - copied;
            if (n > chunk)
                n = chunk;
            memcpy(node->data, (const char *)kbuf + copied, n);
        }
        /* Bytes beyond bufsz stay zero: kexec segments are zero-filled to
         * memsz like Linux. */
        *tail = node;
        tail = &node->next;
        dst->pages_bytes += sizeof(node->data);
        copied += chunk;
    }
    return 0;
}

int kexec_load_segments(uint64_t entry, uint64_t nr_segments,
                        const void *usegments, unsigned long flags)
{
    (void)flags;
    if (!usegments || nr_segments == 0 || nr_segments > KEXEC_MAX_SEGMENTS)
        return -EINVAL;

    /* struct kexec_segment { void* buf; size_t bufsz; void* mem; size_t
     * memsz; } on LP64 = 4 x 8 bytes. */
    kexec_segment_staged_t staged[KEXEC_MAX_SEGMENTS];
    memset(staged, 0, sizeof(staged));

    int rc = 0;
    for (uint64_t i = 0; i < nr_segments && rc == 0; i++) {
        uint64_t wire[4];
        if (copy_from_user(wire, (const char *)usegments + i * 4 * 8,
                           sizeof(wire)) < 0) {
            rc = -EFAULT;
            break;
        }
        const void *ubuf = (const void *)wire[0];
        uint64_t bufsz = wire[1];
        uint64_t mem = wire[2];
        uint64_t memsz = wire[3];

        if (bufsz > memsz)
            return -EINVAL;
        if (mem + memsz < mem)
            return -EINVAL;
        if (kexec_range_overlaps_kernel(mem, memsz)) {
            rc = -EADDRNOTAVAIL;
            break;
        }
        for (uint64_t j = 0; j < i && rc == 0; j++) {
            uint64_t s = staged[j].mem, e = staged[j].mem + staged[j].memsz;
            if (mem < e && s < mem + memsz)
                rc = -EADDRNOTAVAIL;
        }
        if (rc != 0)
            break;

        void *kbuf = NULL;
        if (bufsz) {
            kbuf = kmalloc(bufsz);
            if (!kbuf) {
                rc = -ENOMEM;
                break;
            }
            if (copy_from_user(kbuf, ubuf, bufsz) < 0) {
                kfree(kbuf);
                rc = -EFAULT;
                break;
            }
        }

        rc = kexec_stage_segment(&staged[i], kbuf, bufsz, memsz);
        if (kbuf)
            kfree(kbuf);
        if (rc == 0)
            staged[i].mem = mem;
    }

    if (rc != 0) {
        /* Roll back whatever this call staged so far. */
        for (int i = 0; i < KEXEC_MAX_SEGMENTS; i++) {
            struct kexec_page_chain *p = staged[i].pages;
            while (p) {
                struct kexec_page_chain *n = p->next;
                frame_free(p);
                p = n;
            }
        }
        return rc;
    }

    uint64_t flags_i = spin_lock_irqsave(&g_kexec_lock);
    kexec_image_reset_locked();
    g_kexec_image.valid = 1;
    g_kexec_image.file_backed = 0;
    g_kexec_image.entry = entry;
    g_kexec_image.nr_segments = (int)nr_segments;
    for (int i = 0; i < KEXEC_MAX_SEGMENTS; i++)
        g_kexec_image.seg[i] = staged[i];
    spin_unlock_irqrestore(&g_kexec_lock, flags_i);
    return 0;
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

    int gfd = fdtable_get_current(kernel_fd);
    if (gfd < 0)
        return gfd;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return -EBADF;

    uint64_t size = vf->vnode ? vf->vnode->size : 0;
    vfs_put_file_ref(gfd, vf);
    if (size == 0 || size > (64UL << 20))
        return -EINVAL;

    /* Read and validate the image; placement is deferred to the
     * architecture machine_kexec backend because ELF program headers
     * need arch-specific relocation. */
    char magic[4];
    if (vfs_pread(kernel_fd, magic, sizeof(magic), 0) !=
        (int64_t)sizeof(magic))
        return -EIO;
    if (magic[0] != 0x7f || magic[1] != 'E' || magic[2] != 'L' ||
        magic[3] != 'F')
        return -ENOEXEC;

    void *image = kmalloc(size);
    if (!image)
        return -ENOMEM;
    int64_t r = vfs_pread(kernel_fd, (char *)image, size, 0);
    if (r < 0 || (uint64_t)r != size) {
        kfree(image);
        return r < 0 ? (int)r : -EIO;
    }

    kexec_segment_staged_t staged;
    int rc = kexec_stage_segment(&staged, image, size, size);
    kfree(image);
    if (rc != 0)
        return rc;

    uint64_t flags_i = spin_lock_irqsave(&g_kexec_lock);
    kexec_image_reset_locked();
    g_kexec_image.valid = 1;
    g_kexec_image.file_backed = 1;
    g_kexec_image.entry = 0;
    g_kexec_image.nr_segments = 1;
    staged.mem = 0;   /* backend relocates; no wire promise made here */
    g_kexec_image.seg[0] = staged;
    spin_unlock_irqrestore(&g_kexec_lock, flags_i);
    return 0;
}
