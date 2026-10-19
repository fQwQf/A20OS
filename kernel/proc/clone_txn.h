#ifndef A20_PROC_CLONE_TXN_H
#define A20_PROC_CLONE_TXN_H

#include "core/types.h"
#include "proc/task_construction.h"

/*
 * C/Rust ABI for the fork/clone transaction policy in kernel/proc/clone_txn.rs.
 *
 * Rust owns the transaction: which stages run, what each one inherits, the
 * publication boundary, the CLONE_VFORK arming and its rollback, and the
 * deferred-ready handshake.  C keeps the mechanical operations it already
 * owns -- task and pid allocation, fd/MM object copies, namespace forks, list
 * linking under tasklist_lock, architecture frame construction and usercopy --
 * and exposes them through the bridges below.
 *
 * `clone_view_t` borrows the live fields of one task so Rust can read the
 * parent's inheritance inputs and write the child's policy outputs in place.
 * The task layout does not change and no task state is duplicated on the Rust
 * side.  The adapter fills both the parent and the child view from the same
 * struct type, so a field reorder or an added pointer must fail to build.
 */

typedef struct clone_view {
    /* Child fields the transaction writes. */
    int       *pid;
    int       *tgid;
    int       *ppid;
    void     **parent;
    void     **tg_leader;
    uint32_t  *abi_mode;
    int       *exit_signal;
    int       *clone_flags;
    int      **clear_child_tid;
    int       *sched_policy;
    int       *priority;
    uint32_t  *cfs_weight;
    vaddr_t   *exec_load_addr;
    size_t    *exec_load_size;
    char      *name;
    /* Parent fields the transaction reads. */
    const int      *sched_reset_on_fork;
    const uint64_t *nproc_limit;
} clone_view_t;

/* The Rust view is a fixed sequence of seventeen pointers. */
_Static_assert(sizeof(clone_view_t) == 17 * sizeof(void *),
               "clone_view_t must match the Rust CloneView layout");
_Static_assert(__builtin_offsetof(clone_view_t, pid) == 0,
               "clone_view_t.pid offset");
_Static_assert(__builtin_offsetof(clone_view_t, clear_child_tid) ==
                   8 * sizeof(void *),
               "clone_view_t.clear_child_tid offset");
_Static_assert(__builtin_offsetof(clone_view_t, name) == 14 * sizeof(void *),
               "clone_view_t.name offset");
_Static_assert(__builtin_offsetof(clone_view_t, sched_reset_on_fork) ==
                   15 * sizeof(void *),
               "clone_view_t.sched_reset_on_fork offset");
_Static_assert(__builtin_offsetof(clone_view_t, nproc_limit) ==
                   16 * sizeof(void *),
               "clone_view_t.nproc_limit offset");

/* C adapters used by the Rust transaction.  Every task pointer is a live
 * task_t; the Rust side never dereferences one, it only passes it back. */
void      a20_clone_view_fill(task_t *t, clone_view_t *out);
int       a20_clone_task_pid(const task_t *t);
int       a20_clone_task_uid(const task_t *t);
int       a20_clone_flags_supported(uint64_t flags);
int       a20_clone_has_cap(const task_t *t, int cap);
uint32_t  a20_clone_weight_for_nice(int nice);
void      a20_clone_notify_begin(int parent_pid, uint64_t flags);
void      a20_clone_notify_ready(task_t *t, int child_pid, uint64_t flags);
void      a20_clone_inherit_stdio(task_t *t, task_t *parent);
void      a20_clone_inherit_capabilities(task_t *t, task_t *parent, uint64_t flags);
int       a20_clone_init_mm(task_t *t, task_t *parent, uint64_t flags, int child_pid);
int       a20_clone_init_arch(task_t *t, task_t *parent, uint64_t flags,
                              vaddr_t stack, vaddr_t tls);
uint64_t  a20_clone_tasklist_lock(void);
void      a20_clone_tasklist_unlock(uint64_t flags);
int       a20_clone_vfork_snapshot_create(task_t *parent);
void      a20_clone_vfork_arm(task_t *parent);
void      a20_clone_vfork_disarm(task_t *parent);
void      a20_clone_vfork_wait(task_t *child);

/* Rust entry point.  Returns the child pid, or a negative errno.  *out_task
 * receives a referenced child and makes the transaction deferred-ready. */
int a20_proc_clone_impl(uint64_t flags, vaddr_t stack, int *ptid, vaddr_t tls,
                        int *ctid, int exit_signal, task_t **out_task);

#endif /* A20_PROC_CLONE_TXN_H */
