#ifndef _FDTABLE_H
#define _FDTABLE_H

#include "core/types.h"
#include "core/refcount.h"
#include "proc/proc.h"

typedef struct files_struct {
    refcount_t refcount;
    int     fd[MAX_FILES];
    uint8_t cloexec[MAX_FILES];
    uint64_t open_mask[(MAX_FILES + 63) / 64];
    int     next_fd;
} files_struct_t;

void fdtable_init(task_t *task);
void fdtable_init_stdio(task_t *task);
void fdtable_copy(task_t *dst, const task_t *src);
void fdtable_share(task_t *dst, const task_t *src);
int  fdtable_unshare(task_t *task);
void fdtable_close_all(task_t *task);
void fdtable_close_on_exec(task_t *task);

int  fdtable_get(task_t *task, int fd);
int  fdtable_get_current(int fd);
int  fdtable_install(task_t *task, int gfd, int flags);
int  fdtable_install_current(int gfd, int flags);
int  fdtable_close(task_t *task, int fd);
int  fdtable_close_current(int fd);
int  fdtable_dup(task_t *task, int oldfd, int minfd, int flags);
int  fdtable_dup_to(task_t *task, int oldfd, int newfd, int flags);
int  fdtable_get_cloexec(task_t *task, int fd);
int  fdtable_set_cloexec(task_t *task, int fd, int cloexec);

#endif /* _FDTABLE_H */
