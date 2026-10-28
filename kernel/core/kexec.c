#include "core/kexec.h"

#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "drivers/core/driver_core.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "mm/elf.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

extern char _start[];
extern char _bss_end[];

static a20_elf_kexec_ledger_t g_kexec_image;
static spinlock_t g_kexec_lock = SPINLOCK_INIT;
static int g_kexec_loading;
static int g_kexec_executing;

struct kexec_page_chain {
    struct kexec_page_chain *next;
    uint8_t data[PAGE_SIZE - sizeof(void *)];
};

typedef struct { const void *user_segments; } kexec_user_context_t;
typedef struct { const uint8_t *image; } kexec_image_context_t;

static int kexec_may_manage(void)
{
    task_t *task = proc_current();
    if (!task)
        return -ESRCH;
    if (!proc_has_cap(task, CAP_SYS_BOOT) && task->cred.euid != 0)
        return -EPERM;
    return 0;
}

static int kexec_begin_load(void)
{
    int rc = kexec_may_manage();
    if (rc < 0)
        return rc;
    uint64_t flags = spin_lock_irqsave(&g_kexec_lock);
    rc = g_kexec_executing || g_kexec_loading ? -EBUSY : 0;
    if (!rc)
        g_kexec_loading = 1;
    spin_unlock_irqrestore(&g_kexec_lock, flags);
    return rc;
}

static void kexec_end_load(void)
{
    uint64_t flags = spin_lock_irqsave(&g_kexec_lock);
    g_kexec_loading = 0;
    spin_unlock_irqrestore(&g_kexec_lock, flags);
}

static void kexec_free_chain(struct kexec_page_chain *page)
{
    while (page) {
        struct kexec_page_chain *next = page->next;
        frame_free(page);
        page = next;
    }
}

static int kexec_stage_bytes(const a20_elf_kexec_segment_t *wire,
                             const uint8_t *bytes, uint64_t *pages_out,
                             uint64_t *bytes_out)
{
    if (!wire || !pages_out || !bytes_out || wire->bufsz > wire->memsz ||
        wire->memsz > KEXEC_MAX_IMAGE_SIZE)
        return -EINVAL;
    uint64_t copied = 0;
    struct kexec_page_chain *head = NULL;
    struct kexec_page_chain **tail = &head;
    while (copied < wire->memsz) {
        struct kexec_page_chain *node = frame_alloc();
        if (!node) {
            kexec_free_chain(head);
            return -ENOMEM;
        }
        node->next = NULL;
        memset(node->data, 0, sizeof(node->data));
        size_t count = wire->memsz - copied;
        if (count > sizeof(node->data))
            count = sizeof(node->data);
        if (copied < wire->bufsz) {
            size_t take = wire->bufsz - copied;
            if (take > count)
                take = count;
            memcpy(node->data, bytes + copied, take);
        }
        *tail = node;
        tail = &node->next;
        copied += count;
    }
    *pages_out = (uint64_t)(uintptr_t)head;
    *bytes_out = copied;
    return 0;
}

static int32_t kexec_read_user_segment(void *opaque, uint32_t index,
                                       a20_elf_kexec_segment_t *out)
{
    kexec_user_context_t *context = opaque;
    if (!context || !out)
        return -EFAULT;
    return copy_from_user(out, (const char *)context->user_segments +
                          (size_t)index * sizeof(*out), sizeof(*out)) < 0
           ? -EFAULT : 0;
}

static int32_t kexec_stage_user_segment(void *opaque,
                                        const a20_elf_kexec_segment_t *wire,
                                        uint64_t *pages_out, uint64_t *bytes_out)
{
    (void)opaque;
    if (!wire || wire->bufsz > KEXEC_MAX_IMAGE_SIZE)
        return -EINVAL;
    uint8_t *copy = NULL;
    if (wire->bufsz) {
        if (!wire->buf)
            return -EFAULT;
        copy = kmalloc((size_t)wire->bufsz);
        if (!copy)
            return -ENOMEM;
        if (copy_from_user(copy, (const void *)(uintptr_t)wire->buf,
                           (size_t)wire->bufsz) < 0) {
            kfree(copy);
            return -EFAULT;
        }
    }
    int rc = kexec_stage_bytes(wire, copy, pages_out, bytes_out);
    kfree(copy);
    return rc;
}

static int32_t kexec_read_kernel_segment(void *opaque, uint32_t index,
                                         a20_elf_kexec_segment_t *out)
{
    const kexec_segment_t *segments = opaque;
    if (!segments || !out || index >= KEXEC_MAX_SEGMENTS)
        return -EFAULT;
    out->buf = (uint64_t)(uintptr_t)segments[index].buf;
    out->bufsz = segments[index].bufsz;
    out->mem = (uint64_t)(uintptr_t)segments[index].mem;
    out->memsz = segments[index].memsz;
    return 0;
}

static int32_t kexec_stage_kernel_segment(void *opaque,
                                          const a20_elf_kexec_segment_t *wire,
                                          uint64_t *pages_out, uint64_t *bytes_out)
{
    (void)opaque;
    if (!wire || (wire->bufsz && !wire->buf))
        return -EFAULT;
    return kexec_stage_bytes(wire, (const uint8_t *)(uintptr_t)wire->buf,
                             pages_out, bytes_out);
}

static int32_t kexec_stage_image_segment(void *opaque,
                                         const a20_elf_kexec_segment_t *wire,
                                         uint64_t *pages_out, uint64_t *bytes_out)
{
    kexec_image_context_t *context = opaque;
    if (!context || !wire || (wire->bufsz && !context->image))
        return -EFAULT;
    return kexec_stage_bytes(wire, context->image + wire->buf,
                             pages_out, bytes_out);
}

static uint64_t kexec_lock_cb(void *unused)
{
    (void)unused;
    return spin_lock_irqsave(&g_kexec_lock);
}

static void kexec_unlock_cb(void *unused, uint64_t flags)
{
    (void)unused;
    spin_unlock_irqrestore(&g_kexec_lock, flags);
}

static void kexec_free_pages_cb(void *unused, uint64_t pages)
{
    (void)unused;
    kexec_free_chain((struct kexec_page_chain *)(uintptr_t)pages);
}

static const a20_elf_kexec_load_ops_t kexec_ledger_ops = {
    .lock = kexec_lock_cb,
    .unlock = kexec_unlock_cb,
    .free_pages = kexec_free_pages_cb,
};

int kexec_load_segments(uint64_t entry, uint64_t nr_segments,
                        const void *usegments, unsigned long flags)
{
    if (flags)
        return -EINVAL;
    if (!usegments)
        return -EFAULT;
    int rc = kexec_begin_load();
    if (rc < 0)
        return rc;
    kexec_user_context_t context = { .user_segments = usegments };
    a20_elf_kexec_load_ops_t ops = kexec_ledger_ops;
    ops.context = &context;
    ops.read_segment = kexec_read_user_segment;
    ops.stage_segment = kexec_stage_user_segment;
    rc = a20_elf_kexec_load_transaction(&g_kexec_image, entry, nr_segments,
                                        (uint64_t)va_to_pa(_start),
                                        (uint64_t)va_to_pa(_bss_end), &ops);
    kexec_end_load();
    return rc;
}

int kexec_load_segment_array(uint64_t entry, uint64_t nr_segments,
                             const kexec_segment_t *segments)
{
    if (!segments)
        return -EFAULT;
    int rc = kexec_begin_load();
    if (rc < 0)
        return rc;
    a20_elf_kexec_load_ops_t ops = kexec_ledger_ops;
    ops.context = (void *)segments;
    ops.read_segment = kexec_read_kernel_segment;
    ops.stage_segment = kexec_stage_kernel_segment;
    rc = a20_elf_kexec_load_transaction(&g_kexec_image, entry, nr_segments,
                                        (uint64_t)va_to_pa(_start),
                                        (uint64_t)va_to_pa(_bss_end), &ops);
    kexec_end_load();
    return rc;
}

int kexec_load_image(const void *image, uint64_t size)
{
    if (!image)
        return -EFAULT;
    if (!size || size > KEXEC_MAX_IMAGE_SIZE)
        return -EINVAL;
    int rc = kexec_begin_load();
    if (rc < 0)
        return rc;
    kexec_image_context_t context = { .image = image };
    a20_elf_kexec_load_ops_t ops = kexec_ledger_ops;
    ops.context = &context;
    ops.stage_segment = kexec_stage_image_segment;
    rc = a20_elf_kexec_image_transaction(&g_kexec_image, image, size,
                                         ARCH_ELF_CLASS, ARCH_ELF_MACHINE, &ops);
    kexec_end_load();
    return rc;
}

typedef struct {
    vfile_t *file;
    uint64_t size;
} kexec_file_context_t;

/* Absolute-positioned read from the pinned image file. The caller holds the
 * file's offset_lock for the whole transaction, so the seek+read pair is
 * atomic against other threads sharing the open file description. */
static int32_t kexec_file_read_at(void *opaque, uint64_t off, uint8_t *dst,
                                  size_t len)
{
    kexec_file_context_t *context = opaque;
    if (!context || !context->file || (!dst && len))
        return -EFAULT;
    if (off > context->size || len > context->size - off)
        return -EIO;
    if (!len)
        return 0;
    long seek = context->file->ops->lseek(context->file, (long)off, SEEK_SET);
    if (seek < 0)
        return (int32_t)seek;
    int got = vfs_read_file(context->file, (char *)dst, len);
    if (got < 0)
        return got;
    return (size_t)got == len ? 0 : -EIO;
}

/* Stage one PT_LOAD straight from the image file into a fresh page chain:
 * the payload is streamed node by node and the BSS tail stays zeroed, so a
 * large kernel never needs one contiguous staging buffer. wire->buf carries
 * the file offset. */
static int32_t kexec_stage_file_segment(void *opaque,
                                        const a20_elf_kexec_segment_t *wire,
                                        uint64_t *pages_out, uint64_t *bytes_out)
{
    kexec_file_context_t *context = opaque;
    if (!context || !wire || !pages_out || !bytes_out ||
        wire->bufsz > wire->memsz || wire->memsz > KEXEC_MAX_IMAGE_SIZE)
        return -EINVAL;
    uint64_t copied = 0;
    struct kexec_page_chain *head = NULL;
    struct kexec_page_chain **tail = &head;
    while (copied < wire->memsz) {
        struct kexec_page_chain *node = frame_alloc();
        if (!node) {
            kexec_free_chain(head);
            return -ENOMEM;
        }
        node->next = NULL;
        memset(node->data, 0, sizeof(node->data));
        size_t count = wire->memsz - copied;
        if (count > sizeof(node->data))
            count = sizeof(node->data);
        if (copied < wire->bufsz) {
            size_t take = wire->bufsz - copied;
            if (take > count)
                take = count;
            int32_t rc = kexec_file_read_at(context, wire->buf + copied,
                                            node->data, take);
            if (rc < 0) {
                kexec_free_chain(head);
                return rc;
            }
        }
        *tail = node;
        tail = &node->next;
        copied += count;
    }
    *pages_out = (uint64_t)(uintptr_t)head;
    *bytes_out = copied;
    return 0;
}

int kexec_load_file(int kernel_fd, int initrd_fd,
                    uint64_t cmdline_len, const void *ucmdline,
                    unsigned long flags)
{
    (void)ucmdline;
    if (kernel_fd < 0) {
        if (initrd_fd != -1 || cmdline_len || flags != 1)
            return -EINVAL;
        return kexec_discard();
    }
    if (initrd_fd != -1 || cmdline_len || (flags & ~(1UL << 2)))
        return -EOPNOTSUPP;
    int rc = kexec_may_manage();
    if (rc < 0)
        return rc;
    vfile_t *file = fdtable_get_current_file_ref(kernel_fd);
    if (!file)
        return -EBADF;
    uint64_t size = file->vnode ? file->vnode->size : 0;
    if (!size || size > KEXEC_MAX_IMAGE_SIZE) {
        vfs_put_file(file);
        return -EINVAL;
    }
    if (!file->ops || !file->ops->lseek) {
        vfs_put_file(file);
        return -ESPIPE;
    }
    rc = kexec_begin_load();
    if (rc < 0) {
        vfs_put_file(file);
        return rc;
    }
    kexec_file_context_t context = { .file = file, .size = size };
    a20_elf_kexec_load_ops_t ops = kexec_ledger_ops;
    ops.context = &context;
    ops.stage_segment = kexec_stage_file_segment;
    mutex_lock(&file->offset_lock);
    long saved = file->ops->lseek(file, 0, SEEK_CUR);
    rc = a20_elf_kexec_file_transaction(&g_kexec_image, size,
                                        ARCH_ELF_CLASS, ARCH_ELF_MACHINE,
                                        kexec_file_read_at, &ops);
    if (saved >= 0)
        file->ops->lseek(file, saved, SEEK_SET);
    mutex_unlock(&file->offset_lock);
    kexec_end_load();
    vfs_put_file(file);
    return rc;
}

int kexec_discard(void)
{
    int rc = kexec_may_manage();
    if (rc < 0)
        return rc;
    uint64_t flags = spin_lock_irqsave(&g_kexec_lock);
    rc = g_kexec_executing || g_kexec_loading ? -EBUSY : 0;
    spin_unlock_irqrestore(&g_kexec_lock, flags);
    if (rc < 0)
        return rc;
    return a20_elf_kexec_discard_transaction(&g_kexec_image, &kexec_ledger_ops);
}

int kexec_is_loaded(void)
{
    uint64_t flags = spin_lock_irqsave(&g_kexec_lock);
    int loaded = a20_elf_kexec_loaded(&g_kexec_image);
    spin_unlock_irqrestore(&g_kexec_lock, flags);
    return loaded;
}

void kexec_get_status(kexec_status_t *out)
{
    if (!out)
        return;
    uint64_t flags = spin_lock_irqsave(&g_kexec_lock);
    out->state = g_kexec_executing ? KEXEC_STATUS_EXECUTING :
                 a20_elf_kexec_loaded(&g_kexec_image) ? KEXEC_STATUS_LOADED :
                 KEXEC_STATUS_EMPTY;
    out->entry = g_kexec_image.entry;
    out->nr_segments = g_kexec_image.nr;
    out->backend = machine_kexec_backend_id();
    out->reserved = 0;
    spin_unlock_irqrestore(&g_kexec_lock, flags);
}

const a20_elf_kexec_ledger_t *kexec_image_for_machine(void)
{
    return &g_kexec_image;
}

__attribute__((weak)) uint32_t machine_kexec_backend_id(void)
{
    return 0;
}

__attribute__((weak)) int machine_kexec_prepare(const a20_elf_kexec_ledger_t *image)
{
    (void)image;
    return -ENOSYS;
}

__attribute__((weak)) void machine_kexec_cancel(void) {}

__attribute__((weak, noreturn)) void machine_kexec(const a20_elf_kexec_ledger_t *image)
{
    (void)image;
    for (;;)
        arch_cpu_relax();
}

int kexec_execute(void)
{
    int rc = kexec_may_manage();
    if (rc < 0)
        return rc;
    uint64_t flags = spin_lock_irqsave(&g_kexec_lock);
    if (g_kexec_executing || g_kexec_loading) {
        rc = -EBUSY;
    } else if (!a20_elf_kexec_loaded(&g_kexec_image)) {
        rc = -EINVAL;
    } else {
        g_kexec_executing = 1;
        rc = 0;
    }
    spin_unlock_irqrestore(&g_kexec_lock, flags);
    if (rc < 0)
        return rc;

    rc = driver_core_kexec_prepare();
    if (!rc)
        rc = machine_kexec_prepare(&g_kexec_image);
    if (rc < 0)
        goto fail;
    kinfo("[KEXEC] secondary harts stopped; closing filesystems\n");
    rc = vfs_system_shutdown();
    if (rc < 0) {
        machine_kexec_cancel();
        goto fail;
    }
    kinfo("[KEXEC] filesystems closed; resetting devices\n");
    driver_core_shutdown_all();
    kinfo("[KEXEC] handoff entry=0x%lx segments=%u\n",
          (unsigned long)g_kexec_image.entry, g_kexec_image.nr);
    machine_kexec(&g_kexec_image);

fail:
    flags = spin_lock_irqsave(&g_kexec_lock);
    g_kexec_executing = 0;
    spin_unlock_irqrestore(&g_kexec_lock, flags);
    return rc;
}
