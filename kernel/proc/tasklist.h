#ifndef A20_PROC_TASKLIST_H
#define A20_PROC_TASKLIST_H

#include "core/types.h"

/*
 * C/Rust ABI for the task-list, children and thread-group membership in
 * kernel/proc/tasklist.rs.
 *
 * Rust owns the membership transitions: joining and leaving the global
 * all-task list, the parent/children chain, the thread-group chain and the
 * leader pin.  C keeps the intrusive link fields, the two list ends, the
 * locks that serialize them, the task reference count and the lifecycle phase
 * advance.  The accessors below exist because a transition rewires *other*
 * tasks' links, so a borrowed view of one task would not be enough.
 *
 * Every entry point runs with tasklist_lock held by the adapter, except
 * a20_tasklist_link_newborn, which takes it itself as the C version did.
 */

struct task_t;

/* Link fields. */
struct task_t *a20_tl_all_prev(const struct task_t *t);
void        a20_tl_set_all_prev(struct task_t *t, struct task_t *v);
struct task_t *a20_tl_all_next(const struct task_t *t);
void        a20_tl_set_all_next(struct task_t *t, struct task_t *v);
struct task_t *a20_tl_sibling_next(const struct task_t *t);
void        a20_tl_set_sibling_next(struct task_t *t, struct task_t *v);
struct task_t **a20_tl_sibling_prev_ptr(const struct task_t *t);
void        a20_tl_set_sibling_prev_ptr(struct task_t *t, struct task_t **v);
struct task_t **a20_tl_children_slot(struct task_t *t);
struct task_t **a20_tl_sibling_next_slot(struct task_t *t);
struct task_t *a20_tl_tg_next(const struct task_t *t);
void        a20_tl_set_tg_next(struct task_t *t, struct task_t *v);
struct task_t **a20_tl_tg_prev_ptr(const struct task_t *t);
void        a20_tl_set_tg_prev_ptr(struct task_t *t, struct task_t **v);
struct task_t **a20_tl_tg_next_slot(struct task_t *t);
struct task_t *a20_tl_parent(const struct task_t *t);
void        a20_tl_set_parent(struct task_t *t, struct task_t *v);
void        a20_tl_set_ppid(struct task_t *t, int v);
int         a20_tl_pid(const struct task_t *t);
struct task_t *a20_tl_children(const struct task_t *t);
void        a20_tl_set_children(struct task_t *t, struct task_t *v);
struct task_t *a20_tl_tg_leader(const struct task_t *t);
void        a20_tl_set_tg_leader(struct task_t *t, struct task_t *v);
int         a20_tl_tg_leader_ref_held(const struct task_t *t);
void        a20_tl_set_tg_leader_ref_held(struct task_t *t, int v);

/* The two ends of the global list. */
struct task_t *a20_tl_list_head(void);
void         a20_tl_set_list_head(struct task_t *v);
struct task_t *a20_tl_list_tail(void);
void         a20_tl_set_list_tail(struct task_t *v);

/* Locks, references, the phase advance and the diagnostics. */
uint64_t    a20_tl_tasklist_lock(void);
void        a20_tl_tasklist_unlock(uint64_t flags);
uint64_t    a20_tl_park_lock(struct task_t *t);
void        a20_tl_park_unlock(struct task_t *t, uint64_t flags);
struct task_t *a20_tl_task_get(struct task_t *t);
void        a20_tl_task_put(struct task_t *t);
int         a20_tl_lifecycle_advance(struct task_t *t, uint32_t event);
int         a20_tl_is_kernel_ptr(uintptr_t value);
void        a20_tl_report_corrupt(const struct task_t *t, const struct task_t *next);

/* Rust membership transitions. */
void        a20_tasklist_link_boot_locked(struct task_t *t);
int         a20_tasklist_publish_locked(struct task_t *t);
int         a20_tasklist_link_newborn(struct task_t *t);
void        a20_tasklist_children_link_locked(struct task_t *parent, struct task_t *child);
void        a20_tasklist_children_unlink_locked(struct task_t *t);
void        a20_tasklist_reparent_locked(struct task_t *new_parent, struct task_t *t);
int         a20_tasklist_tg_link_locked(struct task_t *t);
void        a20_tasklist_tg_unlink_locked(struct task_t *t);
void        a20_tasklist_tg_leader_ref_release(struct task_t *t);
void        a20_tasklist_unlink_locked(struct task_t *t);
struct task_t *a20_tasklist_first_locked(void);
struct task_t *a20_tasklist_next_locked(struct task_t *t);

#endif /* A20_PROC_TASKLIST_H */
