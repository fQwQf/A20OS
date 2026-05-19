#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/lock.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/types.h"

void cg_mem_init(cg_mem_state_t *mem)
{
    spin_init(&mem->lock);
    mem->limit = SIZE_MAX;
    mem->swap_limit = SIZE_MAX;
    mem->rss = 0;
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

int cg_mem_charge(struct cg_node *cg, size_t nr_pages)
{
    if (!cg) return 0;
    cg_node_t *node = (cg_node_t *)cg;

    uint64_t flags = spin_lock_irqsave(&node->lock);
    cg_mem_state_t *m = &node->res.mem;

    if (m->limit != SIZE_MAX && m->rss + nr_pages > m->limit) {
        m->failcnt++;
        spin_unlock_irqrestore(&node->lock, flags);
        return -ENOMEM;
    }

    m->rss += nr_pages;
    spin_unlock_irqrestore(&node->lock, flags);
    return 0;
}

void cg_mem_uncharge(struct cg_node *cg, size_t nr_pages)
{
    if (!cg) return;
    cg_node_t *node = (cg_node_t *)cg;

    uint64_t flags = spin_lock_irqsave(&node->lock);
    cg_mem_state_t *m = &node->res.mem;
    if (m->rss >= nr_pages)
        m->rss -= nr_pages;
    else
        m->rss = 0;
    spin_unlock_irqrestore(&node->lock, flags);
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

    uint64_t pflags = spin_lock_irqsave(&proc_lock);
    for (task_t *t = proc_first_task_locked(); t; t = proc_next_task_locked(t)) {
        if (t->state == PROC_UNUSED || t->state == PROC_ZOMBIE) continue;
        if (t->cgroup != cg) continue;
        size_t task_rss = t->mm ? t->mm->rss : 0;
        int score = (int)task_rss + t->policy.oom_score_adj;
        if (score > worst_score) {
            worst_score = score;
            victim_pid = t->pid;
        }
    }
    spin_unlock_irqrestore(&proc_lock, pflags);

    if (victim_pid > 0) {
        kinfo("cg_mem_oom: killing pid %d (score %d)\n", victim_pid, worst_score);
        signal_send(victim_pid, 9);
    }
}
