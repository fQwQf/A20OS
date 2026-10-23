#ifndef _IPC_LANDLOCK_H
#define _IPC_LANDLOCK_H

/*
 * Landlock LSM subset for A20OS.
 *
 * Implements the landlock_create_ruleset / landlock_add_rule /
 * landlock_restrict_self syscalls on top of a per-process ruleset stored in
 * the task. READ_FILE/WRITE_FILE rules match canonical path prefixes at the
 * open boundary, including openat2 and proc-fd reopens, before side effects.
 * Directory mutation, inode-anchored rules and policy stacking are outside
 * this subset; the Linux creation ABI rejects unsupported handled bits.
 */

#include "core/types.h"

struct task_t;

#define LANDLOCK_MAX_RULES 64
#define LANDLOCK_RULE_MAX_PREFIX 256

/* Landlock rule types and access-right bits (Linux ABI). */
#define LANDLOCK_RULE_PATH_BENEATH 1
#define LANDLOCK_ACCESS_FS_EXECUTE       (1ULL << 0)
#define LANDLOCK_ACCESS_FS_WRITE_FILE    (1ULL << 1)
#define LANDLOCK_ACCESS_FS_READ_FILE     (1ULL << 2)
#define LANDLOCK_ACCESS_FS_READ_DIR      (1ULL << 3)
#define LANDLOCK_ACCESS_FS_REMOVE_DIR    (1ULL << 4)
#define LANDLOCK_ACCESS_FS_REMOVE_FILE   (1ULL << 5)
#define LANDLOCK_ACCESS_FS_MAKE_CHAR     (1ULL << 6)
#define LANDLOCK_ACCESS_FS_MAKE_DIR      (1ULL << 7)
#define LANDLOCK_ACCESS_FS_MAKE_REG      (1ULL << 8)
#define LANDLOCK_ACCESS_FS_MAKE_SOCK     (1ULL << 9)
#define LANDLOCK_ACCESS_FS_MAKE_FIFO     (1ULL << 10)
#define LANDLOCK_ACCESS_FS_MAKE_BLOCK    (1ULL << 11)
#define LANDLOCK_ACCESS_FS_MAKE_SYM      (1ULL << 12)
#define LANDLOCK_ACCESS_FS_REFER         (1ULL << 13)
#define LANDLOCK_ACCESS_FS_TRUNCATE      (1ULL << 14)
#define LANDLOCK_ACCESS_FS_IOCTL_DEV     (1ULL << 15)
#define LANDLOCK_ACCESS_FS_ALL (LANDLOCK_ACCESS_FS_EXECUTE | \
                                LANDLOCK_ACCESS_FS_WRITE_FILE | \
                                LANDLOCK_ACCESS_FS_READ_FILE | \
                                LANDLOCK_ACCESS_FS_READ_DIR | \
                                LANDLOCK_ACCESS_FS_REMOVE_DIR | \
                                LANDLOCK_ACCESS_FS_REMOVE_FILE | \
                                LANDLOCK_ACCESS_FS_MAKE_CHAR | \
                                LANDLOCK_ACCESS_FS_MAKE_DIR | \
                                LANDLOCK_ACCESS_FS_MAKE_REG | \
                                LANDLOCK_ACCESS_FS_MAKE_SOCK | \
                                LANDLOCK_ACCESS_FS_MAKE_FIFO | \
                                LANDLOCK_ACCESS_FS_MAKE_BLOCK | \
                                LANDLOCK_ACCESS_FS_MAKE_SYM | \
                                LANDLOCK_ACCESS_FS_REFER | \
                                LANDLOCK_ACCESS_FS_TRUNCATE | \
                                LANDLOCK_ACCESS_FS_IOCTL_DEV)

/* Ruleset state is owned by the Rust IPC engine and stored opaquely in task/vfile. */
typedef struct landlock_ruleset landlock_ruleset_t;

/* Create a ruleset with @handled_access_fs.  Returns a global VFS fd (a
 * ruleset handle) or a negative errno. */
int landlock_create_ruleset(uint64_t handled_access_fs);

/* Copy a Linux path-beneath attribute from @attr_user and resolve its dirfd.
 * The ruleset must still be mutable. Returns 0 or a negative errno. */
int landlock_add_rule(int ruleset_fd, int rule_type, const void *attr_user,
                      unsigned flags);

/* Apply the ruleset to the current process.  Returns 0 or a negative errno. */
int landlock_restrict_self(int ruleset_fd, unsigned flags);

/* Enforcement hook: returns 0 if @path is allowed for the current task under
 * Landlock, or -EACCES if a restricted prefix denies it. */
int landlock_check_path(const char *path, uint64_t needed_access);

/* Task construction: take a child-owned pin before the child is published. */
void landlock_inherit_task(struct task_t *child, const struct task_t *parent);

/* Release a process's rulesets (task teardown). */
void landlock_release_task(struct task_t *t);

#endif /* _IPC_LANDLOCK_H */
