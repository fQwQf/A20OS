#ifndef _CG_CGROUP_H
#define _CG_CGROUP_H

#include "core/types.h"
#include "core/lock.h"

/*
 * Cgroup resource controller state.
 *
 * Lock ordering (outermost -> innermost):
 *   cg_node.lock -> proc_lock -> runq_lock -> pfa.lock
 */

/* ---- Memory controller ---- */
typedef struct cg_mem_state {
    spinlock_t lock;
    size_t limit;            /* max RSS in pages (SIZE_MAX = unlimited) */
    size_t swap_limit;       /* RSS+swap limit in pages (future) */
    size_t rss;              /* current resident set size in pages */
    size_t cache;            /* page cache pages */
    size_t swap_usage;       /* swap pages (future) */
    size_t total_vm;         /* total virtual memory in pages */
    uint64_t failcnt;        /* how many times limit was hit */
    int oom_kill_disable;    /* 1 = disable OOM killer for this cgroup */
    int oom_kill_count;      /* number of tasks killed by OOM */
} cg_mem_state_t;

/* ---- CPU bandwidth controller ---- */
#define CG_CPU_QUOTA_MAX  ((uint64_t)-1)  /* unlimited */

typedef struct cg_cpu_state {
    spinlock_t lock;
    uint64_t quota;          /* ns per period (CG_CPU_QUOTA_MAX = unlimited) */
    uint64_t period;         /* period duration in ns */
    uint64_t runtime;        /* ns consumed in current period */
    uint64_t period_start;   /* ticks when period began */
    int throttled;           /* 1 if cgroup is currently throttled */
    uint32_t nr_throttled;   /* throttle event count */
    uint64_t throttled_time; /* cumulative ns spent throttled */
    uint64_t total_runtime;  /* cumulative ns consumed ever */
} cg_cpu_state_t;

/* ---- cpuset controller ---- */
typedef struct cg_cpuset_state {
    spinlock_t lock;
    uint32_t cpus_allowed;   /* bitmask of allowed CPUs */
    uint32_t mems_allowed;   /* NUMA node mask */
    uint32_t effective_cpus; /* parent intersect requested */
} cg_cpuset_state_t;

/* ---- Combined resource state per cgroup ---- */
typedef struct cg_resource {
    cg_mem_state_t    mem;
    cg_cpu_state_t    cpu;
    cg_cpuset_state_t cpuset;
} cg_resource_t;

/* Forward declaration — defined in fs/cgroupfs.c */
struct cg_node;

/* ---- Init functions (called from cgroupfs_mount) ---- */
void cg_mem_init(cg_mem_state_t *mem);
void cg_cpu_init(cg_cpu_state_t *cpu);
void cg_cpuset_init(cg_cpuset_state_t *cs, unsigned nr_cpus);

/* ---- Memory controller API ---- */
int  cg_mem_charge(struct cg_node *cg, size_t nr_pages);
void cg_mem_uncharge(struct cg_node *cg, size_t nr_pages);
void cg_mem_oom_kill(struct cg_node *cg);

/* ---- CPU controller API ---- */
int  cg_cpu_account(struct cg_node *cg, uint64_t elapsed_ns, uint64_t now);
void cg_cpu_throttle_task(struct cg_node *cg);
void cg_cpu_check_unthrottle(struct cg_node *cg, uint64_t now);

/* ---- cpuset controller API ---- */
unsigned cg_cpuset_select_cpu(struct cg_node *cg);
void     cg_cpuset_update_effective(struct cg_node *cg, unsigned nr_cpus);

/* ---- Task association API ---- */
void cg_attach_task(struct cg_node *cg, int pid);
void cg_detach_task(struct cg_node *cg, int pid);
struct cg_node *cg_root_node(void);

#endif /* _CG_CGROUP_H */
