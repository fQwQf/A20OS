#ifndef _PROC_CRED_H
#define _PROC_CRED_H

#include "proc/proc.h"

/*
 * Kernel-internal POSIX credential transitions (kernel/proc/cred.c).
 *
 * The saved-id semantics, root/capability privilege rules and the
 * uid-transition capability recalculation are process-subsystem policy;
 * the Linux ABI layer only validates its wire arguments and calls these.
 * All setters return 0 or a negative errno.
 */

int cred_setuid(task_t *t, int uid);
int cred_setgid(task_t *t, int gid);
int cred_setreuid(task_t *t, int ruid, int euid);
int cred_setregid(task_t *t, int rgid, int egid);
int cred_setresuid(task_t *t, int ruid, int euid, int suid);
int cred_setresgid(task_t *t, int rgid, int egid, int sgid);

/* Return the previous fsuid/fsgid (Linux setfsuid(2) convention). */
int cred_setfsuid(task_t *t, int uid);
int cred_setfsgid(task_t *t, int gid);

/* Apply the Linux capset(2) EPERM matrix to a credential set that the
 * caller already extracted from wire data. */
int cred_capset_apply(proc_cred_t *cred, uint64_t new_effective,
                      uint64_t new_permitted, uint64_t new_inheritable);

#endif /* _PROC_CRED_H */
