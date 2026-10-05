#ifndef _PROC_PIDNS_H
#define _PROC_PIDNS_H

#include "core/refcount.h"
#include "core/lock.h"
#include "proc/nsclone.h"

struct task_t;

/*
 * PID namespaces.
 *
 * A pid_namespace owns one private id space.  The initial namespace is
 * statically allocated and never freed; a task with no explicit reference
 * belongs to it implicitly, and its id is the task's global task_t::pid --
 * which is exactly the existing allocation in kernel/proc/pid.c, so nothing
 * about the initial namespace changes.
 *
 * Ids are two-dimensional, exactly as in Linux: task_t keeps one id per
 * namespace LEVEL it is visible in (level 0 is the initial namespace), and a
 * namespace sits one level below its parent.  A task is a member of its
 * deepest namespace and is visible from that namespace and from every
 * ancestor, which is why a level-indexed array is the right shape: getpid()
 * inside a container must yield the container-local id, while the same task
 * must still be addressable by its initial-namespace id from outside.
 *
 * Reference model:
 * - A namespace holds one reference on its parent, so walking ->parent from a
 *   referenced namespace can never dangle.
 * - task_t::pid_ns and task_t::pid_ns_for_children each hold a reference.
 * - An open /proc/<pid>/ns/pid file pins its namespace, so setns(2) keeps
 *   working after the target exits.
 * - pidns_release_task() (per-task teardown) drops the task's references; the
 *   namespace is freed when the last one goes away.
 *
 * Level depth is bounded by PID_MAX_LEVELS.  clone(CLONE_NEWPID) past the
 * bound fails with -EUSERSRCH, which is what Linux reports when a namespace
 * cannot allocate another level.
 */
#define PID_MAX_LEVELS        4
#define PIDNS_INIT_INO        4026531836ULL
#define PIDNS_CHILD_MAX       32768

typedef struct pid_namespace {
    uint64_t    ino;              /* immutable id reported as pid:[ino] */
    refcount_t  refs;
    int         level;            /* 0 == initial namespace */
    struct pid_namespace *parent; /* one reference held by this namespace */
    spinlock_t  lock;
    int         next_pid;
    int         nr_allocated;     /* ids currently handed out */
    int         nr_tasks;         /* live members */
    struct pid_namespace *children;
    struct pid_namespace *sibling;   /* child list of ->parent */
    struct pid_namespace *next;      /* registry link */
    /* Per-namespace id bitmap.  Allocated with the namespace; the initial one
     * is the static pid bitmap in kernel/proc/pid.c and is never touched. */
    uint64_t   *id_bitmap;
    unsigned    id_words;
} pid_namespace_t;

void                 pidns_early_init(void);
/* The initial namespace: statically allocated, never freed, so callers that
 * must distinguish it from a heap namespace can compare against this. */
pid_namespace_t     *pidns_init_ns(void);

/* task_active_pid_ns(): the namespace the calling task's OWN ids are reported
 * in.  This is deliberately the task's membership (pid_ns) and not the
 * namespace its children join: unshare(CLONE_NEWPID) parks a fresh namespace
 * for the next fork, and the caller keeps reporting its old id until then.
 * Returning the children namespace here would make getpid() report 0 for a
 * task that has unshared but not yet forked. */
pid_namespace_t     *pidns_current(void);
/* Namespace the task itself is a member of (its deepest one). */
pid_namespace_t     *pidns_task_own(struct task_t *t);
/* Reference-taking accessors; pidns_put() releases. */
pid_namespace_t     *pidns_task_get(struct task_t *t);
pid_namespace_t     *pidns_get(pid_namespace_t *ns);
void                 pidns_put(pid_namespace_t *ns);
uint64_t             pidns_task_ino(struct task_t *t);

/* Id translation.  task_pid_nr_ns() returns 0 when @t is not visible in @ns,
 * which is Linux's answer for a process outside the caller's namespace --
 * getppid() from the init namespace's child therefore reports 0 for a parent
 * living in a nested namespace, and that is correct. */
int                  task_pid_nr_ns(struct task_t *t, pid_namespace_t *ns);
int                  task_ppid_nr_ns(struct task_t *t, pid_namespace_t *ns);
struct task_t      *pidns_find_get(pid_namespace_t *ns, int pid);
/* Resolve a user-supplied pid in the calling task's active namespace.  Every
 * syscall that takes a pid from user space must use this, not
 * proc_find_get(), or a container could address host processes by id. */
struct task_t      *proc_find_get_user(int pid);

/* Inheritance and membership changes. */
int                  pidns_fork(struct task_t *child, struct task_t *parent,
                               uint64_t clone_flags);
int                  pidns_unshare(struct task_t *t);
int                  pidns_join(struct task_t *t, pid_namespace_t *ns);
void                 pidns_release_task(struct task_t *t);

/* Visible from @ns?  True when the target is a member of @ns or of any
 * namespace beneath it. */
int                  pidns_visible(pid_namespace_t *ns, struct task_t *target);
/* Iterate the tasks visible from @ns -- a member of @ns or of any namespace
 * below it -- by walking the global task list under tasklist_lock.  Seeding with
 * *iter == NULL starts the walk; each call returns the next task with a
 * reference taken, or NULL when exhausted.  A namespace-local member list
 * would be faster, but keeping the walk on the one list every other subsystem
 * already uses means namespace teardown has no second list to get wrong. */
struct task_t      *pidns_next_visible(pid_namespace_t *ns, struct task_t **iter);
int                  pidns_nr_tasks(pid_namespace_t *ns);

#endif /* _PROC_PIDNS_H */