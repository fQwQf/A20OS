#ifndef _PROC_TASK_PLACEMENT_H
#define _PROC_TASK_PLACEMENT_H

#include <stddef.h>
#include <stdint.h>

#define A20_TASK_PLACEMENT_MAX_CPUS 32U

/* Scheduler-owned, lock-free snapshot of the inputs used by CPU selection.
 * C derives the online/affinity/cgroup mask and per-CPU load from kernel
 * objects; Rust owns only the deterministic placement decision. */
typedef struct a20_task_placement_snapshot {
    uint32_t current_cpu;
    uint32_t nr_cpus;
    uint32_t eligible_mask;
    uint32_t prefer_current;
    uint32_t cpu_load[A20_TASK_PLACEMENT_MAX_CPUS];
} a20_task_placement_snapshot_t;

typedef struct a20_task_preemption_snapshot {
    uint32_t candidate_policy;
    int32_t candidate_priority;
    uint64_t candidate_deadline;
    uint32_t current_valid;
    uint32_t current_idle;
    uint32_t current_running;
    uint32_t same_task;
    uint32_t current_policy;
    int32_t current_priority;
    uint64_t current_deadline;
} a20_task_preemption_snapshot_t;

_Static_assert(sizeof(a20_task_placement_snapshot_t) == 144,
               "task placement snapshot ABI size");
_Static_assert(_Alignof(a20_task_placement_snapshot_t) == _Alignof(uint32_t),
               "task placement snapshot ABI alignment");
_Static_assert(offsetof(a20_task_placement_snapshot_t, current_cpu) == 0,
               "task placement current_cpu ABI offset");
_Static_assert(offsetof(a20_task_placement_snapshot_t, nr_cpus) == 4,
               "task placement nr_cpus ABI offset");
_Static_assert(offsetof(a20_task_placement_snapshot_t, eligible_mask) == 8,
               "task placement eligible_mask ABI offset");
_Static_assert(offsetof(a20_task_placement_snapshot_t, prefer_current) == 12,
               "task placement prefer_current ABI offset");
_Static_assert(offsetof(a20_task_placement_snapshot_t, cpu_load) == 16,
               "task placement cpu_load ABI offset");
_Static_assert(sizeof(a20_task_preemption_snapshot_t) == 48,
               "task preemption snapshot ABI size");
_Static_assert(_Alignof(a20_task_preemption_snapshot_t) == _Alignof(uint64_t),
               "task preemption snapshot ABI alignment");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, candidate_deadline) == 8,
               "task preemption candidate deadline ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, candidate_policy) == 0,
               "task preemption candidate policy ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, candidate_priority) == 4,
               "task preemption candidate priority ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, current_valid) == 16,
               "task preemption current_valid ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, current_idle) == 20,
               "task preemption current_idle ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, current_running) == 24,
               "task preemption current_running ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, same_task) == 28,
               "task preemption same_task ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, current_policy) == 32,
               "task preemption current policy ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, current_priority) == 36,
               "task preemption current priority ABI offset");
_Static_assert(offsetof(a20_task_preemption_snapshot_t, current_deadline) == 40,
               "task preemption current deadline ABI offset");

/* # Safety
 * snapshot must point to a live, aligned snapshot whose entire object,
 * including every cpu_load entry, is initialized. */
uint32_t a20_task_placement_select(
    const a20_task_placement_snapshot_t *snapshot);
uint32_t a20_task_placement_eligible_mask(uint32_t affinity,
                                          uint32_t cpuset,
                                          uint32_t online,
                                          uint32_t nr_cpus);
uint32_t a20_task_placement_should_preempt(
    const a20_task_preemption_snapshot_t *snapshot);
uint32_t a20_task_placement_strictly_outranks(
    const a20_task_preemption_snapshot_t *snapshot);

#endif
