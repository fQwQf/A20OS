#ifndef _PROCFS_H
#define _PROCFS_H

#include "fs/vfs.h"

vnode_t *procfs_mount(void);

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
struct mnt_namespace *procfs_ns_file_mntns_get(const vfile_t *vf,
                                               int *out_owner_uid);

#endif
