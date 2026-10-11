#ifndef _PROC_RESOURCE_NS_H
#define _PROC_RESOURCE_NS_H

#include "core/refcount.h"
#include "core/lock.h"
#include "proc/nsclone.h"
#include "fs/procfs.h"

struct task_t;
struct user_namespace;
struct cg_node;

/* UTS, IPC, network and cgroup namespace lifetime. Resource implementations
 * use the immutable inode as a domain key; open namespace fds pin the object.
 * The owning user namespace is fixed at creation, not the credentials of the
 * process that happens to open a namespace handle later. */
typedef struct resource_namespace {
    uint64_t ino;
    refcount_t refs;
    int kind;
    struct user_namespace *owner;
    spinlock_t lock;
    char hostname[65];
    char domainname[65];
    struct cg_node *cgroup_root;
    unsigned loopback_up;
    unsigned loopback_mtu;
} resource_namespace_t;

#define RESOURCE_NS_FLAGS                                                      \
    (LINUX_CLONE_NEWUTS | LINUX_CLONE_NEWIPC | LINUX_CLONE_NEWNET |            \
     LINUX_CLONE_NEWCGROUP)
void resource_ns_init(void);
resource_namespace_t *resource_ns_get(resource_namespace_t *ns);
void resource_ns_put(resource_namespace_t *ns);
resource_namespace_t *resource_ns_task_get(struct task_t *t, int kind);
resource_namespace_t *resource_ns_current(int kind);
uint64_t resource_ns_task_ino(struct task_t *t, int kind);
uint64_t resource_ns_ino(int kind);
int resource_ns_is_initial(const resource_namespace_t *ns);
int resource_ns_fork(struct task_t *child, struct task_t *parent,
                     uint64_t flags);
int resource_ns_unshare(struct task_t *t, uint64_t flags);
int resource_ns_join(struct task_t *t, resource_namespace_t *ns);
void resource_ns_release_task(struct task_t *t);
void resource_ns_cgroup_path(struct task_t *target, char *buf, size_t len);

/* IPC tables are partitioned by namespace and reaped on the last reference.
 * SHM mappings and open MQ descriptors retain their objects after removal. */
void sysv_msg_namespace_destroy(uint64_t ino);
void sysv_sem_namespace_destroy(uint64_t ino);
void sysv_shm_namespace_destroy(uint64_t ino);
void posix_mq_namespace_destroy(uint64_t ino);
#endif
