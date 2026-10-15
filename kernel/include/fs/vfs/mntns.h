#ifndef _FS_VFS_MNTNS_H
#define _FS_VFS_MNTNS_H

#include "fs/vfs.h"
#include "core/refcount.h"
#include "proc/nsclone.h"

struct task_t;

/*
 * Mount namespaces (first milestone of the namespace infrastructure).
 *
 * A mnt_namespace owns one private mount table.  The initial namespace is
 * statically allocated and never freed; every task without an explicit
 * namespace reference (kernel threads, tasks that joined the initial
 * namespace) implicitly belongs to it via a NULL task_t->mnt_ns.
 *
 * Reference model:
 * - fork/clone without CLONE_NEWNS shares the parent's namespace and takes
 *   one reference (mntns_fork()).
 * - unshare(CLONE_NEWNS) or clone(CLONE_NEWNS) deep-copies the caller's
 *   mount table into a fresh namespace (mntns_unshare()/mntns_fork()).
 * - An open /proc/<pid>/ns/mnt file pins its target namespace with one
 *   reference, so the fd remains a valid setns(2) target after the process
 *   leaves or exits.
 * - mntns_release_task() (called from the per-task teardown in
 *   fdtable_close_all) drops the task reference; the namespace is freed
 *   when the last reference goes away.
 *
 * Copied mounts share their root vnode and fs_data with the namespace they
 * were copied from. Each copied entry pins its source mount object; dropping
 * the source namespace is deferred until copied entries detach, so vnode->mnt
 * never points at a freed table entry.
 */
#define MNTNS_MAX_MOUNTS   64
#define MNTNS_INIT_INO     4026531840ULL  /* Linux-compatible init mnt ns ino */


typedef struct mnt_namespace {
    uint64_t    ino;        /* immutable id reported as mnt:[ino] */
    refcount_t  refs;
    int         nmounts;
    /* A last-put teardown that failed keeps one internal reference and the
     * table alive until the reaper can safely retry it. */
    int         teardown_pending;
    int         teardown_running;
    int         teardown_retryable;
    uint64_t    teardown_attempt_pass;
    /* Heap-allocated mount_t objects behind stable pointers.  The previous
     * inline array shifted entries on umount, silently repointing every
     * vnode->mnt / dcache / quota reference at the wrong mount.  Removed
     * entries are parked on dead_mounts (objects stay allocated — holders
     * may outlive the umount) and freed only when the namespace dies. */
    mount_t    *mounts[MNTNS_MAX_MOUNTS];
    mount_t    *dead_mounts;             /* intrusive mount_t.dead_next list */
    struct mnt_namespace *next;  /* registry link (g_mntns_lock) */
} mnt_namespace_t;

void             mntns_early_init(void);
/* Namespace that VFS mount-table operations apply to: the current task's
 * namespace, or the initial namespace when there is no current task or the
 * task never acquired a private one. */
mnt_namespace_t *mntns_current(void);
uint64_t         mntns_task_ino(const struct task_t *t);
/* Returns the task's namespace (the initial namespace when the task has no
 * private one) with a reference held; mntns_put() releases it. */
mnt_namespace_t *mntns_task_get(struct task_t *t);
/* unshare(CLONE_NEWNS): give @t a private deep copy of its namespace. */
int              mntns_unshare(struct task_t *t);
/* Clone-time inheritance: shares the parent's namespace, or deep-copies it
 * when clone_flags carries CLONE_NEWNS. */
int              mntns_fork(struct task_t *child, struct task_t *parent,
                            uint64_t clone_flags);
/* setns(2): move @t into @ns, consuming the caller's reference. */
int              mntns_join(struct task_t *t, mnt_namespace_t *ns);
void             mntns_put(mnt_namespace_t *ns);
/* Retry tracked last-put teardown and shared-source mounts after a clone
 * detaches.  The retry worker is started lazily on first failure. */
void             mntns_retry_pending(void);
/* Serialize mount-table snapshots, insertion/removal, and teardown admission. */
uint64_t         mntns_mount_table_lock(void);
void             mntns_mount_table_unlock(uint64_t flags);
/* Idempotent task-teardown drop of t->mnt_ns. */
void             mntns_release_task(struct task_t *t);

/* Orderly system shutdown admission/snapshot. begin pins every registered
 * namespace while holding the registry lock. The snapshot remains stable
 * until abort; a successful shutdown intentionally retains the pins. */
int              mntns_shutdown_begin(void);
void             mntns_shutdown_abort(void);
int              mntns_shutdown_active(void);
int              mntns_shutdown_namespace_count(void);
mnt_namespace_t *mntns_shutdown_namespace_at(int index);
/* Enter/leave an in-flight VFS mutation. Shutdown admission atomically
 * closes this gate and refuses to start while admitted mutations remain. */
int              mntns_mutation_enter(void);
void             mntns_mutation_leave(void);
int              mntns_fileop_enter(void);
void             mntns_fileop_leave(void);

#endif /* _FS_VFS_MNTNS_H */
