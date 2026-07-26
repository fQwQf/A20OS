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

static const uint32_t sched_prio_to_weight[40] = {
     88,   110,   132,   156,   183,   215,   251,   292,
    341,   399,   465,   543,   635,   742,   867,  1013,
   1185,  1386,  1622,  1898,  2218,  2593,  3032,  3546,
   4148,  4851,  5668,  6623,  7738,  9041, 10562, 12340,
  14424, 16860, 19711, 23044, 26940, 31492, 36814, 43020,
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
unsigned proc_sched_select_cpu_locked(task_t *t);
void proc_sched_kick_cpu(unsigned cpu);
void proc_sched_stop_current(int exit_code);
void proc_sched_assert_task_locked(task_t *t);
unsigned proc_sched_task_runq_memberships_locked(task_t *t);
unsigned proc_current_owner_memberships_locked(task_t *t);
unsigned proc_current_slot_count_locked(void);
unsigned proc_current_lifetime_violations_locked(void);
unsigned proc_wait_timer_count_locked(void);
int proc_wait_timer_register_locked(task_t *t, uint64_t deadline,
                                    uint64_t wait_seq);
void proc_wait_timer_cancel_locked(task_t *t, uint64_t wait_seq);
void proc_runq_enqueue_locked(task_t *t);
void proc_runq_remove_locked(task_t *t);
task_t *proc_runq_pick_locked(void);
void sched_reap_zombies(void);

#endif
