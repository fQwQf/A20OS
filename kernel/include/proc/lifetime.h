#ifndef _PROC_LIFETIME_H
#define _PROC_LIFETIME_H

#include "core/types.h"

/*
 * STEP35_TASK_LIFETIME_DIAGNOSTICS:
 * These counters make Park/Wake, scheduler ownership, and task-reference
 * hand-offs observable without changing their semantics. Values ending in
 * "_errors" are monotonic; ownership counts must return to their pre-test
 * baseline after all children and waiters have been reaped.
 */
typedef struct proc_lifetime_stats {
    unsigned long task_objects;
    unsigned long task_refs;
    unsigned long listed_tasks;
    unsigned long listed_refs;
    unsigned long pid_entries;
    unsigned long runqueue_entries;
    unsigned long dispatching_tasks;
    unsigned long cpu_owned_tasks;
    unsigned long wait_entries;
    unsigned long wake_entries;
    unsigned long timeout_entries;
    unsigned long timeout_capacity;
    unsigned long timeout_full_failures;
    unsigned long timeout_duplicate_rejections;
    unsigned long timeout_stale_expirations;
    unsigned long timeout_heap_violations;
    unsigned long zombies;
    unsigned long ref_get_failures;
    unsigned long ref_underflows;
    unsigned long duplicate_destroy;
    unsigned long bad_final_put;
    unsigned long state_violations;
    unsigned long lifetime_errors;
} proc_lifetime_stats_t;

void proc_lifetime_snapshot(proc_lifetime_stats_t *stats);
size_t proc_lifetime_format(char *buf, size_t bufsz);

/* Internal ownership instrumentation. */
#ifndef CONFIG_MCU
void proc_lifetime_note_task_init(int dynamic);
void proc_lifetime_note_task_free(void);
void proc_lifetime_note_ref_get(void);
void proc_lifetime_note_ref_put(void);
void proc_lifetime_note_ref_get_failure(void);
void proc_lifetime_note_ref_underflow(void);
void proc_lifetime_note_duplicate_destroy(void);
void proc_lifetime_note_bad_final_put(void);
void proc_lifetime_note_pid_add(void);
void proc_lifetime_note_pid_remove(void);
void proc_lifetime_note_wait_add(void);
void proc_lifetime_note_wait_remove(void);
void proc_lifetime_note_wait_to_wake(void);
void proc_lifetime_note_wake_remove(void);
#else
static inline void proc_lifetime_note_task_init(int dynamic)
{
    (void)dynamic;
}
static inline void proc_lifetime_note_task_free(void) {}
static inline void proc_lifetime_note_ref_get(void) {}
static inline void proc_lifetime_note_ref_put(void) {}
static inline void proc_lifetime_note_ref_get_failure(void) {}
static inline void proc_lifetime_note_ref_underflow(void) {}
static inline void proc_lifetime_note_duplicate_destroy(void) {}
static inline void proc_lifetime_note_bad_final_put(void) {}
static inline void proc_lifetime_note_pid_add(void) {}
static inline void proc_lifetime_note_pid_remove(void) {}
static inline void proc_lifetime_note_wait_add(void) {}
static inline void proc_lifetime_note_wait_remove(void) {}
static inline void proc_lifetime_note_wait_to_wake(void) {}
static inline void proc_lifetime_note_wake_remove(void) {}
#endif

#endif
