#ifndef _PROCFS_H
#define _PROCFS_H

#include "fs/vfs.h"

vnode_t *procfs_mount(void);

/* Namespace objects referenced below.  Declared here so the accessors' out
 * parameters name the same type the implementations use rather than an
 * incomplete type invented inside a parameter list. */
struct user_namespace;

/* setns(2) support: kinds returned by procfs_ns_file_kind() for open
 * /proc/<pid>/ns/<type> files (-1 = not a namespace file). */
#define PROCNS_MNT    1
#define PROCNS_PID    2
#define PROCNS_UTS    3
#define PROCNS_USER   4
#define PROCNS_IPC    5
#define PROCNS_NET    6
#define PROCNS_CGROUP 7

int  procfs_ns_file_kind(const vfile_t *vf);

/*
 * The three *_get() accessors below hand setns(2) the object behind an open
 * /proc/<pid>/ns/<type> descriptor, plus the identity of the process that
 * owned it when the descriptor was opened:
 *
 *  out_owner_uid      the owner's GLOBAL uid, recorded at open time
 *  out_owner_userns   a NEW reference on the owner's user namespace, or NULL
 *
 * The user namespace reference is what makes the setns permission check
 * namespace-scoped rather than global: authority over a mount or pid
 * namespace is authority over the user namespace that owns it, and the owner
 * may exit long before setns runs.  Callers must userns_put() it.  It is NULL
 * only if the descriptor predates the pin (an already-open fd across a reboot
 * of this code is impossible) -- treat NULL as "no namespace-scoped answer".
 */
struct pid_namespace *procfs_ns_file_pidns_get(const vfile_t *vf,
                                              int *out_owner_uid,
                                              struct user_namespace
                                                  **out_owner_userns);
struct mnt_namespace *procfs_ns_file_mntns_get(const vfile_t *vf,
                                               int *out_owner_uid,
                                               struct user_namespace
                                                   **out_owner_userns);
struct user_namespace *procfs_ns_file_userns_get(const vfile_t *vf,
                                                 int *out_owner_uid);

#endif
