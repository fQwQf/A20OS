#include "mm/oom.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/swap.h"
#include "cg/cgroup.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/klog.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/timer.h"

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

    uint64_t flags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t == proc_idle_task() || t->state == PROC_UNUSED || t->state == PROC_ZOMBIE)
            continue;
        if (t->pid <= 2)
            continue;
        int score = t->policy.oom_score_adj;
        if (!t->mm)
            score -= 100;
        if (score > best_score ||
            (score == best_score && t->pid > victim_pid)) {
            best_score = score;
            victim_pid = t->pid;
        }
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    return victim_pid;
}

#ifdef CONFIG_SWAP
static int vma_is_swappable(const vm_area_t *vma)
{
    return vma && (vma->vm_flags & VM_ANON) &&
           !(vma->vm_flags & (VM_SHARED | VM_LOCKED | VM_PFNMAP |
                              VM_VMO | VM_FILE));
}

static int swap_out_victim_pages(int target_pages)
{
    int victim_pid = oom_pick_victim_pid();
    if (victim_pid <= 0)
        return 0;

    task_t *victim = proc_find_get(victim_pid);
    if (!victim)
        return 0;
    if (!victim->mm || !victim->mm->pgdir) {
        proc_put(victim);
        return 0;
    }

    mm_struct_t *mm = victim->mm;
    int reclaimed = 0;
    uint64_t flags = spin_lock(&mm->lock);

    /* TODO: install a busy swap PTE and drop mm->lock before block I/O. */
    for (vm_area_t *vma = mm->mmap;
         vma && reclaimed < target_pages && reclaimed < MAX_SWAP_RECLAIM;
         vma = vma->next) {
        if (!vma_is_swappable(vma))
            continue;

        for (vaddr_t va = vma->start;
             va < vma->end && reclaimed < target_pages && reclaimed < MAX_SWAP_RECLAIM;
             va += PAGE_SIZE) {
            int level = 0;
            vaddr_t base = 0;
            size_t size = 0;
            pte_t *pte = pt_lookup_leaf(mm->pgdir, va, &level, &base, &size);
            if (!pte || !pte_present(*pte) || !arch_pte_is_leaf(*pte) ||
                !(*pte & PTE_U) || level != 0 || base != va || size != PAGE_SIZE)
                continue;

            pfn_t pfn = phys_to_pfn(arch_pte_addr(*pte));
            if (!pfn_valid(pfn))
                continue;

            uint64_t pfa_flags = spin_lock_irqsave(&pfa.lock);
            uint16_t refs = pfa.meta[pfn].refcount;
            spin_unlock_irqrestore(&pfa.lock, pfa_flags);
            if (refs > 1)
                continue;

            swap_entry_t entry = get_swap_page();
            if (!entry)
                continue;
            if (swap_write_page(entry, pfn_to_virt(pfn)) < 0) {
                swap_free(entry);
                continue;
            }
            if (cg_mem_swap_charge(victim, 1) < 0) {
                swap_free(entry);
                continue;
            }

            *pte = swp_entry_to_pte(entry);
            arch_tlb_flush_page_local(va);
            frame_put(pfn);
            cg_mem_uncharge(victim->cgroup, 1);
            mm->rss = mm->rss ? mm->rss - 1 : 0;
            reclaimed++;

            if (pfa_free_count() >= OOM_MIN_FREE_PAGES)
                break;
        }
    }

    spin_unlock(&mm->lock);
    proc_put(victim);
    return reclaimed;
}
#else
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
}
