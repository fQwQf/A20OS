/*
 * fscompat/proc/proc.h — 用户态 FS 宿主的最小任务上下文。
 *
 * 磁盘文件系统源码仅使用 proc_current() 与 task_t 的 uid/gid/pid 字段
 * （权限判定路径）；单用户服务进程中固定为 root。
 */
#ifndef _PROC_H
#define _PROC_H

#include "core/types.h"

#define MAX_GROUPS 32

typedef struct proc_cred {
    int      uid;
    int      euid;
    int      suid;
    int      fsuid;
    int      gid;
    int      egid;
    int      sgid;
    int      fsgid;
    int      ngroups;
    int      groups[MAX_GROUPS];
    uint64_t cap_effective;
    uint64_t cap_permitted;
    uint64_t cap_inheritable;
    uint64_t cap_bounding;
} proc_cred_t;

typedef struct task_t {
    uint32_t    pid;
    proc_cred_t cred;
} task_t;

task_t *proc_current(void);

#endif /* _PROC_H */
