/*
 * A20OS Native ABI — user-space driver (udriver) syscalls.
 *
 * Design reference: docs/hybrid-kernel/00-design.md §3.2.
 * The kernel authorizes device windows and delivers IRQs to event
 * queues; device logic lives in user tasks.
 */
#include "core/types.h"
#include "core/string.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/vmo.h"
#include "sys_validate.h"
#include "abi/native/ipc_internal.h"
#include "drivers/core/udriver.h"
#include "mm/frame.h"
#include "ipc/objstats.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                               a20_handle_t h,
                                               uint16_t expected_type,
                                               a20_rights_t required_rights,
                                               a20_handle_entry_t *out);
extern void a20_object_release(void *object, uint16_t type);
extern void a20_object_ref(void *object, uint16_t type);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);

int64_t sys_a20_device_map_mmio(const a20_syscall_args_t *args)
{
    a20_device_map_mmio_args_t *uargs =
        (a20_device_map_mmio_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_device_map_mmio_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    if (!cur || !cur->mm) return -A20_ERR_BAD_HANDLE;

    extern int udriver_mmio_user_owned(uint64_t phys);
    extern int udriver_claim_owner(uint64_t phys);
    if (udriver_mmio_user_owned(kargs.phys_base) &&
        udriver_claim_owner(kargs.phys_base) != cur->pid)
        return -A20_ERR_ACCESS;

    uint64_t va = 0;
    if (udriver_map_mmio(cur->mm, kargs.phys_base, kargs.length,
                         kargs.prot, &va) < 0)
        return -A20_ERR_ACCESS;

    kargs.out_addr = va;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_device_irq_listen(const a20_syscall_args_t *args)
{
    a20_device_irq_listen_args_t *uargs =
        (a20_device_irq_listen_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_device_irq_listen_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.queue,
                                               A20_OBJ_EVENT_QUEUE,
                                               A20_RIGHT_READ, &entry);
    if (r < 0) return r;

    a20_eventq_t *eq = (a20_eventq_t *)entry.object;
    if (udriver_irq_listen(kargs.irq, eq, kargs.user_data, cur->pid) < 0) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_ACCESS;
    }

    /* Bind the IRQ key to the queue so SIGNALED events are delivered. */
    int64_t wr = a20_eventq_watch(eq, (a20_handle_t)kargs.irq,
                                  (void *)(uintptr_t)kargs.irq,
                                  A20_OBJ_DEVICE,
                                  A20_EVENT_MASK(A20_EVENT_SIGNALED),
                                  kargs.user_data);
    a20_object_release(entry.object, entry.type);
    if (wr < 0) {
        udriver_irq_unlisten(kargs.irq);
        return wr;
    }
    return A20_OK;
}

int64_t sys_a20_device_irq_ack(const a20_syscall_args_t *args)
{
    uint32_t irq = (uint32_t)A20_ARG(0);
    if (udriver_irq_ack(irq) < 0)
        return -A20_ERR_BAD_HANDLE;
    return A20_OK;
}

int64_t sys_a20_device_irq_unlisten(const a20_syscall_args_t *args)
{
    uint32_t irq = (uint32_t)A20_ARG(0);
    if (udriver_irq_unlisten(irq) < 0)
        return -A20_ERR_BAD_HANDLE;
    return A20_OK;
}

/*
 * sys_a20_device_vmo_phys — DMA contract (M4): a user driver never
 * supplies raw physical addresses; it allocates a VMO through the normal
 * memory syscalls, materializes the pages, and asks the kernel for their
 * physical addresses here.  VMO pages are never paged out, which provides
 * the pin guarantee for DMA descriptors.
 */int64_t sys_a20_device_vmo_phys(const a20_syscall_args_t *args)
{
    a20_device_vmo_phys_args_t *uargs =
        (a20_device_vmo_phys_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_device_vmo_phys_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.vmo,
                                               A20_OBJ_MEMORY,
                                               A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    struct vmo *v = (struct vmo *)entry.object;
    uint32_t n = v->page_count;
    if (n > kargs.max_pages)
        n = kargs.max_pages;
    for (uint32_t i = 0; i < n; i++) {
        pfn_t pfn = vmo_peek_page(v, i);
        uint64_t pa = (pfn == PFN_NONE) ? 0 : (uint64_t)pfn_to_phys(pfn);
        if (copy_to_user((void *)(kargs.out_paddrs +
                                  (uint64_t)i * sizeof(uint64_t)),
                         &pa, sizeof(pa)) < 0) {
            a20_object_release(entry.object, entry.type);
            return -A20_ERR_FAULT;
        }
    }
    kargs.out_count = n;
    a20_object_release(entry.object, entry.type);
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

typedef struct a20_device_block_attach_args {
    uint32_t       size;
    uint32_t       version;
    a20_handle_t   ring_vmo;      /* one-page MEMORY handle */
    uint32_t       _pad;
    uint64_t       capacity;      /* sectors */
    uint64_t       out_doorbell;  /* out: channel endpoint handle */
} a20_device_block_attach_args_t;

extern int udisk_attach(struct vmo *vmo, uint64_t capacity,
                        a20_channel_ep_t **out_doorbell, int owner_pid);

int64_t sys_a20_device_block_attach(const a20_syscall_args_t *args)
{
    a20_device_block_attach_args_t *uargs =
        (a20_device_block_attach_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_device_block_attach_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.ring_vmo,
                                               A20_OBJ_MEMORY,
                                               A20_RIGHT_READ | A20_RIGHT_WRITE,
                                               &entry);
    if (r < 0) return r;
    struct vmo *vmo = (struct vmo *)entry.object;

    a20_channel_ep_t *ubd_ep = NULL;
    r = udisk_attach(vmo, kargs.capacity, &ubd_ep, cur->pid);
    a20_object_release(entry.object, entry.type);
    if (r < 0 || !ubd_ep)
        return -A20_ERR_ACCESS;

    /* Hand the driver its doorbell endpoint (a new handle + ref). */
    a20_object_ref(ubd_ep, A20_OBJ_CHANNEL_ENDPOINT);
    int64_t h = a20_handle_install(ht, ubd_ep, A20_OBJ_CHANNEL_ENDPOINT,
                                   A20_RIGHT_READ | A20_RIGHT_WRITE);
    if (h < 0) {
        a20_object_release(ubd_ep, A20_OBJ_CHANNEL_ENDPOINT);
        return h;
    }
    kargs.out_doorbell = (uint64_t)h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

extern int udisk_complete(int pid, uint32_t n_done);

int64_t sys_a20_device_block_complete(const a20_syscall_args_t *args)
{
    uint32_t n_done = (uint32_t)A20_ARG(0);
    task_t *cur = proc_current();
    if (!cur)
        return -A20_ERR_BAD_HANDLE;
    return udisk_complete(cur->pid, n_done) < 0
        ? -A20_ERR_BAD_HANDLE : A20_OK;
}

int64_t sys_a20_device_claim(const a20_syscall_args_t *args)
{
    uint64_t phys = A20_ARG(0);
    task_t *cur = proc_current();
    if (!cur)
        return -A20_ERR_BAD_HANDLE;
    extern int udriver_claim(uint64_t phys, int pid);
    int r = udriver_claim(phys, cur->pid);
    if (r == -2)
        return -A20_ERR_ACCESS;      /* not a user-claimable window */
    if (r < 0)
        return -A20_ERR_BUSY;        /* claimed by another task */
    return A20_OK;
}

int64_t sys_a20_device_release(const a20_syscall_args_t *args)
{
    uint64_t phys = A20_ARG(0);
    task_t *cur = proc_current();
    if (!cur)
        return -A20_ERR_BAD_HANDLE;
    extern int udriver_release(uint64_t phys, int pid);
    int r = udriver_release(phys, cur->pid);
    if (r == -2)
        return -A20_ERR_ACCESS;
    if (r < 0)
        return -A20_ERR_PERM;        /* not the owner */
    return A20_OK;
}

/* Contiguous DMA heap (04-dual-placement.md): allocate order-N frames
 * and hand them to the driver as a pre-materialized VMO, so the phys
 * table from vmo_phys is guaranteed contiguous — closing the gap with
 * the kernel drv_dma backend. */
int64_t sys_a20_device_alloc_dma(const a20_syscall_args_t *args)
{
    uint64_t size = A20_ARG(0);
    if (size == 0 || size > 64 * PAGE_SIZE)
        return -A20_ERR_INVALID_ARGUMENT;
    uint32_t npages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
    unsigned order = 0;
    while ((1u << order) < npages)
        order++;

    pfn_t pfn = pfa_alloc((int)order);
    if (pfn == PFN_NONE)
        return -A20_ERR_NO_MEMORY;

    struct vmo *vmo = vmo_create(VMO_ANONYMOUS, (uint64_t)npages * PAGE_SIZE, 0);
    if (!vmo) {
        pfa_free(pfn, (int)order);
        return -A20_ERR_NO_MEMORY;
    }

    /* Pre-materialize with the contiguous block; frames are zeroed so
     * the device never reads stale kernel data. */
    memset(pfn_to_virt(pfn), 0, (uint64_t)npages * PAGE_SIZE);
    spin_lock(&vmo->lock);
    for (uint32_t i = 0; i < npages; i++)
        vmo->pages[i] = pfn + i;
    vmo->phys_size = (uint64_t)npages * PAGE_SIZE;
    a20_objstat_add(&g_a20_objstats.vmo_pages, npages);
    spin_unlock(&vmo->lock);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) {
        vmo_release(vmo);
        return -A20_ERR_BAD_HANDLE;
    }

    int64_t h = a20_handle_install(ht, vmo, A20_OBJ_MEMORY,
                                   A20_RIGHT_READ | A20_RIGHT_WRITE |
                                   A20_RIGHT_MAP | A20_RIGHT_STAT | A20_RIGHT_DUP |
                                   A20_RIGHT_TRANSFER | A20_RIGHT_CONTROL);
    if (h < 0)
        vmo_release(vmo);
    return h;
}
