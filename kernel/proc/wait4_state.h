#ifndef _PROC_WAIT4_STATE_H
#define _PROC_WAIT4_STATE_H

#include <stddef.h>
#include <stdint.h>

/*
 * Rust wait4 controller ABI (kernel/proc/wait4_state.rs).
 *
 * Rust owns the child-matching rules, reap/report classification, status
 * encoding, and the scan -> reap / scan -> park transaction order. The C
 * adapter in kernel/proc/wait.c supplies the IRQ-save locks, the children and
 * thread-group chain walks, field sampling, task pin/detach/destroy/release,
 * child-time accounting, the park/wake protocol and proc_yield().
 *
 * All views are plain integer data with identical LP64/ILP32 layout; the Rust
 * side declares them #[repr(C)] in the same field order and pins the layout
 * with the static assertions below.
 */

/* One child's matching facts, sampled under tasklist_lock. parent_tgid and
 * parent_pid are -1 when the child's parent link is NULL. direct_child is the
 * sampled fact `child->parent == waiter || child->ppid == waiter->pid`; the
 * waiter_* fields are the waiting task's own ids. The adapter performs the
 * locked loads; Rust applies every selection rule to these values. */
typedef struct wait4_child_view {
    int32_t state;
    int32_t pid;
    int32_t pgid;
    int32_t parent_tgid;
    int32_t parent_pid;
    int32_t waiter_tgid;
    int32_t waiter_pid;
    int32_t waiter_pgid;
    uint8_t direct_child;
    uint8_t reserved[3];
} wait4_child_view_t;

_Static_assert(sizeof(wait4_child_view_t) == 36,
               "wait4 child view must match Rust repr(C)");
_Static_assert(offsetof(wait4_child_view_t, parent_tgid) == 12 &&
               offsetof(wait4_child_view_t, direct_child) == 32,
               "wait4 child view offsets must match Rust repr(C)");

/* The child's stop/continue facts, sampled by the adapter under the child's
 * own park_lock immediately before a20_wait4_report_decide(). `state` is the
 * caller's outer task-state sample. */
typedef struct wait4_report_view {
    int32_t state;
    int32_t options;
    int32_t stop_report_pending;
    int32_t continue_report_pending;
    int32_t ptrace_stop_active;
    int32_t ptrace_event;
    int32_t exit_code;
    int32_t reserved;
} wait4_report_view_t;

_Static_assert(sizeof(wait4_report_view_t) == 32,
               "wait4 report view must match Rust repr(C)");
_Static_assert(offsetof(wait4_report_view_t, exit_code) == 24,
               "wait4 report view offsets must match Rust repr(C)");

/* a20_wait4_report_decide() action bits: report bits say which event the
 * waiter received; clear bits say which pending flags the adapter must clear
 * (never set under WNOWAIT). */
enum {
    WAIT4_REPORT_NONE = 0,
    WAIT4_REPORT_STOP = 1,
    WAIT4_REPORT_CONT = 2,
    WAIT4_REPORT_CLEAR_STOP = 4,
    WAIT4_REPORT_CLEAR_CONT = 8,
};

typedef uint64_t (*wait4_scan_begin_fn)(void *context);
typedef void (*wait4_scan_unlock_fn)(void *context, uint64_t flags);
typedef void (*wait4_yield_fn)(void *context);
typedef void *(*wait4_group_start_fn)(void *context, int whole_group);
typedef void *(*wait4_chain_next_fn)(void *context, void *node);
typedef int (*wait4_child_view_fn)(void *context, void *child,
                                   wait4_child_view_t *out);
typedef int (*wait4_child_flag_fn)(void *context, void *child);
typedef void *(*wait4_reap_pin_fn)(void *context, void *child, int32_t want_pin,
                                   int32_t *exit_code_out);
typedef void (*wait4_child_void_fn)(void *context, void *child);
typedef int (*wait4_report_take_fn)(void *context, void *child, int state);
typedef int (*wait4_park_publish_fn)(void *context);
typedef int (*wait4_park_commit_fn)(void *context);
typedef void (*wait4_context_void_fn)(void *context);

/* Callback contracts:
 * - scan_begin scans the caller's chain under tasklist_lock and returns its
 *   IRQ-save flags; scan_unlock must receive exactly those flags. The chain
 *   and sampling callbacks run with tasklist_lock held. Sampling takes only
 *   the child's own park_lock, one task at a time (tasklist_lock -> park_lock).
 * - group_start(whole_group) selects the waiting thread (0) or its group
 *   leader (1); member_next/child_first/child_next are raw chain walkers.
 * - group_dead / is_current_any_cpu are the existing C predicates.
 * - reap_pin runs under the child's park_lock and always samples exit_code.
 *   It takes a task reference (proc_get) only when want_pin is nonzero and
 *   returns that pinned task, or NULL when the reference cannot be taken;
 *   with want_pin == 0 it returns the child as an unpinned alias that must
 *   never be passed to reap_release. reap_detach runs with tasklist_lock held;
 *   reap_release runs after scan_unlock and accounts, destroys and drops the
 *   pinned reference.
 * - report_take runs under the child's park_lock, fills the report view, calls
 *   a20_wait4_report_decide(), applies its clear bits and returns its action.
 * - park_publish runs with tasklist_lock still held: it publishes
 *   waiting_for_child, prepares the interruptible park token, samples the
 *   unblocked-signal condition, self-wakes on it, and returns that flag. The
 *   caller orders scan_unlock, then park_commit, park_finish, park_clear.
 */
typedef struct wait4_ops {
    void *context;
    wait4_scan_begin_fn scan_begin;
    wait4_scan_unlock_fn scan_unlock;
    wait4_yield_fn yield_now;
    wait4_group_start_fn group_start;
    wait4_chain_next_fn member_next;
    wait4_chain_next_fn child_first;
    wait4_chain_next_fn child_next;
    wait4_child_view_fn child_view;
    wait4_child_flag_fn group_dead;
    wait4_child_flag_fn is_current_any_cpu;
    wait4_reap_pin_fn reap_pin;
    wait4_child_void_fn reap_detach;
    wait4_child_void_fn reap_release;
    wait4_report_take_fn report_take;
    wait4_park_publish_fn park_publish;
    wait4_park_commit_fn park_commit;
    wait4_context_void_fn park_finish;
    wait4_context_void_fn park_clear;
} wait4_ops_t;

_Static_assert(offsetof(wait4_ops_t, context) == 0 &&
               offsetof(wait4_ops_t, scan_begin) == sizeof(void *) &&
               offsetof(wait4_ops_t, park_clear) == 18 * sizeof(void *),
               "wait4 ops table must match Rust repr(C)");
_Static_assert(sizeof(wait4_ops_t) == 19 * sizeof(void *),
               "wait4 ops table size must match Rust repr(C)");

/* Run one wait4 request. `status` is NULL or the waiter's writable int;
 * `target_task` is NULL or the pinned task pointer to match by identity. */
int a20_wait4_run(const wait4_ops_t *ops, int pid, int *status, int options,
                  void *target_task);

/* Decide one stop/continue report while the child's park_lock is held. Writes
 * the wait status when `status` is non-NULL; returns the action bits above. */
int a20_wait4_report_decide(const wait4_report_view_t *view, int *status);

#endif
