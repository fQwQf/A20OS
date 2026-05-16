#include "cg/cgroup.h"
#include "cg/cgroup_impl.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/lock.h"
#include "core/timer.h"
#include "core/cpu.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/types.h"

void cg_cpu_init(cg_cpu_state_t *cpu)
{
    spin_init(&cpu->lock);
    cpu->quota = CG_CPU_QUOTA_MAX;
    cpu->period = 100000000ULL;
    cpu->runtime = 0;
    cpu->period_start = 0;
    cpu->throttled = 0;
    cpu->nr_throttled = 0;
    cpu->throttled_time = 0;
    cpu->total_runtime = 0;
}

int cg_cpu_account(struct cg_node *cg, uint64_t elapsed_ns, uint64_t now)
{
    if (!cg) return 0;
    cg_node_t *node = (cg_node_t *)cg;

    uint64_t flags = spin_lock_irqsave(&node->lock);
    cg_cpu_state_t *c = &node->res.cpu;

    if (c->quota == CG_CPU_QUOTA_MAX) {
        spin_unlock_irqrestore(&node->lock, flags);
        return 0;
    }

    if (c->period_start == 0)
        c->period_start = now;

    if (now - c->period_start >= c->period) {
        if (c->throttled)
            c->throttled_time += (now - c->period_start) - c->runtime;
        c->runtime = 0;
        c->period_start = now;
        c->throttled = 0;
    }

    c->runtime += elapsed_ns;
    c->total_runtime += elapsed_ns;

    if (c->runtime >= c->quota) {
        c->throttled = 1;
        c->nr_throttled++;
        spin_unlock_irqrestore(&node->lock, flags);
        return 1;
    }

    spin_unlock_irqrestore(&node->lock, flags);
    return 0;
}

void cg_cpu_throttle_task(struct cg_node *cg)
{
    if (!cg) return;
    cg_node_t *node = (cg_node_t *)cg;

    for (int i = 0; i < node->pid_count; i++) {
        task_t *t = proc_find(node->pids[i]);
        if (t && t->state == PROC_READY) {
            t->cg_throttled = 1;
        }
    }
}

void cg_cpu_check_unthrottle(struct cg_node *cg, uint64_t now)
{
    if (!cg) return;
    cg_node_t *node = (cg_node_t *)cg;

    uint64_t flags = spin_lock_irqsave(&node->lock);
    cg_cpu_state_t *c = &node->res.cpu;

    if (!c->throttled) {
        spin_unlock_irqrestore(&node->lock, flags);
        return;
    }

    if (now - c->period_start >= c->period) {
        c->throttled_time += (now - c->period_start) - c->runtime;
        c->runtime = 0;
        c->period_start = now;
        c->throttled = 0;
        spin_unlock_irqrestore(&node->lock, flags);

        for (int i = 0; i < node->pid_count; i++) {
            task_t *t = proc_find(node->pids[i]);
            if (t) {
                t->cg_throttled = 0;
                if (t->state == PROC_READY)
                    proc_make_ready(t);
            }
        }
        return;
    }

    spin_unlock_irqrestore(&node->lock, flags);
}

void cg_cpuset_init(cg_cpuset_state_t *cs, unsigned nr_cpus)
{
    spin_init(&cs->lock);
    cs->cpus_allowed = (1U << nr_cpus) - 1;
    cs->mems_allowed = 1;
    cs->effective_cpus = cs->cpus_allowed;
}

unsigned cg_cpuset_select_cpu(struct cg_node *cg)
{
    if (!cg) return cpu_current_id();
    cg_node_t *node = (cg_node_t *)cg;

    uint64_t flags = spin_lock_irqsave(&node->lock);
    uint32_t mask = node->res.cpuset.effective_cpus;
    spin_unlock_irqrestore(&node->lock, flags);

    if (!mask) return cpu_current_id();

    unsigned current = cpu_current_id();
    if (mask & (1U << current)) return current;

    for (unsigned i = 0; i < CONFIG_NR_CPUS && i < 32; i++) {
        if (mask & (1U << i)) return i;
    }

    return current;
}

void cg_cpuset_update_effective(struct cg_node *cg, unsigned nr_cpus)
{
    if (!cg) return;
    cg_node_t *node = (cg_node_t *)cg;
    uint32_t all = (1U << nr_cpus) - 1;
    uint32_t parent_mask = all;

    if (node->parent) {
        uint64_t pflags = spin_lock_irqsave(&node->parent->lock);
        parent_mask = node->parent->res.cpuset.effective_cpus;
        spin_unlock_irqrestore(&node->parent->lock, pflags);
    }

    node->res.cpuset.effective_cpus = node->res.cpuset.cpus_allowed & parent_mask;
    if (!node->res.cpuset.effective_cpus)
        node->res.cpuset.effective_cpus = node->res.cpuset.cpus_allowed & all;
}
