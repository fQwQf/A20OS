#include "mm/oom.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/swap.h"
#include "mm/pt.h"
#include "cg/cgroup.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/timer.h"

/* Reclaim levers outside mm/: clean page-cache drop (fs/page_cache.c) and
 * spare-slab release (mm/slab.c).  Declared locally to keep mm -> fs
 * dependency surface at exactly these two symbols. */
extern size_t page_cache_drop_clean(void);
extern size_t slab_reclaim_spare(void);

#define OOM_COOLDOWN_TICKS MS_TO_TICKS(2000)
#define OOM_MIN_FREE_PAGES 256
#define MAX_SWAP_RECLAIM 8
#define KSWAPD_INTERVAL_TICKS MS_TO_TICKS(1000)
#define KSWAPD_SWAP_BATCH 64


#ifdef CONFIG_SWAP
static int vma_is_swappable(const mm_seg_t *vma)
{
    return vma && (vma->vm_flags & VM_ANON) &&
           !(vma->vm_flags & (VM_SHARED | VM_LOCKED | VM_PFNMAP |
                              VM_VMO | VM_FILE));
}

static int swap_out_task_pages(task_t *victim, int target_pages,
                                vaddr_t first, vaddr_t last, int forced)
{
    mm_struct_t *mm = proc_task_get_mm(victim);
    if (!mm || !mm->pgdir) {
        if (mm)
            mm_destroy(mm);
        return 0;
    }

    struct cg_node *owner = cg_task_group_pin(victim);
    int reclaimed = 0;
    vaddr_t scan = first;
    int limit = forced || target_pages < MAX_SWAP_RECLAIM ? target_pages : MAX_SWAP_RECLAIM;
    while (reclaimed < limit && scan < last) {
        swap_entry_t entry = 0;
        pfn_t pfn = PFN_NONE;
        mm_seg_t *source = NULL;
        vaddr_t va = 0;
        int exhausted = 0;
        unsigned budget = 256;
        /* SWAP_OUT_IO_LOCKING: protect the private frame and finish its TLB
         * shootdown BEFORE reading it for I/O. A racing writer must COW to a
         * different frame; merely checking the PFN after I/O misses writes. */
        mm_tlb_invalidate_begin(mm);
        uint64_t flags = spin_lock_irqsave(&mm->lock);
        mm_seg_t *vma = mm_seg_find(mm, scan);
        for (; vma && vma->start < last && budget && !entry; vma = entry ? vma : vma->next) {
            if (!vma_is_swappable(vma)) { scan = vma->end; continue; }
            va = scan > vma->start ? scan : vma->start;
            for (; va < vma->end && va < last && budget && !entry; va += PAGE_SIZE) {
                scan = va + PAGE_SIZE;
                budget--;
                int level = 0;
                vaddr_t base = 0;
                size_t size = 0;
                pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
                if (!pte || level || base != va || size != PAGE_SIZE) continue;
#if defined(ARCH_HAS_PGTABLE_OPS)
                pte_t *table = mm_pt_leaf_table(mm->pgdir, va);
                if (table) mm_pt_node_lock(table);
#endif
                pte_t snapshot = *pte;
                pfn_t candidate = phys_to_pfn(arch_pte_addr(snapshot));
                int eligible = pte_present(snapshot) && arch_pte_is_leaf(snapshot) &&
                               (snapshot & PTE_U) && pfn_valid(candidate);
                if (eligible) {
                    uint64_t pf = spin_lock_irqsave(&pfa.lock);
                    eligible = pfa.meta[candidate].refcount == 1;
                    spin_unlock_irqrestore(&pfa.lock, pf);
                }
                if (eligible) {
                    entry = get_swap_page_owned(owner);
                    if (entry) {
                        pfn = candidate;
                        frame_get(pfn); /* immutable I/O snapshot pin */
                        source = mm_seg_get(vma);
                        pte_t prot = arch_pte_flags(snapshot);
                        if (prot & (PTE_W | PTE_COW)) {
                            prot &= ~(pte_t)(PTE_W | PTE_D);
                            prot |= PTE_COW;
                        }
                        *pte = arch_pte_leaf(arch_pte_addr(snapshot), prot);
#if defined(ARCH_HAS_PGTABLE_OPS)
                        if (table) (void)mm_pt_sync_status(table, 0,
                                            arch_pt_vpn(va, 0), MM_ST_ANON_MAPPED);
#endif
                        if (*pte != snapshot) mm_tlb_note_change(mm, va, PAGE_SIZE);
                    } else exhausted = 1;
                }
#if defined(ARCH_HAS_PGTABLE_OPS)
                if (table) mm_pt_node_unlock(table);
#endif
                if (exhausted) break;
            }
            if (entry) { va -= PAGE_SIZE; break; }
            if (exhausted || !budget) break;
        }
        if (!vma || vma->start >= last || scan >= last) exhausted = 1;
        spin_unlock_irqrestore(&mm->lock, flags);
        mm_tlb_invalidate_finish(mm);
        if (!entry) {
            if (exhausted) break;
            continue;
        }

        int written = swap_write_page(entry, pfn_to_virt(pfn));
        int committed = 0;
        if (written >= 0) {
            mm_tlb_invalidate_begin(mm);
            flags = spin_lock_irqsave(&mm->lock);
            mm_seg_t *current = mm_seg_find(mm, va);
            int level = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, NULL, NULL);
#if defined(ARCH_HAS_PGTABLE_OPS)
            pte_t *table = level == 0 ? mm_pt_leaf_table(mm->pgdir, va) : NULL;
            if (table) mm_pt_node_lock(table);
#endif
            if (current == source && va >= source->start && va < source->end &&
                vma_is_swappable(source) && pte && !level &&
                pte_present(*pte) && arch_pte_is_leaf(*pte) && !(*pte & PTE_W) &&
                phys_to_pfn(arch_pte_addr(*pte)) == pfn && mm_tlb_hold_frame(mm, pfn) == 0) {
                *pte = swp_entry_to_pte(entry);
#if defined(ARCH_HAS_PGTABLE_OPS)
                if (table) mm_pt_note_present(table, 0, arch_pt_vpn(va, 0),
                    MM_ST_CLS_BYTE(MM_ST_SWAPPED) | mm_pt_prot_bits(source->pte_flags));
#endif
                mm_tlb_note_change(mm, va, PAGE_SIZE);
                frame_put(pfn); /* old mapping; the TLB hold outlives shootdown */
                mm_rss_sub_clamped(mm, 1);
                reclaimed++;
                committed = 1;
            }
#if defined(ARCH_HAS_PGTABLE_OPS)
            if (table) mm_pt_node_unlock(table);
#endif
            spin_unlock_irqrestore(&mm->lock, flags);
            mm_tlb_invalidate_finish(mm);
        }
        if (committed) cg_mem_uncharge(owner, 1);
        else swap_free(entry);
        frame_put(pfn);
        mm_seg_put(source);
        if (!forced && pfa_free_count() >= OOM_MIN_FREE_PAGES) break;
    }
    cg_group_unpin(owner);
    mm_destroy(mm);
    return reclaimed;
}
#endif /* CONFIG_SWAP */

/*
 * Victim selection, the reclaim escalation and the background reclaimer policy
 * live in kernel/mm/oom_policy.rs.  What is left here is the swap-out page
 * walk (above), the task-list walk, the levers and the kill.
 */
extern int oom_try_reclaim(void);
extern int oom_swap_reclaim_pages(int target_pages);
extern int oom_swap_reclaim_range(struct task_t *t, vaddr_t start, size_t len);
extern void oom_kswapd_thread(void);
extern void oom_get_stats(oom_stats_t *out);

struct a20_oom_task_snap {
    int32_t pid;
    int32_t state;
    int32_t score;
    int32_t has_mm;
    int32_t is_idle;
};
struct a20_oom_params {
    uintptr_t min_free_pages;
    int32_t   max_swap_reclaim;
    uint64_t  cooldown_ticks;
    uint64_t  kswapd_interval_ticks;
    int32_t   proc_unused;
    int32_t   proc_zombie;
    int32_t   kswapd_swap_batch;
    int32_t   reserved;
};
#if __SIZEOF_POINTER__ == 8
_Static_assert(sizeof(struct a20_oom_task_snap) == 20, "Rust/C oom snap ABI");
_Static_assert(sizeof(struct a20_oom_params) == 48, "Rust/C oom params ABI");
#endif

void a20_oom_params(struct a20_oom_params *out)
{
    if (!out)
        return;
    out->min_free_pages = OOM_MIN_FREE_PAGES;
    out->max_swap_reclaim = MAX_SWAP_RECLAIM;
    out->cooldown_ticks = OOM_COOLDOWN_TICKS;
    out->kswapd_interval_ticks = KSWAPD_INTERVAL_TICKS;
    out->proc_unused = PROC_UNUSED;
    out->proc_zombie = PROC_ZOMBIE;
    out->kswapd_swap_batch = KSWAPD_SWAP_BATCH;
    out->reserved = 0;
}

void *a20_oom_task_first(void) { return proc_first_task_locked(); }
void *a20_oom_task_next(void *t) { return proc_next_task_locked(t); }

/* One task sample, taken under that task's park_lock so the park state machine
 * cannot move underneath the scorer. */
int a20_oom_task_snapshot(void *t_ptr, struct a20_oom_task_snap *out)
{
    task_t *t = t_ptr;
    if (!t || !out)
        return -1;
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    out->pid = t->pid;
    out->state = t->state;
    out->score = t->policy.oom_score_adj;
    out->has_mm = t->mm != NULL;
    out->is_idle = (t == proc_idle_task());
    spin_unlock_irqrestore(&t->park_lock, flags);
    return 0;
}

uintptr_t a20_oom_free_pages(void) { return pfa_free_count(); }
uint64_t a20_oom_now(void) { return timer_get_ticks(); }
uintptr_t a20_oom_user_va_limit(void) { return USER_VA_LIMIT; }

/* Swap out one victim's anonymous pages, chosen by the Rust policy. */
int a20_oom_swap_victim_by_pid(int pid, int target_pages)
{
#ifdef CONFIG_SWAP
    task_t *victim = pid > 0 ? proc_find_get(pid) : NULL;
    if (!victim)
        return 0;
    int r = swap_out_task_pages(victim, target_pages, 0, USER_VA_LIMIT, 0);
    proc_put(victim);
    return r;
#else
    (void)pid;
    (void)target_pages;
    return 0;
#endif
}

/* PAGEOUT over an explicit range: no victim selection, no threshold. */
int a20_oom_swap_task(void *t, int limit, uintptr_t start, uintptr_t end)
{
#ifdef CONFIG_SWAP
    return swap_out_task_pages(t, limit, start, end, 1);
#else
    (void)t;
    (void)limit;
    (void)start;
    (void)end;
    return 0;
#endif
}

void *a20_oom_find_get(int pid) { return proc_find_get(pid); }
void a20_oom_put(void *t) { proc_put(t); }

void a20_oom_kill(void *t_ptr)
{
    task_t *victim = t_ptr;
    if (!victim)
        return;
    kerr("[OOM] Killing pid=%d name=%s oom_score_adj=%d free_frames=%lu\n",
         victim->pid, victim->name, victim->policy.oom_score_adj,
         (unsigned long)pfa_free_count());
    proc_force_exit(victim, -9);
}

void a20_oom_sleep_until(uint64_t tick) { proc_sleep_until(tick); }
void a20_oom_yield(void) { proc_yield(); }
void a20_oom_kswapd_started(void)
{
    klog(KLOG_INFO, "[KSWAPD] background reclaimer started\n");
}
