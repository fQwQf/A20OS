#include "mm/shutdown_write.h"

#include "core/errno.h"
#include "core/panic.h"
#include "core/preempt.h"
#include "core/string.h"
#include "core/sync.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/pt.h"
#include "mm/slab.h"
#include "fs/page_cache.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"

/* A grant that faults during pre-teardown quiescence sleeps here. Abort or
 * terminal commit wakes it; no MM/page-cache lock is held while parked. */
static wait_queue_t mm_shared_write_waiters = WAIT_QUEUE_INIT;

#ifdef CONFIG_MM_SHUTDOWN_WRITE_RUST
extern int a20_mm_shared_write_enter_rust(void);
extern int a20_mm_shared_write_leave_rust(void);
extern int a20_mm_shared_write_is_committed_rust(void);
extern int a20_mm_shared_write_is_closed_rust(void);
extern int a20_mm_shared_write_begin_rust(uint64_t *token_out);
extern int a20_mm_shared_write_drained_rust(uint64_t token);
extern int a20_mm_shared_write_abort_rust(uint64_t token);
extern int a20_mm_shared_write_commit_rust(uint64_t token);
#else
#define MM_SW_CLOSED    (1u << 31)
#define MM_SW_COMMITTED (1u << 30)
#define MM_SW_COUNT     (MM_SW_COMMITTED - 1)
static volatile uint32_t mm_sw_c_state;
static volatile uint32_t mm_sw_c_generation;
static volatile uint32_t mm_sw_c_control;

static int mm_sw_c_lock(void)
{
    uint32_t expected = 0;
    return __atomic_compare_exchange_n(&mm_sw_c_control, &expected, 1, 0,
                                        __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

static void mm_sw_c_unlock(void)
{
    __atomic_store_n(&mm_sw_c_control, 0, __ATOMIC_RELEASE);
}

static int mm_sw_c_valid(uint64_t token)
{
    return token && token <= UINT32_MAX &&
        __atomic_load_n(&mm_sw_c_generation, __ATOMIC_ACQUIRE) == token &&
        (__atomic_load_n(&mm_sw_c_state, __ATOMIC_ACQUIRE) & MM_SW_CLOSED);
}

int a20_mm_shared_write_enter_rust(void)
{
    uint32_t old = __atomic_load_n(&mm_sw_c_state, __ATOMIC_ACQUIRE);
    for (;;) {
        if (old & MM_SW_CLOSED) return 0;
        if ((old & MM_SW_COUNT) == MM_SW_COUNT) return -EOVERFLOW;
        if (__atomic_compare_exchange_n(&mm_sw_c_state, &old, old + 1, 1,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return 1;
    }
}

int a20_mm_shared_write_leave_rust(void)
{
    uint32_t old = __atomic_load_n(&mm_sw_c_state, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!(old & MM_SW_COUNT) || (old & MM_SW_COMMITTED)) return -EINVAL;
        if (__atomic_compare_exchange_n(&mm_sw_c_state, &old, old - 1, 1,
                                         __ATOMIC_RELEASE, __ATOMIC_ACQUIRE))
            return 0;
    }
}

int a20_mm_shared_write_is_committed_rust(void)
{
    return !!(__atomic_load_n(&mm_sw_c_state, __ATOMIC_ACQUIRE) & MM_SW_COMMITTED);
}
int a20_mm_shared_write_is_closed_rust(void)
{
    return !!(__atomic_load_n(&mm_sw_c_state, __ATOMIC_ACQUIRE) & MM_SW_CLOSED);
}

int a20_mm_shared_write_begin_rust(uint64_t *token)
{
    if (!token) return -EINVAL;
    if (!mm_sw_c_lock()) return -EBUSY;
    uint32_t gen = __atomic_load_n(&mm_sw_c_generation, __ATOMIC_RELAXED);
    if (gen == UINT32_MAX) { mm_sw_c_unlock(); return -EOVERFLOW; }
    uint32_t old = __atomic_load_n(&mm_sw_c_state, __ATOMIC_ACQUIRE);
    for (;;) {
        if (old & MM_SW_CLOSED) { mm_sw_c_unlock(); return -EBUSY; }
        if (__atomic_compare_exchange_n(&mm_sw_c_state, &old, old | MM_SW_CLOSED,
                                         1, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            break;
    }
    gen++;
    __atomic_store_n(&mm_sw_c_generation, gen, __ATOMIC_RELEASE);
    *token = gen;
    mm_sw_c_unlock();
    return 0;
}

int a20_mm_shared_write_drained_rust(uint64_t token)
{
    if (!mm_sw_c_lock()) return 0;
    int result = !mm_sw_c_valid(token) ? -EINVAL :
        ((__atomic_load_n(&mm_sw_c_state, __ATOMIC_ACQUIRE) & MM_SW_COUNT) == 0);
    mm_sw_c_unlock();
    return result;
}

int a20_mm_shared_write_abort_rust(uint64_t token)
{
    if (!mm_sw_c_lock()) return -EBUSY;
    int result = -EINVAL;
    if (mm_sw_c_valid(token)) {
        uint32_t expected = MM_SW_CLOSED;
        result = __atomic_compare_exchange_n(&mm_sw_c_state, &expected, 0, 0,
                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 0 : -EINVAL;
    }
    mm_sw_c_unlock();
    return result;
}

int a20_mm_shared_write_commit_rust(uint64_t token)
{
    if (!mm_sw_c_lock()) return -EBUSY;
    int result = -EINVAL;
    if (mm_sw_c_valid(token)) {
        uint32_t expected = MM_SW_CLOSED;
        result = __atomic_compare_exchange_n(&mm_sw_c_state, &expected,
                    MM_SW_CLOSED | MM_SW_COMMITTED, 0,
                    __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) ? 0 : -EINVAL;
    }
    mm_sw_c_unlock();
    return result;
}
#endif

int mm_shared_write_enter(void)
{
    return a20_mm_shared_write_enter_rust();
}

static void mm_shared_write_wake_waiters(void)
{
    (void)wait_queue_wake_all(&mm_shared_write_waiters, 0, PROC_WAKE_EVENT);
}

static int mm_shared_write_wait_open(void)
{
    for (;;) {
        if (!mm_shared_write_is_closed())
            return 0;
        if (mm_shared_write_is_committed())
            return -EIO;

        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        if (!token.task) {
            proc_yield();
            continue;
        }

        wait_queue_entry_t entry = {0};
        uint64_t flags = spin_lock_irqsave(&mm_shared_write_waiters.lock);
        if (!mm_shared_write_is_closed() || mm_shared_write_is_committed()) {
            spin_unlock_irqrestore(&mm_shared_write_waiters.lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        bool linked = wait_queue_link_locked(&mm_shared_write_waiters,
                                              &entry, token, 0);
        spin_unlock_irqrestore(&mm_shared_write_waiters.lock, flags);
        if (linked)
            (void)proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&mm_shared_write_waiters, &entry);
        proc_park_finish(token);
    }
}

int mm_shared_write_enter_wait(void)
{
    /* This helper can park while shutdown owns admission. Every caller must
     * arrive outside hardirq/preempt-disabled context and with MM/PTE/page
     * cache/VFS locks released; waiting under them would deadlock the revoker.
     * Synchronous user-trap callers may have local IRQs masked by trap entry. */
    if (preempt_count() != 0 || in_hardirq())
        panic("shared-write admission wait in atomic context");

    for (;;) {
        int entered = mm_shared_write_enter();
        if (entered > 0)
            return 0;
        if (entered < 0)
            return entered;
        if (mm_shared_write_is_committed())
            return -EIO;
        if (!mm_shared_write_is_closed())
            continue; /* Abort raced the failed admission attempt. */
        int r = mm_shared_write_wait_open();
        if (r < 0)
            return r;
    }
}

void mm_shared_write_leave(void)
{
    if (a20_mm_shared_write_leave_rust() < 0)
        panic("mm shared-write admission underflow");
}

int mm_shared_write_is_committed(void)
{
    return a20_mm_shared_write_is_committed_rust();
}

int mm_shared_write_is_closed(void)
{
    return a20_mm_shared_write_is_closed_rust();
}

int mm_shared_write_shutdown_begin(uint64_t *token)
{
    if (!token || ((uintptr_t)token & (_Alignof(uint64_t) - 1)))
        return -EINVAL;
    return a20_mm_shared_write_begin_rust(token);
}

int mm_shared_write_shutdown_drained(uint64_t token)
{
    return a20_mm_shared_write_drained_rust(token);
}

int mm_shared_write_shutdown_abort(uint64_t token)
{
    int r = a20_mm_shared_write_abort_rust(token);
    if (r == 0)
        mm_shared_write_wake_waiters();
    return r;
}

int mm_shared_write_shutdown_commit(uint64_t token)
{
    int r = a20_mm_shared_write_commit_rust(token);
    if (r == 0)
        mm_shared_write_wake_waiters();
    return r;
}

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
static int mm_shutdown_shared_file_vma(const mm_seg_t *vma)
{
    return vma && (vma->vm_flags & (VM_FILE | VM_SHARED)) ==
                      (VM_FILE | VM_SHARED) && vma->file_vnode;
}

/* Hardware may set accessed/dirty bits without taking the software node lock.
 * Use an atomic RMW loop so a concurrent hardware update is folded into the
 * value from which write permission is removed.  A plain load/store here can
 * erase a dirty bit set between the two operations. */
static int mm_shutdown_clear_leaf_write(pte_t *pte, pte_t *new_pte)
{
    pte_t old = __atomic_load_n(pte, __ATOMIC_ACQUIRE);
    for (;;) {
        if (!(old & PTE_W)) {
            *new_pte = old;
            return 0;
        }
        pte_t flags = arch_pte_flags(old) & ~(pte_t)PTE_W;
        pte_t desired = arch_pte_leaf(arch_pte_addr(old), flags);
        if (__atomic_compare_exchange_n(pte, &old, desired, 0,
                                         __ATOMIC_ACQ_REL,
                                         __ATOMIC_ACQUIRE)) {
            *new_pte = desired;
            return 1;
        }
        /* On failure old contains the hardware-updated current entry. */
    }
}

/* Some supported page-table formats cannot retain a dirty indication once W
 * is cleared: AArch64 maps PTE_D to PTE_W, and ARM32 has no PTE_D bit.  For
 * those targets conservatively dirty every page-cache page covered by a
 * writable shared-file leaf before revoking it.  The later shootdown ensures
 * stale writable translations are gone before writeback begins. */
#if PTE_D == 0 || PTE_D == PTE_W
static int mm_shutdown_mark_leaf_dirty(mm_seg_t *vma, vaddr_t base,
                                       size_t size)
{
    vaddr_t start = base > vma->start ? base : vma->start;
    uint64_t leaf_end = (uint64_t)base + size;
    vaddr_t end = leaf_end < vma->end ? (vaddr_t)leaf_end : vma->end;
    if (end <= start)
        return 0;
    uint64_t page = (vma->backing_offset + (start - vma->start)) / PAGE_SIZE;
    for (vaddr_t va = start; va < end; va += PAGE_SIZE, page++) {
        page_cache_page_t *cached = page_cache_get(vma->file_vnode, page, 0);
        if (!cached)
            return -EIO;
        page_cache_mark_dirty(cached);
        page_cache_put(cached);
    }
    return 0;
}
#endif

/* Clear write permission under the leaf-table lock, preserving PTE_D.  The
 * dirty bit remains available until the synchronous shootdown has waited out
 * CPUs that could still write through stale translations. */
static int mm_shutdown_revoke_mm(mm_struct_t *mm)
{
    int result = 0;
    mm_tlb_invalidate_begin(mm);
    spin_lock(&mm->lock);
    for (mm_seg_t *vma = mm->mmap; vma; vma = vma->next) {
        if (!mm_shutdown_shared_file_vma(vma))
            continue;
        for (vaddr_t va = vma->start; va < vma->end; ) {
            int level = 0;
            vaddr_t base = va;
            size_t size = PAGE_SIZE;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            vaddr_t next = size && base + size > va ? base + size : va + PAGE_SIZE;
            if (next <= va)
                next = va + PAGE_SIZE;
            if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte)) {
                va = next;
                continue;
            }

            pte_t *table = mm_pt_leaf_table(mm->pgdir, va);
            if (!table) {
                result = -EFAULT;
                va = next;
                continue;
            }
            mm_pt_node_lock(table);
            pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            pte_t current = pte ? __atomic_load_n(pte, __ATOMIC_ACQUIRE) : 0;
            if (pte && (current & PTE_V) && arch_pte_is_leaf(current)) {
                pte_t replacement = 0;
                int changed = 0;
#if PTE_D == 0 || PTE_D == PTE_W
                if (current & PTE_W) {
                    result = mm_shutdown_mark_leaf_dirty(vma, base, size);
                    if (result < 0) {
                        mm_pt_node_unlock(table);
                        va = next;
                        continue;
                    }
                }
#endif
                changed = mm_shutdown_clear_leaf_write(pte, &replacement);
                if (changed) {
                    (void)mm_pt_refresh_leaf_prot(table,
                                                  arch_pt_vpn(va, level),
                                                  replacement);
                    mm_tlb_note_change(mm, base, size);
                }
            }
            mm_pt_node_unlock(table);
            va = next;
        }
    }
    spin_unlock(&mm->lock);
    mm_tlb_invalidate_finish(mm);

    /* Harvest only after remote CPUs have crossed the shootdown barrier.  A
     * huge dirty leaf covers every cache page in its VMA intersection; doing
     * one lookup at the leaf base loses dirty subpages of that mapping. */
    spin_lock(&mm->lock);
    for (mm_seg_t *vma = mm->mmap; vma; vma = vma->next) {
        if (!mm_shutdown_shared_file_vma(vma))
            continue;
        for (vaddr_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
            int level = 0;
            vaddr_t base = 0;
            size_t size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (!pte || !(*pte & PTE_V) || !(*pte & PTE_D))
                continue;
            uint64_t offset = vma->backing_offset + (va - vma->start);
            page_cache_page_t *page = page_cache_get(vma->file_vnode,
                                                       offset / PAGE_SIZE, 0);
            if (page) {
                page_cache_mark_dirty(page);
                page_cache_put(page);
            }
        }
    }
    spin_unlock(&mm->lock);
    return result;
}
#else
static int mm_shutdown_revoke_mm(mm_struct_t *mm)
{
    (void)mm;
    return 0;
}
#endif

static void mm_shutdown_drop_snapshot(mm_struct_t **items, size_t count)
{
    for (size_t i = 0; i < count; i++)
        mm_destroy(items[i]);
    kfree(items);
}

static int mm_shutdown_snapshot(mm_struct_t ***items_out, size_t *count_out)
{
    *items_out = NULL;
    *count_out = 0;
    /* A racing fork/exit can invalidate a capacity estimate.  Retry a small
     * bounded number of times, then let shutdown abort/reopen instead of
     * monopolizing the scheduler or allocating without a bound. */
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        size_t capacity = 0;
        uint64_t flags = spin_lock_irqsave(&tasklist_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t))
            capacity++;
        spin_unlock_irqrestore(&tasklist_lock, flags);

        if (capacity == 0)
            return 0;
        if (capacity > SIZE_MAX / sizeof(mm_struct_t *))
            return -ENOMEM;
        mm_struct_t **items = kcalloc(capacity, sizeof(*items));
        if (!items)
            return -ENOMEM;

        size_t count = 0;
        int retry = 0;
        mm_struct_t *overflow_pin = NULL;
        flags = spin_lock_irqsave(&tasklist_lock);
        for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
            if (proc_task_state_get(t) == PROC_UNUSED)
                continue;
            mm_struct_t *mm = proc_task_get_mm(t);
            if (!mm)
                continue;
            int duplicate = 0;
            for (size_t i = 0; i < count; i++) {
                if (items[i] == mm) {
                    duplicate = 1;
                    break;
                }
            }
            if (duplicate) {
                /* `items[i]` is already a distinct live pin for this mm, so
                 * this duplicate pin cannot be its final reference. */
                mm_destroy(mm);
                continue;
            }
            if (count == capacity) {
                overflow_pin = mm;
                retry = 1;
                break;
            }
            items[count++] = mm;
        }
        spin_unlock_irqrestore(&tasklist_lock, flags);

        /* Never drop a possibly-final MM reference under tasklist_lock. */
        if (overflow_pin)
            mm_destroy(overflow_pin);

        if (retry) {
            mm_shutdown_drop_snapshot(items, count);
            continue;
        }
        *items_out = items;
        *count_out = count;
        return 0;
    }
    return -EAGAIN;
}

int mm_shared_write_shutdown_revoke(uint64_t token)
{
    int drained = a20_mm_shared_write_drained_rust(token);
    if (drained <= 0)
        return drained == 0 ? -EAGAIN : drained;

    mm_struct_t **items = NULL;
    size_t count = 0;
    int r = mm_shutdown_snapshot(&items, &count);
    if (r < 0)
        return r;

    int first_error = 0;
    for (size_t i = 0; i < count; i++) {
        r = mm_shutdown_revoke_mm(items[i]);
        if (r < 0 && first_error == 0)
            first_error = r;
    }
    mm_shutdown_drop_snapshot(items, count);
    return first_error;
}
