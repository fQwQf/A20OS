#include "ipc/userfaultfd.h"

#include "core/consts.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "core/lock.h"
#include "core/poll.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "proc/park.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/usercopy.h"
#include "cg/cgroup.h"

/*
 * USERFAULTFD_MISSING_PROTOCOL:
 * - A registered MISSING range is an anonymous private mapping whose faults
 *   are handed to the userfaultfd owner before the kernel fabricates a zero
 *   page.  The faulting thread enqueues a UFFD_EVENT_PAGEFAULT message, wakes
 *   the fd's readers, then parks on the per-address faulter wait queue with a
 *   PROC_WAIT_KILLABLE token.
 * - UFFDIO_COPY / UFFDIO_ZEROPAGE map the page into the registering mm under
 *   mm->lock and then wake waiters whose park key equals the faulted address;
 *   UFFDIO_WAKE and UNREGISTER wake by address / globally.  A woken faulter
 *   re-checks the PTE under mm->lock before returning, so a resolve and its
 *   wake can never be lost.
 * - Ranges keep an mm_get() reference, so a handler's in-flight COPY can never
 *   touch an address space that is being destroyed; the mm reference is
 *   dropped when the range is unregistered or the fd closes.
 */

typedef struct userfaultfd_range {
    struct mm_struct *mm;
    vaddr_t start;
    vaddr_t end;
    uint64_t flags;
    struct userfaultfd *owner;
    struct userfaultfd_range *next;
    struct userfaultfd_range *gnext;
} userfaultfd_range_t;

#define UFFD_EVENT_CAPACITY 256

typedef struct userfaultfd {
    spinlock_t lock;
    refcount_t refcount;
    int nonblock;
    uint32_t api_version;
    wait_queue_t readers;
    wait_queue_t faulters;
    userfaultfd_range_t *ranges;
    uffd_msg_t *events;
    size_t event_cap;
    size_t event_head;
    size_t event_tail;
    size_t event_count;
} userfaultfd_t;

/* Global registry of every registered range (fault path lookup by mm). */
static spinlock_t g_uffd_lock = SPINLOCK_INIT;
static userfaultfd_range_t *g_uffd_ranges;
static int g_uffd_range_count;

static userfaultfd_t *userfaultfd_get(userfaultfd_t *uffd)
{
    if (!uffd || !refcount_inc_not_zero(&uffd->refcount))
        return NULL;
    return uffd;
}

static void userfaultfd_free(userfaultfd_t *uffd)
{
    if (!uffd)
        return;
    if (uffd->events)
        kfree(uffd->events);
    kfree(uffd);
}

static void userfaultfd_put(userfaultfd_t *uffd)
{
    if (uffd && refcount_dec_and_test(&uffd->refcount))
        userfaultfd_free(uffd);
}

static void userfaultfd_drop_range(userfaultfd_range_t *range)
{
    if (!range)
        return;
    if (range->mm)
        mm_destroy(range->mm);
    kfree(range);
}

/* Remove a single range from its owner list and the global registry.
 * Caller holds g_uffd_lock. */
static void userfaultfd_range_unlink(userfaultfd_range_t *range)
{
    userfaultfd_t *uffd = range->owner;
    userfaultfd_range_t **pp = &uffd->ranges;
    while (*pp && *pp != range)
        pp = &(*pp)->next;
    if (*pp)
        *pp = range->next;

    userfaultfd_range_t **gp = &g_uffd_ranges;
    while (*gp && *gp != range)
        gp = &(*gp)->gnext;
    if (*gp)
        *gp = range->gnext;

    range->next = NULL;
    range->gnext = NULL;
    g_uffd_range_count--;
}

static void userfaultfd_release_all_ranges(userfaultfd_t *uffd)
{
    uint64_t flags = spin_lock_irqsave(&g_uffd_lock);
    userfaultfd_range_t *range = uffd->ranges;
    while (range) {
        userfaultfd_range_t *next = range->next;
        userfaultfd_range_unlink(range);
        userfaultfd_drop_range(range);
        range = next;
    }
    spin_unlock_irqrestore(&g_uffd_lock, flags);
}

/* Find a MISSING-mode range of @mm covering @page_va and return a referenced
 * uffd, or NULL. */
static userfaultfd_t *userfaultfd_range_lookup(struct mm_struct *mm,
                                               uint64_t page_va)
{
    if (!mm)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&g_uffd_lock);
    for (userfaultfd_range_t *r = g_uffd_ranges; r; r = r->gnext) {
        if (r->mm == mm && (r->flags & UFFDIO_REGISTER_MODE_MISSING) &&
            page_va >= (uint64_t)r->start && page_va < (uint64_t)r->end) {
            userfaultfd_t *uffd = userfaultfd_get(r->owner);
            spin_unlock_irqrestore(&g_uffd_lock, flags);
            return uffd;
        }
    }
    spin_unlock_irqrestore(&g_uffd_lock, flags);
    return NULL;
}

int userfaultfd_range_present(struct mm_struct *mm, uint64_t page_va)
{
    if (!mm || g_uffd_range_count == 0)
        return 0;
    uint64_t flags = spin_lock_irqsave(&g_uffd_lock);
    for (userfaultfd_range_t *r = g_uffd_ranges; r; r = r->gnext) {
        if (r->mm == mm && (r->flags & UFFDIO_REGISTER_MODE_MISSING) &&
            page_va >= (uint64_t)r->start && page_va < (uint64_t)r->end) {
            spin_unlock_irqrestore(&g_uffd_lock, flags);
            return 1;
        }
    }
    spin_unlock_irqrestore(&g_uffd_lock, flags);
    return 0;
}

static void userfaultfd_enqueue_event_locked(userfaultfd_t *uffd,
                                             const uffd_msg_t *msg)
{
    if (uffd->event_count == uffd->event_cap) {
        uffd->event_head = (uffd->event_head + 1) % uffd->event_cap;
        uffd->event_count--;
    }
    uffd->events[uffd->event_tail] = *msg;
    uffd->event_tail = (uffd->event_tail + 1) % uffd->event_cap;
    uffd->event_count++;
}

static void userfaultfd_queue_pagefault(userfaultfd_t *uffd,
                                        uint64_t page_va, int write_access)
{
    uffd_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.event = UFFD_EVENT_PAGEFAULT;
    msg.arg.pagefault.flags = write_access ? UFFD_PAGEFAULT_FLAG_WRITE : 0;
    msg.arg.pagefault.address = page_va;
    msg.arg.pagefault.ptid = 0;

    spin_lock(&uffd->lock);
    userfaultfd_enqueue_event_locked(uffd, &msg);
    spin_unlock(&uffd->lock);
    wait_queue_wake_all(&uffd->readers, 0, PROC_WAKE_EVENT);
}

int userfaultfd_handle_fault(struct task_t *t, struct mm_struct *mm,
                             uint64_t page_va, int write_access)
{
    int first = 1;
    for (;;) {
        /* Resolved by the handler (COPY/ZEROPAGE) or by another faulter. */
        spin_lock(&mm->lock);
        pte_t *pte = pt_lookup_leaf(mm->pgdir, page_va, NULL, NULL, NULL);
        bool present = pte && (*pte & PTE_V);
        spin_unlock(&mm->lock);
        if (present)
            return 0;

        userfaultfd_t *uffd = userfaultfd_range_lookup(mm, page_va);
        if (!uffd)
            return 0; /* range unregistered: caller runs the normal fault */

        if (first) {
            userfaultfd_queue_pagefault(uffd, page_va, write_access);
            first = 0;
        }

        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_KILLABLE, 0);
        if (!token.task) {
            userfaultfd_put(uffd);
            return -1;
        }

        wait_queue_entry_t entry = {0};
        /* Link before the PTE recheck so a resolve that lands between our
         * present-check and the link cannot be missed: once linked, the
         * handler's wake finds us, and if the page is already mapped we
         * cancel and let the loop top return. */
        spin_lock(&uffd->lock);
        bool linked = wait_queue_link(&uffd->faulters, &entry, token, page_va);
        spin_unlock(&uffd->lock);

        spin_lock(&mm->lock);
        pte_t *pte2 = pt_lookup_leaf(mm->pgdir, page_va, NULL, NULL, NULL);
        bool resolved = pte2 && (*pte2 & PTE_V);
        spin_unlock(&mm->lock);

        proc_wake_reason_t reason;
        if (resolved) {
            if (linked) {
                (void)proc_park_cancel(token);
                reason = PROC_WAKE_CANCEL;
            } else {
                reason = PROC_WAKE_CANCEL;
            }
            proc_park_finish(token);
            wait_queue_unlink(&uffd->faulters, &entry);
            userfaultfd_put(uffd);
            continue;
        }
        if (!linked) {
            proc_park_finish(token);
            userfaultfd_put(uffd);
            continue;
        }
        reason = proc_park_commit(token);
        proc_park_finish(token);
        wait_queue_unlink(&uffd->faulters, &entry);
        userfaultfd_put(uffd);

        if (reason == PROC_WAKE_FATAL_SIGNAL ||
            reason == PROC_WAKE_TASK_EXIT)
            return -1;
        if (proc_wake_reason_is_task_interrupt(reason) &&
            signal_task_has_fatal(t))
            return -1;
    }
}

/* ---- fd operations ---- */

static int userfaultfd_read(vfile_t *vf, char *buf, size_t count)
{
    userfaultfd_t *uffd = vf ? vf->priv : NULL;
    if (!uffd)
        return -EBADF;
    if (count < sizeof(uffd_msg_t))
        return -EINVAL;

    size_t copied = 0;
    for (;;) {
        spin_lock(&uffd->lock);
        while (copied + sizeof(uffd_msg_t) <= count &&
               uffd->event_count > 0) {
            memcpy(buf + copied,
                   &uffd->events[uffd->event_head], sizeof(uffd_msg_t));
            uffd->event_head = (uffd->event_head + 1) % uffd->event_cap;
            uffd->event_count--;
            copied += sizeof(uffd_msg_t);
        }
        if (copied > 0) {
            spin_unlock(&uffd->lock);
            return (int)copied;
        }
        if (uffd->nonblock) {
            spin_unlock(&uffd->lock);
            return -EAGAIN;
        }
        spin_unlock(&uffd->lock);

        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task)
            return -EAGAIN;

        wait_queue_entry_t entry = {0};
        spin_lock(&uffd->lock);
        if (uffd->event_count > 0) {
            spin_unlock(&uffd->lock);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        bool linked = wait_queue_link(&uffd->readers, &entry, token, 0);
        spin_unlock(&uffd->lock);

        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&uffd->readers, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
    }
}

static int userfaultfd_poll(vfile_t *vf, short events)
{
    userfaultfd_t *uffd = vf ? vf->priv : NULL;
    if (!uffd)
        return POLLNVAL;
    short revents = 0;
    spin_lock(&uffd->lock);
    if ((events & POLLIN) && uffd->event_count > 0)
        revents |= POLLIN;
    spin_unlock(&uffd->lock);
    return revents;
}

static int userfaultfd_close(vfile_t *vf)
{
    userfaultfd_t *uffd = vf ? vf->priv : NULL;
    if (uffd) {
        vf->priv = NULL;
        userfaultfd_release_all_ranges(uffd);
        wait_queue_wake_all(&uffd->faulters, 0, PROC_WAKE_EVENT);
        userfaultfd_put(uffd);
    }
    return 0;
}

/* ---- ioctl implementations ---- */

static int uffd_io_api(userfaultfd_t *uffd, void *arg)
{
    if (!arg)
        return -EFAULT;
    uffdio_api_t api;
    if (copy_from_user(&api, arg, sizeof(api)) < 0)
        return -EFAULT;

    if (uffd->api_version) {
        /* Already negotiated: only re-query of the same version is allowed. */
        if (api.api != UFFD_API_VERSION)
            return -EINVAL;
    } else {
        if (api.api != UFFD_API_VERSION)
            return -EINVAL;
        uffd->api_version = UFFD_API_VERSION;
    }

    api.features = UFFD_API_FEATURES;
    api.ioctls = (1ULL << 0x3f) |      /* UFFDIO_API */
                 (1ULL << 0x00) |      /* UFFDIO_REGISTER */
                 (1ULL << 0x01) |      /* UFFDIO_UNREGISTER */
                 (1ULL << 0x02) |      /* UFFDIO_WAKE */
                 (1ULL << 0x03) |      /* UFFDIO_COPY */
                 (1ULL << 0x04);       /* UFFDIO_ZEROPAGE */
    if (copy_to_user(arg, &api, sizeof(api)) < 0)
        return -EFAULT;
    return 0;
}

static int uffd_io_register(userfaultfd_t *uffd, void *arg)
{
    if (!arg)
        return -EFAULT;
    uffdio_register_t reg;
    if (copy_from_user(&reg, arg, sizeof(reg)) < 0)
        return -EFAULT;

    if (reg.range.len == 0 || (reg.range.start & (PAGE_SIZE - 1)) ||
        (reg.range.len & (PAGE_SIZE - 1)))
        return -EINVAL;
    if (reg.mode & ~UFFDIO_REGISTER_MODE_MISSING)
        return -EINVAL;

    uint64_t start = reg.range.start;
    uint64_t end = start + reg.range.len;
    if (end < start || end > USER_VA_LIMIT)
        return -EINVAL;

    task_t *t = proc_current();
    if (!t || !t->mm)
        return -EINVAL;
    mm_struct_t *mm = t->mm;

    /* The whole range must be covered by anonymous private VMAs. */
    spin_lock(&mm->lock);
    uint64_t covered = 0;
    for (vm_area_t *vma = mm->mmap; vma; vma = vma->next) {
        if ((uint64_t)vma->end <= start || (uint64_t)vma->start >= end)
            continue;
        if ((vma->vm_flags & (VM_ANON | VM_FILE | VM_VMO | VM_SHARED)) !=
            VM_ANON) {
            spin_unlock(&mm->lock);
            return -EINVAL;
        }
        uint64_t a = (uint64_t)vma->start > start ? (uint64_t)vma->start : start;
        uint64_t b = (uint64_t)vma->end < end ? (uint64_t)vma->end : end;
        covered += b - a;
    }
    spin_unlock(&mm->lock);
    if (covered < reg.range.len)
        return -EINVAL;

    uint64_t gflags = spin_lock_irqsave(&g_uffd_lock);
    /* Reject overlap with an existing registration of the same uffd. */
    for (userfaultfd_range_t *r = uffd->ranges; r; r = r->next) {
        if ((uint64_t)r->start < end && (uint64_t)r->end > start) {
            spin_unlock_irqrestore(&g_uffd_lock, gflags);
            return -EEXIST;
        }
    }
    userfaultfd_range_t *range = kmalloc(sizeof(*range));
    if (!range) {
        spin_unlock_irqrestore(&g_uffd_lock, gflags);
        return -ENOMEM;
    }
    memset(range, 0, sizeof(*range));
    range->mm = mm_get(mm);
    range->start = (vaddr_t)start;
    range->end = (vaddr_t)end;
    range->flags = reg.mode;
    range->owner = userfaultfd_get(uffd);
    range->next = uffd->ranges;
    uffd->ranges = range;
    range->gnext = g_uffd_ranges;
    g_uffd_ranges = range;
    g_uffd_range_count++;
    spin_unlock_irqrestore(&g_uffd_lock, gflags);

    reg.ioctls = (1ULL << 0x01) | (1ULL << 0x02) |
                 (1ULL << 0x03) | (1ULL << 0x04);
    if (copy_to_user(arg, &reg, sizeof(reg)) < 0)
        return -EFAULT;
    return 0;
}

static int uffd_io_unregister(userfaultfd_t *uffd, void *arg)
{
    if (!arg)
        return -EFAULT;
    uffdio_range_t range;
    if (copy_from_user(&range, arg, sizeof(range)) < 0)
        return -EFAULT;

    if (range.len == 0 || (range.start & (PAGE_SIZE - 1)) ||
        (range.len & (PAGE_SIZE - 1)))
        return -EINVAL;
    uint64_t start = range.start;
    uint64_t end = start + range.len;
    if (end < start)
        return -EINVAL;

    uint64_t flags = spin_lock_irqsave(&g_uffd_lock);
    int removed = 0;
    userfaultfd_range_t *r = uffd->ranges;
    while (r) {
        userfaultfd_range_t *next = r->next;
        if ((uint64_t)r->start < end && (uint64_t)r->end > start) {
            userfaultfd_range_unlink(r);
            userfaultfd_drop_range(r);
            removed++;
        }
        r = next;
    }
    spin_unlock_irqrestore(&g_uffd_lock, flags);

    if (removed)
        wait_queue_wake_all(&uffd->faulters, 0, PROC_WAKE_EVENT);
    return removed ? 0 : -ENOENT;
}

static int uffd_io_wake(userfaultfd_t *uffd, void *arg)
{
    if (!arg)
        return -EFAULT;
    uffdio_range_t range;
    if (copy_from_user(&range, arg, sizeof(range)) < 0)
        return -EFAULT;
    if (range.len == 0 || (range.start & (PAGE_SIZE - 1)) ||
        (range.len & (PAGE_SIZE - 1)))
        return -EINVAL;
    for (uint64_t page = range.start; page < range.start + range.len;
         page += PAGE_SIZE)
        wait_queue_wake_all(&uffd->faulters, page, PROC_WAKE_EVENT);
    return 0;
}

static int uffd_io_copy(userfaultfd_t *uffd, void *arg)
{
    if (!arg)
        return -EFAULT;
    uffdio_copy_t c;
    if (copy_from_user(&c, arg, sizeof(c)) < 0)
        return -EFAULT;

    if (c.len == 0 || (c.dst & (PAGE_SIZE - 1)) ||
        (c.len & (PAGE_SIZE - 1)))
        return -EINVAL;
    if (c.mode & ~UFFDIO_COPY_MODE_DONTWAKE)
        return -EINVAL;

    /* src is copied per page through copy_from_user; Linux does not require
     * it to be page aligned. */

    /* Locate the registered range for dst; COPY applies to the registering mm. */
    uint64_t gflags = spin_lock_irqsave(&g_uffd_lock);
    userfaultfd_range_t *r = NULL;
    for (r = uffd->ranges; r; r = r->next) {
        if ((uint64_t)r->start <= c.dst && (uint64_t)r->end >= c.dst + c.len &&
            (r->flags & UFFDIO_REGISTER_MODE_MISSING))
            break;
    }
    userfaultfd_t *ref = NULL;
    mm_struct_t *mm = NULL;
    if (r) {
        ref = userfaultfd_get(uffd);
        mm = r->mm;
    }
    spin_unlock_irqrestore(&g_uffd_lock, gflags);
    if (!r || !mm || !ref)
        return -ENOENT;

    task_t *t = proc_current();
    uint64_t charged = 0;
    int ret = 0;
    for (uint64_t off = 0; off < c.len; off += PAGE_SIZE) {
        uint64_t dst = c.dst + off;

        spin_lock(&mm->lock);
        pte_t *pte = pt_lookup_leaf(mm->pgdir, dst, NULL, NULL, NULL);
        bool present = pte && (*pte & PTE_V);
        spin_unlock(&mm->lock);
        if (present) {
            ret = -EEXIST;
            break;
        }

        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) {
            ret = -ENOMEM;
            break;
        }
        if (t && cg_mem_charge(t->cgroup, 1) != 0) {
            frame_put(pfn);
            ret = -ENOMEM;
            break;
        }
        if (copy_from_user(pfn_to_virt(pfn),
                           (void *)(uintptr_t)(c.src + off), PAGE_SIZE) < 0) {
            if (t)
                cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            ret = -EFAULT;
            break;
        }

        spin_lock(&mm->lock);
        pte_t *pte2 = pt_lookup_leaf(mm->pgdir, dst, NULL, NULL, NULL);
        if (pte2 && (*pte2 & PTE_V)) {
            spin_unlock(&mm->lock);
            if (t)
                cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            ret = -EEXIST;
            break;
        }
        vm_area_t *vma = mm_find_vma(mm, dst);
        if (!vma) {
            spin_unlock(&mm->lock);
            if (t)
                cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            ret = -ENOENT;
            break;
        }
        int mr = pt_map(mm->pgdir, dst, pfn_to_phys(pfn), vma->pte_flags);
        if (mr < 0) {
            spin_unlock(&mm->lock);
            if (t)
                cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            ret = -EAGAIN;
            break;
        }
        mm->rss++;
        spin_unlock(&mm->lock);
        arch_tlb_flush_page_local(dst);
        charged++;
        c.copy += (int64_t)PAGE_SIZE;

        if (!(c.mode & UFFDIO_COPY_MODE_DONTWAKE))
            wait_queue_wake_all(&uffd->faulters, dst, PROC_WAKE_EVENT);
    }
    userfaultfd_put(ref);

    if (copy_to_user(arg, &c, sizeof(c)) < 0)
        return -EFAULT;
    return ret;
}

static int uffd_io_zeropage(userfaultfd_t *uffd, void *arg)
{
    if (!arg)
        return -EFAULT;
    uffdio_zeropage_t z;
    if (copy_from_user(&z, arg, sizeof(z)) < 0)
        return -EFAULT;

    if (z.range.len == 0 || (z.range.start & (PAGE_SIZE - 1)) ||
        (z.range.len & (PAGE_SIZE - 1)))
        return -EINVAL;
    if (z.mode & ~UFFDIO_ZEROPAGE_MODE_DONTWAKE)
        return -EINVAL;

    uint64_t gflags = spin_lock_irqsave(&g_uffd_lock);
    userfaultfd_range_t *r = NULL;
    for (r = uffd->ranges; r; r = r->next) {
        if ((uint64_t)r->start <= z.range.start &&
            (uint64_t)r->end >= z.range.start + z.range.len &&
            (r->flags & UFFDIO_REGISTER_MODE_MISSING))
            break;
    }
    mm_struct_t *mm = r ? r->mm : NULL;
    spin_unlock_irqrestore(&g_uffd_lock, gflags);
    if (!r || !mm)
        return -ENOENT;

    task_t *t = proc_current();
    int ret = 0;
    for (uint64_t off = 0; off < z.range.len; off += PAGE_SIZE) {
        uint64_t dst = z.range.start + off;

        spin_lock(&mm->lock);
        pte_t *pte = pt_lookup_leaf(mm->pgdir, dst, NULL, NULL, NULL);
        bool present = pte && (*pte & PTE_V);
        spin_unlock(&mm->lock);
        if (present) {
            ret = -EEXIST;
            break;
        }

        pfn_t pfn = pfa_alloc_page();
        if (pfn == PFN_NONE) {
            ret = -ENOMEM;
            break;
        }
        if (t && cg_mem_charge(t->cgroup, 1) != 0) {
            frame_put(pfn);
            ret = -ENOMEM;
            break;
        }
        memset(pfn_to_virt(pfn), 0, PAGE_SIZE);

        spin_lock(&mm->lock);
        pte_t *pte2 = pt_lookup_leaf(mm->pgdir, dst, NULL, NULL, NULL);
        if (pte2 && (*pte2 & PTE_V)) {
            spin_unlock(&mm->lock);
            if (t)
                cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            ret = -EEXIST;
            break;
        }
        vm_area_t *vma = mm_find_vma(mm, dst);
        if (!vma) {
            spin_unlock(&mm->lock);
            if (t)
                cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            ret = -ENOENT;
            break;
        }
        int mr = pt_map(mm->pgdir, dst, pfn_to_phys(pfn), vma->pte_flags);
        if (mr < 0) {
            spin_unlock(&mm->lock);
            if (t)
                cg_mem_uncharge(t->cgroup, 1);
            frame_put(pfn);
            ret = -EAGAIN;
            break;
        }
        mm->rss++;
        spin_unlock(&mm->lock);
        arch_tlb_flush_page_local(dst);
        z.zeropage += (int64_t)PAGE_SIZE;

        if (!(z.mode & UFFDIO_ZEROPAGE_MODE_DONTWAKE))
            wait_queue_wake_all(&uffd->faulters, dst, PROC_WAKE_EVENT);
    }

    if (copy_to_user(arg, &z, sizeof(z)) < 0)
        return -EFAULT;
    return ret;
}

static int userfaultfd_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    userfaultfd_t *uffd = vf ? vf->priv : NULL;
    if (!uffd)
        return -EBADF;
    switch (req & 0xffffffffUL) {
    case UFFDIO_API:
        return uffd_io_api(uffd, arg);
    case UFFDIO_REGISTER:
        return uffd_io_register(uffd, arg);
    case UFFDIO_UNREGISTER:
        return uffd_io_unregister(uffd, arg);
    case UFFDIO_WAKE:
        return uffd_io_wake(uffd, arg);
    case UFFDIO_COPY:
        return uffd_io_copy(uffd, arg);
    case UFFDIO_ZEROPAGE:
        return uffd_io_zeropage(uffd, arg);
    default:
        return -EINVAL;
    }
}

static vfile_ops_t g_userfaultfd_ops = {
    .read = userfaultfd_read,
    .poll = userfaultfd_poll,
    .ioctl = userfaultfd_ioctl,
    .close = userfaultfd_close,
};

int userfaultfd_create_file(unsigned flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK | 1)) /* 1 = UFFD_USER_MODE_ONLY */
        return -EINVAL;

    userfaultfd_t *uffd = kmalloc(sizeof(*uffd));
    if (!uffd)
        return -ENOMEM;
    memset(uffd, 0, sizeof(*uffd));
    spin_init(&uffd->lock);
    spin_set_debug(&uffd->lock, "userfaultfd", uffd);
    refcount_set(&uffd->refcount, 1);
    wait_queue_init(&uffd->readers);
    wait_queue_init(&uffd->faulters);
    uffd->nonblock = (flags & O_NONBLOCK) != 0;
    uffd->event_cap = UFFD_EVENT_CAPACITY;
    uffd->events = kcalloc(uffd->event_cap, sizeof(uffd_msg_t));
    if (!uffd->events) {
        kfree(uffd);
        return -ENOMEM;
    }

    vfile_t *vf = vfile_alloc();
    if (!vf) {
        kfree(uffd->events);
        kfree(uffd);
        return -ENOMEM;
    }
    vf->flags = O_RDWR | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_userfaultfd_ops;
    vf->priv = uffd;
    return anonfd_install_vfile(vf, flags);
}

int64_t sys_userfaultfd(unsigned flags)
{
    return userfaultfd_create_file(flags);
}
