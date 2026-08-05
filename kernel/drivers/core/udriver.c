/*
 * A20OS user-space driver (udriver) support — kernel side.
 * See kernel/include/drivers/core/udriver.h for the design.
 */
#include "core/types.h"
#include "core/string.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/refcount.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/slab.h"
#include "drivers/core/driver_hwapi.h"
#include "drivers/core/udriver.h"
#include "abi/native/types.h"
#include "abi/native/ipc_internal.h"
#include "ipc/objstats.h"

/* ---- Whitelisted user-mappable MMIO windows ---- */

typedef struct {
    uint64_t    base;
    uint64_t    size;
    const char *name;
    uint8_t     user_owned;  /* kernel probe must skip this device */
} udriver_mmio_window_t;

static const udriver_mmio_window_t g_mmio_windows[] = {
#ifdef CONFIG_BOARD_QEMU_VIRT_RISCV64
    { 0x101000, 0x1000, "goldfish-rtc", 1 },
    /* virtio-mmio slot 3: reserved for the user-space virtio-blk pilot
     * (docs/hybrid-kernel/02-mainstream-plan.md M4).  The kernel's
     * virtio_mmio_enumerate() skips user-owned slots. */
    { 0x10004000, 0x1000, "virtio-blk-user", 1 },
#endif
};

#ifdef CONFIG_BOARD_QEMU_VIRT_RISCV64
#define UDRIVER_MMIO_WINDOWS_NR \
    (sizeof(g_mmio_windows) / sizeof(g_mmio_windows[0]))
#else
#define UDRIVER_MMIO_WINDOWS_NR 0
#endif

static int udriver_mmio_allowed(uint64_t phys, uint64_t size)
{
    if (size == 0 || phys + size < phys)
        return 0;
    for (unsigned i = 0; i < UDRIVER_MMIO_WINDOWS_NR; i++) {
        uint64_t wb = g_mmio_windows[i].base;
        uint64_t we = wb + g_mmio_windows[i].size;
        if (phys >= wb && phys + size <= we)
            return 1;
    }
    return 0;
}

/* Board device enumeration consults this before binding a kernel driver
 * to a device that belongs to a user-space driver. */
int udriver_mmio_user_owned(uint64_t phys)
{
    for (unsigned i = 0; i < UDRIVER_MMIO_WINDOWS_NR; i++)
        if (g_mmio_windows[i].user_owned &&
            phys >= g_mmio_windows[i].base &&
            phys < g_mmio_windows[i].base + g_mmio_windows[i].size)
            return 1;
    return 0;
}

int udriver_map_mmio(mm_struct_t *mm, uint64_t phys, uint64_t size,
                     uint32_t prot, uint64_t *out_va)
{
    if (!mm || !mm->pgdir || !out_va)
        return -1;
    if (!udriver_mmio_allowed(phys, size)) {
        klog(KLOG_WARN, "udriver: mmio window 0x%lx+%lu not whitelisted\n",
             (unsigned long)phys, (unsigned long)size);
        return -1;
    }
    if (phys & (PAGE_SIZE - 1))
        return -1;
    size = (size + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);

    pte_t ptef = PTE_V | PTE_U | PTE_A | PTE_LEAF;
    uint64_t vma_flags = VM_SHARED | VM_PFNMAP | VM_DONTFORK;
    if (prot & 1) { ptef |= PTE_R; vma_flags |= VM_READ; }
    if (prot & 2) { ptef |= PTE_W | PTE_D | PTE_R; vma_flags |= VM_WRITE; }
    if (!(ptef & (PTE_R | PTE_W)))
        return -1;

    uint64_t lock_flags = spin_lock_irqsave(&mm->lock);
    vaddr_t va = mm_find_gap(mm, mm->mmap_base, size);
    if (!va)
        goto fail;
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        if (pt_map(mm->pgdir, va + off, phys + off, ptef) < 0)
            goto fail;
    }
    vm_area_t *vma = kcalloc(1, sizeof(*vma));
    if (!vma)
        goto fail;
    vma->start = va;
    vma->end = va + size;
    vma->vm_flags = vma_flags;
    mm_insert_vma(mm, vma);
    spin_unlock_irqrestore(&mm->lock, lock_flags);
    *out_va = va;
    return 0;

fail:
    spin_unlock_irqrestore(&mm->lock, lock_flags);
    return -1;
}

/* ---- IRQ → event queue delivery ---- */

#define UDRIVER_IRQ_MAX 8

typedef struct {
    uint32_t         irq;
    a20_eventq_t    *queue;      /* referenced while registered */
    uint64_t         user_data;
    int              owner_pid;
    uint8_t          active;
} udriver_irq_entry_t;

static spinlock_t          g_udr_lock = SPINLOCK_INIT;
static udriver_irq_entry_t g_udr_irq[UDRIVER_IRQ_MAX];

static int udriver_irq_thunk(int irq, void *priv)
{
    udriver_irq_entry_t *e = (udriver_irq_entry_t *)priv;
    /* Mask at the irqchip first so a level-triggered line cannot storm
     * while the user handler works; it re-arms via udriver_irq_ack(). */
    irq_disable((uint32_t)irq);
    if (e->active)
        a20_event_notify((void *)(uintptr_t)e->irq, A20_OBJ_DEVICE,
                         A20_EVENT_SIGNALED, e->user_data, 0);
    return 0;
}

int udriver_irq_listen(uint32_t irq, a20_eventq_t *queue,
                       uint64_t user_data, int owner_pid)
{
    if (!queue || irq == 0 || irq >= 1024)
        return -1;

    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    udriver_irq_entry_t *slot = NULL;
    for (unsigned i = 0; i < UDRIVER_IRQ_MAX; i++) {
        if (g_udr_irq[i].active && g_udr_irq[i].irq == irq) {
            spin_unlock_irqrestore(&g_udr_lock, flags);
            return -1; /* already bound */
        }
        if (!g_udr_irq[i].active && !slot)
            slot = &g_udr_irq[i];
    }
    if (!slot) {
        spin_unlock_irqrestore(&g_udr_lock, flags);
        return -1;
    }

    slot->irq = irq;
    slot->queue = queue;
    slot->user_data = user_data;
    slot->owner_pid = owner_pid;
    slot->active = 1;
    a20_objstat_add(&g_a20_objstats.irq_bindings, 1);
    spin_unlock_irqrestore(&g_udr_lock, flags);

    refcount_inc(&queue->refcount);
    if (request_irq(irq, udriver_irq_thunk, 0, slot) < 0) {
        flags = spin_lock_irqsave(&g_udr_lock);
        slot->active = 0;
        spin_unlock_irqrestore(&g_udr_lock, flags);
        a20_eventq_release(queue);
        return -1;
    }
    return 0;
}

static udriver_irq_entry_t *udriver_find_irq(uint32_t irq)
{
    for (unsigned i = 0; i < UDRIVER_IRQ_MAX; i++)
        if (g_udr_irq[i].active && g_udr_irq[i].irq == irq)
            return &g_udr_irq[i];
    return NULL;
}

int udriver_irq_ack(uint32_t irq)
{
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    udriver_irq_entry_t *e = udriver_find_irq(irq);
    spin_unlock_irqrestore(&g_udr_lock, flags);
    if (!e)
        return -1;
    irq_enable(irq);
    return 0;
}

int udriver_irq_unlisten(uint32_t irq)
{
    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    udriver_irq_entry_t *e = udriver_find_irq(irq);
    a20_eventq_t *queue = NULL;
    if (e) {
        e->active = 0;
        queue = e->queue;
        e->queue = NULL;
        a20_objstat_add(&g_a20_objstats.irq_bindings, -1);
    }
    spin_unlock_irqrestore(&g_udr_lock, flags);
    if (!e)
        return -1;
    free_irq(irq, e);
    if (queue)
        a20_eventq_release(queue);
    return 0;
}

void udriver_task_cleanup(int pid)
{
    a20_eventq_t *dead[UDRIVER_IRQ_MAX];
    uint32_t irqs[UDRIVER_IRQ_MAX];
    void *privs[UDRIVER_IRQ_MAX];
    unsigned n = 0;

    uint64_t flags = spin_lock_irqsave(&g_udr_lock);
    for (unsigned i = 0; i < UDRIVER_IRQ_MAX; i++) {
        if (g_udr_irq[i].active && g_udr_irq[i].owner_pid == pid) {
            g_udr_irq[i].active = 0;
            dead[n] = g_udr_irq[i].queue;
            irqs[n] = g_udr_irq[i].irq;
            privs[n] = &g_udr_irq[i];
            g_udr_irq[i].queue = NULL;
            a20_objstat_add(&g_a20_objstats.irq_bindings, -1);
            n++;
        }
    }
    spin_unlock_irqrestore(&g_udr_lock, flags);

    for (unsigned i = 0; i < n; i++) {
        free_irq(irqs[i], privs[i]);
        if (dead[i])
            a20_eventq_release(dead[i]);
    }
}
