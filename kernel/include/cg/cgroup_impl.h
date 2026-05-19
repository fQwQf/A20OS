#ifndef _CG_CGROUP_IMPL_H
#define _CG_CGROUP_IMPL_H

#include "cg/cgroup.h"
#include "core/lock.h"

#define CG_MAX_CHILDREN 128
#define CG_MAX_FILES   64
#define CG_MAX_PIDS    64

typedef enum {
    CG_V1,
    CG_V2,
} cg_ver_t;

typedef enum {
    CF_TASKS,
    CF_CGROUP_PROCS,
    CF_NOTIFY_ON_RELEASE,
    CF_RELEASE_AGENT,
    CF_CLONE_CHILDREN,
    CF_EVENT_CONTROL,
    CF_CGROUP_CONTROLLERS,
    CF_CGROUP_SUBTREE_CONTROL,
    CF_CGROUP_KILL,
    CF_CGROUP_TYPE,
    CF_MEMORY_USAGE,
    CF_MEMORY_LIMIT,
    CF_MEMORY_MAX_USAGE,
    CF_MEMORY_STAT,
    CF_MEMORY_SWAPPINESS,
    CF_MEMORY_USE_HIERARCHY,
    CF_MEMORY_MEMSW_USAGE,
    CF_MEMORY_MEMSW_LIMIT,
    CF_MEMORY_KMEM_USAGE,
    CF_MEMORY_KMEM_LIMIT,
    CF_MEMORY_CURRENT,
    CF_MEMORY_MAX,
    CF_MEMORY_MIN,
    CF_MEMORY_LOW,
    CF_MEMORY_EVENTS,
    CF_MEMORY_SWAP_CURRENT,
    CF_MEMORY_SWAP_MAX,
    CF_CPUSET_CPUS,
    CF_CPUSET_MEMS,
    CF_CPUSET_MEMORY_MIGRATE,
    CF_CPU_CFS_QUOTA,
    CF_CPU_CFS_PERIOD,
    CF_CPU_SHARES,
    CF_CPU_STAT,
    CF_CPU_MAX,
    CF_FILE_MAX,
} cg_file_t;

typedef struct cg_node {
    char name[64];
    struct cg_node *parent;
    struct cg_node *children[CG_MAX_CHILDREN];
    int child_count;
    cg_file_t files[CG_MAX_FILES];
    int file_count;
    int is_root;
    int clone_children;
    int pids[CG_MAX_PIDS];
    int pid_count;
    uint32_t uid;
    uint32_t gid;
    spinlock_t lock;
    cg_resource_t res;
} cg_node_t;

typedef struct {
    cg_ver_t ver;
    uint32_t controllers;
    cg_node_t *root;
} cg_sb_t;

#define CTRL_MEMORY   (1 << 0)
#define CTRL_CPU      (1 << 1)
#define CTRL_CPUSET   (1 << 2)
#define CTRL_CPUACCT  (1 << 3)

#endif
