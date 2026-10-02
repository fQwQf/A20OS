#ifndef _FDTABLE_H
#define _FDTABLE_H

#include "core/types.h"
#include "core/refcount.h"
#include "core/lock.h"
#include "core/sync.h"
#include "proc/proc.h"

typedef struct vfile vfile_t;

/*
 * files_struct lifetime invariants:
 * - task_t.files points to one files_struct. fork-like sharing increments
 *   refcount; fdtable_unshare() creates a private copy before mutation when a
 *   task needs copy-on-write fd semantics.
 * - lock protects fd[], cloexec[], open_mask, and next_fd. Code that installs,
 *   duplicates, closes, or changes close-on-exec state must hold it.
 * - fd slots store vfile pointers directly; there is no global file-number
 *   space. An open slot holds exactly one vfile reference; install takes it
 *   from the caller (on failure the caller keeps it), and close, exec, and
 *   exit paths drop exactly one reference per open slot. References that pin
 *   a file outside any slot (VMAs, exec, native handles) hold their own
 *   vfile_get()/vfile_put() pair.
 */

/*
 * When there is no current task (boot paths before the first kthread),
 * fd operations resolve against this boot table so vfs_open()/stdio behave
 * the same way the old global table did for kernel context.
 */
typedef struct files_struct {
    spinlock_t lock;
    wait_queue_t readiness_waiters;
    refcount_t refcount;
    int     owners;
    int     release_owner_pid;
    struct vfile *fd[MAX_FILES];
    uint8_t cloexec[MAX_FILES];
    uint64_t open_mask[(MAX_FILES + 63) / 64];
    int     next_fd;
} files_struct_t;

struct vfile;

void fdtable_init(task_t *task);
void fdtable_init_stdio(task_t *task);
void fdtable_copy(task_t *dst, const task_t *src);
void fdtable_share(task_t *dst, const task_t *src);
int  fdtable_unshare(task_t *task);
void fdtable_close_all(task_t *task);
void fdtable_close_on_exec(task_t *task);

/* Resolve an open fd to a referenced vfile, or return NULL.  The reference
 * is dropped with vfs_put_file().  When @task is NULL the boot table (or,
 * failing that, the current task's table) is used. */
struct vfile *fdtable_get_file_ref(task_t *task, int fd, int *cloexec);
struct vfile *fdtable_get_current_file_ref(int fd);
/* Validate that @fd is open in @task; returns @fd or -EBADF.  Same-task
 * callers can feed the result straight back into the vfs_* fd API. */
int  fdtable_get(task_t *task, int fd);
int  fdtable_get_current(int fd);

/* Install @vf into the lowest free slot >= next_fd (respecting RLIMIT_NOFILE).
 * On success the slot takes over the caller's reference and the fd is
 * returned; on failure (EMFILE) the caller still owns @vf. */
int  fdtable_install_vfile(task_t *task, vfile_t *vf, int flags);
int  fdtable_install_current_vfile(vfile_t *vf, int flags);
int  fdtable_close(task_t *task, int fd);
int  fdtable_close_current(int fd);
int  fdtable_dup(task_t *task, int oldfd, int minfd, int flags);
int  fdtable_dup_current(int oldfd, int minfd, int flags);
int  fdtable_dup_to(task_t *task, int oldfd, int newfd, int flags);
int  fdtable_get_cloexec(task_t *task, int fd);
int  fdtable_set_cloexec(task_t *task, int fd, int cloexec);
wait_queue_t *fdtable_current_readiness_queue(void);
/* True when @fd currently resolves to the same open file (same vfile and
 * identity) it had when @vf/@identity were recorded. */
bool fdtable_current_matches_file(int fd, vfile_t *vf, uint64_t identity);
/* System-wide count of open fd slots across every task table (plus the boot
 * table).  Backs /proc-style fd accounting. */
size_t fdtable_open_fd_count(void);

#endif /* _FDTABLE_H */
