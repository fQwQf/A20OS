#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/lock.h"
#include "core/perf.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/types.h"

void cg_mem_init(cg_mem_state_t *mem)
{
    spin_init(&mem->lock);
    mem->limit = SIZE_MAX;
    mem->swap_limit = SIZE_MAX;
    mem->rss = 0;
    mem->shared_anon_rss = 0;
    mem->cache = 0;
    mem->swap_usage = 0;
    mem->total_vm = 0;
    mem->failcnt = 0;
    mem->oom_kill_disable = 0;
    mem->oom_kill_count = 0;
    mem->hierarchy = 1;
    mem->swappiness = 60;
    mem->min_val = 0;
    mem->low_val = 0;
}

static int cg_mem_charge_kind(struct cg_node *cg, size_t nr_pages, int shared_anon)
{
    if (!cg) return 0;
    cg_node_t *node = (cg_node_t *)cg;

    uint64_t flags;
    if (!spin_trylock_irqsave(&node->lock, &flags)) {
        a20_perf_count(A20_PERF_MM_CG_LOCK_CONTENDED);
        flags = spin_lock_irqsave(&node->lock);
    }
    a20_perf_count(A20_PERF_MM_CG_LOCK_ACQUIRES);
    cg_mem_state_t *m = &node->res.mem;

    if (nr_pages > SIZE_MAX - m->rss ||
        (m->limit != SIZE_MAX && m->rss + nr_pages > m->limit)) {
        m->failcnt++;
        spin_unlock_irqrestore(&node->lock, flags);
        return -ENOMEM;
    }

    m->rss += nr_pages;
    if (shared_anon)
        m->shared_anon_rss += nr_pages;
    spin_unlock_irqrestore(&node->lock, flags);
    return 0;
}

int cg_mem_charge(struct cg_node *cg, size_t nr_pages)
{
    return cg_mem_charge_kind(cg, nr_pages, 0);
}

int cg_mem_charge_shared_anon(struct cg_node *cg, size_t nr_pages)
{
    return cg_mem_charge_kind(cg, nr_pages, 1);
}

void cg_mem_uncharge(struct cg_node *cg, size_t nr_pages)
{
    if (!cg) return;
    cg_node_t *node = (cg_node_t *)cg;

    uint64_t flags = spin_lock_irqsave(&node->lock);
    cg_mem_state_t *m = &node->res.mem;
    /* Exit's legacy RSS-based accounting also sees borrowed shared leaves.
     * It must not release a live VMO's physical-memory charge; that charge
     * belongs to the backing object until its last reference is destroyed. */
    size_t owned_by_mm = m->rss - m->shared_anon_rss;
    m->rss -= nr_pages < owned_by_mm ? nr_pages : owned_by_mm;
    spin_unlock_irqrestore(&node->lock, flags);
}

void cg_mem_uncharge_shared_anon(struct cg_node *cg, size_t nr_pages)
{
    if (!cg) return;
    cg_node_t *node = (cg_node_t *)cg;
    uint64_t flags = spin_lock_irqsave(&node->lock);
    cg_mem_state_t *m = &node->res.mem;
    size_t released = nr_pages < m->shared_anon_rss ? nr_pages : m->shared_anon_rss;
    m->shared_anon_rss -= released;
    m->rss -= released;
    spin_unlock_irqrestore(&node->lock, flags);
}

int cg_mem_swap_charge_node(struct cg_node *cg, size_t pages)
{
    if (!cg) return 0;
    cg_node_t *node = (cg_node_t *)cg;
    uint64_t flags = spin_lock_irqsave(&node->lock);
    cg_mem_state_t *m = &node->res.mem;

    if (pages > SIZE_MAX - m->swap_usage ||
        m->swap_usage > m->swap_limit || pages > m->swap_limit - m->swap_usage) {
        m->failcnt++;
        spin_unlock_irqrestore(&node->lock, flags);
        return -ENOMEM;
    }

    m->swap_usage += pages;
    spin_unlock_irqrestore(&node->lock, flags);
    return 0;
}

void cg_mem_swap_uncharge_node(struct cg_node *cg, size_t pages)
{
    if (!cg) return;
    cg_node_t *node = (cg_node_t *)cg;
    uint64_t flags = spin_lock_irqsave(&node->lock);
    cg_mem_state_t *m = &node->res.mem;
    if (m->swap_usage >= pages)
        m->swap_usage -= pages;
    else
        m->swap_usage = 0;
    spin_unlock_irqrestore(&node->lock, flags);
}

void cg_group_pin(struct cg_node *cg)
{
    if (cg) __atomic_fetch_add(&((cg_node_t *)cg)->namespace_refs, 1, __ATOMIC_RELAXED);
}

void cg_group_unpin(struct cg_node *cg)
{
    if (cg) __atomic_fetch_sub(&((cg_node_t *)cg)->namespace_refs, 1, __ATOMIC_RELEASE);
}

struct cg_node *cg_task_group_pin(task_t *t)
{
    if (!t) return NULL;
    uint64_t flags = spin_lock_irqsave(&t->park_lock);
    struct cg_node *cg = t->cgroup;
    cg_group_pin(cg);
    spin_unlock_irqrestore(&t->park_lock, flags);
    return cg;
}

int cg_mem_swap_charge(task_t *t, size_t pages)
{
    struct cg_node *cg = cg_task_group_pin(t);
    int r = cg_mem_swap_charge_node(cg, pages);
    cg_group_unpin(cg);
    return r;
}

void cg_mem_swap_uncharge(task_t *t, size_t pages)
{
    struct cg_node *cg = cg_task_group_pin(t);
    cg_mem_swap_uncharge_node(cg, pages);
    cg_group_unpin(cg);
}

void cg_mem_oom_kill(struct cg_node *cg)
{
    if (!cg) return;
    cg_node_t *node = (cg_node_t *)cg;

    uint64_t flags = spin_lock_irqsave(&node->lock);
    if (node->res.mem.oom_kill_disable) {
        spin_unlock_irqrestore(&node->lock, flags);
        return;
    }
    node->res.mem.oom_kill_count++;
    spin_unlock_irqrestore(&node->lock, flags);

    int victim_pid = -1;
    int worst_score = -1;

    /* E2: tasklist_lock walks the global list, each candidate's ->state,
     * ->cgroup and ->mm are park_lock-owned.  The mm reference the exiting
     * task holds is still live while that park_lock is held, so ->mm cannot be
     * freed under the scan. */
    uint64_t pflags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        uint64_t tf = spin_lock_irqsave(&t->park_lock);
        int st = t->state;
        if (st == PROC_UNUSED || st == PROC_ZOMBIE || t->cgroup != cg) {
            spin_unlock_irqrestore(&t->park_lock, tf);
            continue;
        }
        /* ->mm stays pinned by the exiting task for as long as this task's
         * park_lock is held, so the RSS read below cannot race the drop. */
        size_t task_rss = mm_rss_get(t->mm);
        int score = (int)task_rss + t->policy.oom_score_adj;
        int tpid = t->pid;
        spin_unlock_irqrestore(&t->park_lock, tf);
        if (score > worst_score) {
            worst_score = score;
            victim_pid = tpid;
        }
    }
    spin_unlock_irqrestore(&tasklist_lock, pflags);

    if (victim_pid > 0) {
        kinfo("cg_mem_oom: killing pid %d (score %d)\n", victim_pid, worst_score);
        signal_send(victim_pid, 9);
    }
}
