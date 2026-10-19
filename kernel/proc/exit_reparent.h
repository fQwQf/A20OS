#ifndef A20_PROC_EXIT_REPARENT_H
#define A20_PROC_EXIT_REPARENT_H

#include "core/types.h"

/*
 * C/Rust ABI for the orphan-reparenting and pdeathsig policy in
 * kernel/proc/exit_reparent.rs.
 *
 * Rust owns the reaper election, the group-dead reap decision, the pdeathsig
 * claim, the force-kill capture, the reparent target and the waiter wake
 * selection.  C keeps the tasklist and park locks, the task references, the
 * group-dead walk, the reparent/unlink primitives, the detach, the destroy and
 * the deferred wake/force-exit/signal calls.
 */

#define EXIT_REPARENT_MAX 64

typedef struct exit_reparent_lists {
    struct task_t *to_destroy[EXIT_REPARENT_MAX];
    int32_t pdeathsig_pids[EXIT_REPARENT_MAX];
    int32_t pdeathsig_signals[EXIT_REPARENT_MAX];
    int32_t force_kill_pids[EXIT_REPARENT_MAX];
} exit_reparent_lists_t;

typedef struct exit_reparent_result {
    struct task_t *wake_for;
    size_t destroy_count;
    size_t pdeathsig_count;
    size_t force_kill_count;
} exit_reparent_result_t;

void a20_ex_reparent_children(struct task_t *dead, struct task_t *reaper,
                              exit_reparent_lists_t *lists,
                              exit_reparent_result_t *out);

/* Adapter bridges (defined in kernel/proc/exit.c). */
int      a20_ex_exit_code(const struct task_t *t);
int      a20_ex_pid(const struct task_t *t);
int      a20_ex_tgid(const struct task_t *t);
struct task_t *a20_ex_tg_leader(const struct task_t *t);
struct task_t *a20_ex_tg_next(const struct task_t *t);
int      a20_ex_exit_signal(const struct task_t *t);
uint64_t a20_ex_clone_flags(const struct task_t *t);
struct task_t *a20_ex_children(const struct task_t *t);
struct task_t *a20_ex_sibling_next(const struct task_t *t);
struct task_t *a20_ex_idle_task(void);
int      a20_ex_state(struct task_t *t);
uint64_t a20_ex_park_lock(struct task_t *t);
void     a20_ex_park_unlock(struct task_t *t, uint64_t flags);
int      a20_ex_pdeathsig(const struct task_t *t);
void     a20_ex_set_pdeathsig(struct task_t *t, int sig);
int      a20_ex_tg_group_dead(struct task_t *t);
int      a20_ex_native_thread_waitable(struct task_t *child, struct task_t *reaper);
int      a20_ex_is_current_any_cpu(struct task_t *t);
void     a20_ex_reap_detach(struct task_t *t);
void     a20_ex_note_zombie(void);
struct task_t *a20_ex_get(struct task_t *t);

#endif /* A20_PROC_EXIT_REPARENT_H */
