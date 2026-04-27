#include "syscall_internal.h"

int syscall_sig_diag_count = 0;
int syscall_sleep_diag_count = 0;

int64_t syscall_get_global_fd(int fd) {
    task_t *t = proc_current();
    if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
    int gfd = t->fd_table[fd];
    if (gfd < 0) return -EBADF;
    return gfd;
}

int syscall_alloc_local_fd(task_t *t, int gfd) {
    if (!t || gfd < 0) return -EBADF;
    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_table[i] < 0) {
            t->fd_table[i] = gfd;
            t->fd_cloexec[i] = 0;
            return i;
        }
    }
    vfs_close(gfd);
    return -EMFILE;
}

int syscall_alloc_local_fd_with_flags(task_t *t, int gfd, int flags) {
    int fd = syscall_alloc_local_fd(t, gfd);
    if (fd >= 0)
        t->fd_cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
    return fd;
}
