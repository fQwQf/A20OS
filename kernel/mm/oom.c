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

static volatile int oom_in_progress;
static uint64_t oom_last_kill_tick;
static unsigned long oom_kill_count;
static int oom_last_victim_pid;
static int oom_last_victim_score;
static unsigned long oom_free_pages_at_kill;

static int oom_pick_victim_pid(void)
{
    int victim_pid = -1;
    int best_score = -1;

    /* E2: tasklist_lock walks the global list; every field read below is
     * park_lock-owned and is sampled under that task's own park_lock, one task
     * at a time.  Reading ->state without the lock would be unsynchronised
     * against the park state machine (INV-P1). */
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t == proc_idle_task())
            continue;
        uint64_t tf = spin_lock_irqsave(&t->park_lock);
        int st = t->state;
        int score = t->policy.oom_score_adj;
        int has_mm = t->mm != NULL;
        int tpid = t->pid;
        spin_unlock_irqrestore(&t->park_lock, tf);
        if (st == PROC_UNUSED || st == PROC_ZOMBIE)
            continue;
        if (tpid <= 2)
            continue;
        if (!has_mm)
            score -= 100;
        if (score > best_score ||
            (score == best_score && tpid > victim_pid)) {
            best_score = score;
            victim_pid = tpid;
        }
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    return victim_pid;
}

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
static int swap_out_victim_pages(int target_pages)
{
    int pid = oom_pick_victim_pid();
    task_t *victim = pid > 0 ? proc_find_get(pid) : NULL;
    if (!victim) return 0;
    int r = swap_out_task_pages(victim, target_pages, 0, USER_VA_LIMIT, 0);
    proc_put(victim);
    return r;
}

int oom_swap_reclaim_range(task_t *t, vaddr_t start, size_t len)
{
    if (!t || !len || (start & (PAGE_SIZE - 1)) ||
        (len & (PAGE_SIZE - 1)) || start >= USER_VA_LIMIT || len > USER_VA_LIMIT - start)
        return -EINVAL;
    size_t pages = len / PAGE_SIZE;
    int limit = pages > 2147483647U ? 2147483647 : (int)pages;
    return swap_out_task_pages(t, limit, start, start + len, 1);
}
#else
int oom_swap_reclaim_range(task_t *t, vaddr_t start, size_t len)
{
    (void)t; (void)start; (void)len;
    return 0; /* no swap backing: PAGEOUT remains a best-effort hint */
}
static int swap_out_victim_pages(int target_pages)
{
    (void)target_pages;
    return 0;
}
#endif

int oom_try_reclaim(void)
{
    if (__atomic_load_n(&oom_in_progress, __ATOMIC_RELAXED))
        return 0;

    size_t free = pfa_free_count();
    if (free >= OOM_MIN_FREE_PAGES)
        return 0;

    uint64_t now = timer_get_ticks();
    if (now - oom_last_kill_tick < OOM_COOLDOWN_TICKS)
        return 0;

    __atomic_store_n(&oom_in_progress, 1, __ATOMIC_RELAXED);

    if (swap_out_victim_pages(MAX_SWAP_RECLAIM) > 0 &&
        pfa_free_count() >= OOM_MIN_FREE_PAGES) {
        __atomic_store_n(&oom_in_progress, 0, __ATOMIC_RELAXED);
        return 1;
    }

    int victim_pid = oom_pick_victim_pid();
    if (victim_pid <= 0) {
        __atomic_store_n(&oom_in_progress, 0, __ATOMIC_RELAXED);
        return 0;
    }

    task_t *victim = proc_find_get(victim_pid);
    if (!victim) {
        __atomic_store_n(&oom_in_progress, 0, __ATOMIC_RELAXED);
        return 0;
    }

    oom_last_kill_tick = now;
    oom_kill_count++;
    oom_last_victim_pid = victim->pid;
    oom_last_victim_score = victim->policy.oom_score_adj;
    oom_free_pages_at_kill = pfa_free_count();
    kerr("[OOM] Killing pid=%d name=%s oom_score_adj=%d free_frames=%lu\n",
         victim->pid, victim->name, victim->policy.oom_score_adj,
         (unsigned long)pfa_free_count());

    proc_force_exit(victim, -9);
    proc_put(victim);

    __atomic_store_n(&oom_in_progress, 0, __ATOMIC_RELAXED);
    return 1;
}

int oom_swap_reclaim_pages(int target_pages)
{
#ifdef CONFIG_SWAP
    if (__atomic_load_n(&oom_in_progress, __ATOMIC_RELAXED))
        return 0;
    return swap_out_victim_pages(target_pages);
#else
    (void)target_pages;
    return 0;
#endif
}

/* Background reclaimer (kswapd role).  All memory reclamation used to run
 * synchronously in the allocation path: an allocation that hit the watermark
 * paid slab shrink + page-cache drop + swap I/O before it could proceed, and
 * nothing reclaimed while the system was otherwise idle.  This thread scans
 * once a second and pulls the three existing levers in escalating order —
 * spare slab pages, clean page cache, anonymous swap-out — so allocations
 * mostly find water above the mark instead of having to dig for it. */
#define KSWAPD_INTERVAL_TICKS MS_TO_TICKS(1000)
static unsigned long kswapd_passes;
static unsigned long kswapd_pages_freed;
static unsigned long kswapd_last_pass_tick;

void oom_kswapd_thread(void)
{
    klog(KLOG_INFO, "[KSWAPD] background reclaimer started\n");
    for (;;) {
        uint64_t now = timer_get_ticks();
        if (pfa_free_count() >= OOM_MIN_FREE_PAGES) {
            proc_sleep_until(now + KSWAPD_INTERVAL_TICKS);
            continue;
        }
        kswapd_passes++;
        kswapd_last_pass_tick = now;
        size_t freed = slab_reclaim_spare() / PAGE_SIZE;
        freed += page_cache_drop_clean();
        if (pfa_free_count() < OOM_MIN_FREE_PAGES)
            freed += (size_t)oom_swap_reclaim_pages(64);
        kswapd_pages_freed += freed;
        /* Yield to let consumers catch up, but if nothing was freed, stop
         * spinning: back off a full interval before trying again. */
        if (freed == 0)
            proc_sleep_until(now + KSWAPD_INTERVAL_TICKS);
        else
            proc_yield();
    }
}

void oom_get_stats(oom_stats_t *out)
{
    if (!out)
        return;
    out->kills = oom_kill_count;
    out->last_kill_tick = oom_last_kill_tick;
    out->last_victim_pid = oom_last_victim_pid;
    out->last_victim_score = oom_last_victim_score;
    out->free_pages_at_kill = oom_free_pages_at_kill;
    out->free_pages_now = pfa_free_count();
    out->in_progress = __atomic_load_n(&oom_in_progress, __ATOMIC_RELAXED);
    out->kswapd_passes = kswapd_passes;
    out->kswapd_pages_freed = kswapd_pages_freed;
    out->kswapd_last_pass_tick = kswapd_last_pass_tick;
}
