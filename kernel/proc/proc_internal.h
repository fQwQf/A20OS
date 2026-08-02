#ifndef _PROC_INTERNAL_H
#define _PROC_INTERNAL_H

#include "proc/proc.h"
#include "core/lock.h"

#define SCHED_LEVELS 8
#define SCHED_NO_DEADLINE (~0ULL)

#ifndef CONFIG_DEBUG_SCHED_STATE
#ifdef DEBUG
#define CONFIG_DEBUG_SCHED_STATE 1
#else
#define CONFIG_DEBUG_SCHED_STATE 0
#endif
#endif

#define SCHED_NORMAL   0
#define SCHED_FIFO     1
#define SCHED_RR       2
#define SCHED_BATCH    3
#define SCHED_IDLE     5
#define SCHED_RESET_ON_FORK 0x40000000

#define CLONE_VM             0x00000100
#define CLONE_FILES          0x00000400
#define CLONE_SIGHAND        0x00000800
#define CLONE_VFORK          0x00004000
#define CLONE_PARENT         0x00008000
#define CLONE_THREAD         0x00010000
#define CLONE_SETTLS         0x00080000
#define CLONE_PARENT_SETTID  0x00100000
#define CLONE_CHILD_CLEARTID 0x00200000
#define CLONE_CHILD_SETTID   0x01000000

/*
 * nice -> weight table (Linux orientation): lower nice = higher priority =
 * larger weight = more CPU share.  This was previously stored in reverse,
 * which made nice levels behave as decorative only in the level scheduler.
 * EEVDF relies on this table being correct: a task's weight scales its
 * virtual runtime so proportional fairness actually holds.
 */
static const uint32_t sched_prio_to_weight[40] = {
    43020, 36814, 31492, 26940, 23044, 19711, 16860, 14424,
    12340, 10562,  9041,  7738,  6623,  5668,  4851,  4148,
     3546,  3032,  2593,  2218,  1898,  1622,  1386,  1185,
     1013,   867,   742,   635,   543,   465,   399,   341,
      292,   251,   215,   183,   156,   132,   110,    88,
};

static inline uint32_t sched_weight_for_nice(int nice)
{
    int idx = nice + 20;
    if (idx < 0) idx = 0;
    if (idx > 39) idx = 39;
    return sched_prio_to_weight[idx];
}

extern spinlock_t proc_lock;
void proc_sched_note_zombie(void);
task_t *proc_current_on_cpu(unsigned cpu);
int proc_task_is_current_any_cpu(task_t *task);
int proc_sched_resume_stopped(task_t *task, int report_continued);
void proc_wake_child_waiters_locked(task_t *parent);
void proc_switch_complete(void);

task_t *proc_idle_task(void);
task_t *proc_first_task_locked(void);
task_t *proc_next_task_locked(task_t *t);
void proc_unlink_task_locked(task_t *t);
task_t *proc_set_current(task_t *next);
pt_root_t *proc_kernel_pgdir_shared(void);

void proc_pid_init(void);
int proc_pid_alloc(void);
void proc_pid_register(task_t *t);
void proc_pid_unregister(task_t *t);
int proc_pid_next_value(void);

void proc_task_init_common(task_t *t, task_t *parent);
task_t *proc_task_alloc_storage(void);
void proc_task_init_idle_state(task_t *t, unsigned cpu);
void proc_task_first_entry(void) NORETURN;
void proc_destroy_task(task_t *t);
task_t *proc_alloc_task_slot(void);
void proc_complete_vfork(task_t *child);
void proc_reap_detach_locked(task_t *t);

void proc_sched_runq_init(void);
void proc_timer_heap_init(void);
int  proc_sched_timers_due(uint64_t now);
void sched_scan_timers(uint64_t now);
unsigned proc_sched_select_cpu_locked(task_t *t);
void proc_sched_kick_cpu(unsigned cpu);
void proc_sched_request_cpu(unsigned cpu, int priority);
void proc_sched_request_current(void);
void proc_sched_handle_reschedule_ipi(void);
void proc_sched_tick(int from_user);
int proc_sched_safe_point(void);
void proc_sched_stop_current(int exit_code);
void proc_sched_assert_task_locked(task_t *t);
unsigned proc_sched_task_runq_memberships_locked(task_t *t);
uint64_t proc_sched_task_runq_cpu_mask_locked(task_t *t);
int proc_sched_should_preempt_locked(task_t *t, unsigned cpu);
unsigned proc_current_owner_memberships_locked(task_t *t);
unsigned proc_current_slot_count_locked(void);
unsigned proc_current_lifetime_violations_locked(void);
unsigned proc_wait_timer_count_locked(void);
unsigned proc_wait_timer_capacity(void);
unsigned long proc_wait_timer_full_failures_locked(void);
unsigned long proc_wait_timer_duplicate_rejections_locked(void);
unsigned long proc_wait_timer_stale_expirations_locked(void);
unsigned proc_wait_timer_violations_locked(void);
int proc_wait_timer_register_locked(task_t *t, uint64_t deadline,
                                    uint64_t wait_seq);
void proc_wait_timer_cancel_locked(task_t *t, uint64_t wait_seq);
void proc_runq_enqueue_locked(task_t *t);
void proc_runq_remove_locked(task_t *t);
task_t *proc_runq_pick_local(void);
void sched_reap_zombies(void);

typedef struct proc_sched_diag {
    unsigned long runqueue_migrations;
    unsigned long runqueue_local_picks;
    unsigned long runqueue_empty_picks;
    unsigned long runqueue_lock_acquires;
    unsigned long runqueue_lock_contentions;
    unsigned long runqueue_parallel_pick_peak;
    unsigned long resched_requests;
    unsigned long resched_priority_requests;
    unsigned long resched_ipi_sent;
    unsigned long resched_ipi_acks;
    unsigned long resched_consumed;
    unsigned long resched_pending;
    unsigned long scheduler_violations;
} proc_sched_diag_t;

void proc_sched_diag_snapshot(proc_sched_diag_t *diag);

#endif
