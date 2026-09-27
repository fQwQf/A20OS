#ifndef _FS_VFS_MNTNS_H
#define _FS_VFS_MNTNS_H

#include "fs/vfs.h"
#include "core/refcount.h"

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
 * were copied from; such entries carry VFS_MOUNT_NS_SHARED and umount only
 * detaches them from the local table without running the filesystem
 * unmount destructor (the other namespace still uses the filesystem).
 */
#define MNTNS_MAX_MOUNTS   64
#define MNTNS_INIT_INO     4026531840ULL  /* Linux-compatible init mnt ns ino */

/* CLONE_NEW* namespace flag values (Linux uapi).  Declared here so both the
 * clone/unshare/setns implementation and listns(2) classify ns types from
 * one source. */
#define LINUX_CLONE_NEWNS      0x00020000ULL
#define LINUX_CLONE_NEWCGROUP  0x02000000ULL
#define LINUX_CLONE_NEWUTS     0x04000000ULL
#define LINUX_CLONE_NEWIPC     0x08000000ULL
#define LINUX_CLONE_NEWUSER    0x10000000ULL
#define LINUX_CLONE_NEWPID     0x20000000ULL
#define LINUX_CLONE_NEWNET     0x40000000ULL

/* Linux-compatible init-namespace inos for the ns types A20OS does not
 * implement as objects; they render as system-wide singletons and are
 * refused by unshare(2)/setns(2).  Shared by /proc/<pid>/ns and listns(2). */
#define MNTNS_INIT_INO_PID     4026531836ULL
#define MNTNS_INIT_INO_USER    4026531837ULL
#define MNTNS_INIT_INO_UTS     4026531838ULL
#define MNTNS_INIT_INO_IPC     4026531839ULL
#define MNTNS_INIT_INO_NET     4026531841ULL
#define MNTNS_INIT_INO_CGROUP  4026531835ULL

typedef struct mnt_namespace {
    uint64_t    ino;        /* immutable id reported as mnt:[ino] */
    refcount_t  refs;
    int         nmounts;
    mount_t     mounts[MNTNS_MAX_MOUNTS];
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
/* Idempotent task-teardown drop of t->mnt_ns. */
void             mntns_release_task(struct task_t *t);

#endif /* _FS_VFS_MNTNS_H */
