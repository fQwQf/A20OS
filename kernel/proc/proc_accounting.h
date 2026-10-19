#ifndef A20_PROC_PROC_ACCOUNTING_H
#define A20_PROC_PROC_ACCOUNTING_H

#include "core/types.h"

/*
 * C/Rust ABI for the process accounting reports in
 * kernel/proc/proc_accounting.rs: the huge-page classification behind
 * /proc/meminfo and the /proc/sys/kernel/pidmap rendering.
 *
 * C keeps the tasklist lock primitive, the task-list walk accessors, the
 * page-table leaf query and the pid-id counters.
 */
typedef struct a20_vm_stats {
    size_t anon_huge_pages;
    size_t shmem_huge_pages;
    size_t file_huge_pages;
} a20_vm_stats_t;

typedef struct a20_leaf_info {
    int32_t  level;
    uint64_t base;
    uint64_t size;
} a20_leaf_info_t;

_Static_assert(sizeof(a20_vm_stats_t) == 3 * sizeof(size_t),
               "a20_vm_stats_t layout must match the Rust VmStats");
_Static_assert(sizeof(a20_leaf_info_t) == 24,
               "a20_leaf_info_t layout must match the Rust LeafInfo");

struct task_t;
struct mm_struct;
struct mm_seg;
struct pt_root;

/* Adapter bridges. */
int      a20_pa_task_state(struct task_t *t);
int      a20_pa_pid_max(void);
int      a20_pa_pid_next(void);
void    *a20_pa_task_mm(struct task_t *t);
void    *a20_pa_mm_pgdir(struct mm_struct *mm);
void    *a20_pa_first_vma(struct mm_struct *mm);
void    *a20_pa_next_vma(struct mm_seg *vma);
uint64_t a20_pa_vma_start(struct mm_seg *vma);
uint64_t a20_pa_vma_end(struct mm_seg *vma);
uint64_t a20_pa_vma_flags(struct mm_seg *vma);
int      a20_pa_query_leaf(void *pgdir, uint64_t va, a20_leaf_info_t *out);

/* Rust reports. */
void   a20_proc_vm_stats(a20_vm_stats_t *stats);
size_t a20_proc_format_pidmap(char *buf, size_t bufsz);

#endif /* A20_PROC_PROC_ACCOUNTING_H */
