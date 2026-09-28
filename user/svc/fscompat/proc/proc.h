/*
 * fscompat/proc/proc.h — the minimal task context of the user-space FS host.
 *
 * The disk filesystem sources only use proc_current() and the uid/gid/pid
 * fields of task_t (the permission-decision path); these are fixed to root in
 * the single-user service process.
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
