#include "syscall_internal.h"

int64_t sys_read(int fd, char *buf, size_t count) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    char *kbuf = kmalloc(4096);
    if (!kbuf) return -ENOMEM;
    int64_t total = 0;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > 4096) chunk = 4096;
        int64_t n = vfs_read(gfd, kbuf, chunk);
        if (n <= 0) { kfree(kbuf); return total > 0 ? total : n; }
        if (copy_to_user(buf + total, kbuf, (size_t)n) < 0) { kfree(kbuf); return -EFAULT; }
        total += n;
        if ((size_t)n < chunk) break;
    }
    kfree(kbuf);
    return total;
}

int64_t sys_write(int fd, const char *buf, size_t count) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    char *kbuf = kmalloc(4096);
    if (!kbuf) return -ENOMEM;
    int64_t total = 0;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > 4096) chunk = 4096;
        if (copy_from_user(kbuf, buf + total, chunk) < 0) { kfree(kbuf); return -EFAULT; }
        int64_t n = vfs_write(gfd, kbuf, chunk);
        if (n <= 0) { kfree(kbuf); return total > 0 ? total : n; }
        total += n;
        if ((size_t)n < chunk) break;
    }
    kfree(kbuf);
    return total;
}

int64_t sys_pread64(int fd, char *buf, size_t count, long off) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    long curoff = vfs_lseek(gfd, 0, SEEK_CUR);
    vfs_lseek(gfd, off, SEEK_SET);
    char *kbuf = kmalloc(4096);
    if (!kbuf) { vfs_lseek(gfd, curoff, SEEK_SET); return -ENOMEM; }
    int64_t total = 0;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > 4096) chunk = 4096;
        int64_t n = vfs_read(gfd, kbuf, chunk);
        if (n <= 0) { kfree(kbuf); vfs_lseek(gfd, curoff, SEEK_SET); return total > 0 ? total : n; }
        if (copy_to_user(buf + total, kbuf, (size_t)n) < 0) { kfree(kbuf); vfs_lseek(gfd, curoff, SEEK_SET); return -EFAULT; }
        total += n;
        if ((size_t)n < chunk) break;
    }
    vfs_lseek(gfd, curoff, SEEK_SET);
    kfree(kbuf);
    return total;
}

int64_t sys_pwrite64(int fd, char *buf, size_t count, long off) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    long curoff = vfs_lseek(gfd, 0, SEEK_CUR);
    vfs_lseek(gfd, off, SEEK_SET);
    char *kbuf = kmalloc(4096);
    if (!kbuf) { vfs_lseek(gfd, curoff, SEEK_SET); return -ENOMEM; }
    int64_t total = 0;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > 4096) chunk = 4096;
        if (copy_from_user(kbuf, buf + total, chunk) < 0) { kfree(kbuf); vfs_lseek(gfd, curoff, SEEK_SET); return -EFAULT; }
        int64_t n = vfs_write(gfd, kbuf, chunk);
        if (n <= 0) { kfree(kbuf); vfs_lseek(gfd, curoff, SEEK_SET); return total > 0 ? total : n; }
        total += n;
        if ((size_t)n < chunk) break;
    }
    vfs_lseek(gfd, curoff, SEEK_SET);
    kfree(kbuf);
    return total;
}

int64_t sys_writev(int fd, const void *iov, int iovcnt) {
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    struct iovec { char *base; size_t len; };
    int64_t total = 0;
    char *kbuf = kmalloc(4096);
    if (!kbuf) return -ENOMEM;
    for (int i = 0; i < iovcnt; i++) {
        struct iovec v;
        if (copy_from_user(&v, (const char *)iov + (size_t)i * sizeof(struct iovec), sizeof(v)) < 0) { kfree(kbuf); return -EFAULT; }
        if (!v.base || v.len == 0) continue;
        size_t done = 0;
        while (done < v.len) {
            size_t chunk = v.len - done;
            if (chunk > 4096) chunk = 4096;
            if (copy_from_user(kbuf, v.base + done, chunk) < 0) { kfree(kbuf); return -EFAULT; }
            int64_t n = vfs_write(gfd, kbuf, chunk);
            if (n < 0) { kfree(kbuf); return n; }
            total += n;
            done += (size_t)n;
            if ((size_t)n < chunk) break;
        }
    }
    kfree(kbuf);
    return total;
}

int64_t sys_readv(int fd, const void *iov, int iovcnt) {
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    struct iovec { char *base; size_t len; };
    int64_t total = 0;
    char *kbuf = kmalloc(4096);
    if (!kbuf) return -ENOMEM;
    for (int i = 0; i < iovcnt; i++) {
        struct iovec v;
        if (copy_from_user(&v, (const char *)iov + (size_t)i * sizeof(struct iovec), sizeof(v)) < 0) { kfree(kbuf); return -EFAULT; }
        if (!v.base || v.len == 0) continue;
        size_t done = 0;
        while (done < v.len) {
            size_t chunk = v.len - done;
            if (chunk > 4096) chunk = 4096;
            int64_t n = vfs_read(gfd, kbuf, chunk);
            if (n <= 0) { kfree(kbuf); return total > 0 ? total : n; }
            if (copy_to_user(v.base + done, kbuf, (size_t)n) < 0) { kfree(kbuf); return -EFAULT; }
            total += n;
            done += (size_t)n;
            if ((size_t)n < chunk) break;
        }
    }
    kfree(kbuf);
    return total;
}
int64_t sys_openat(int dirfd, const char *path, int flags, int mode) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) return pr;
    int gfd = vfs_open(full, flags, mode);
    if (gfd < 0) return gfd;
    task_t *t = proc_current();
    return syscall_alloc_local_fd_with_flags(t, gfd, flags);
}

int64_t sys_close(int fd) {
    task_t *t = proc_current();
    if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
    int gfd = t->fd_table[fd];
    if (gfd < 0) return -EBADF;
    t->fd_table[fd] = -1;
    t->fd_cloexec[fd] = 0;
    return vfs_close(gfd);
}

int64_t sys_lseek(int fd, long offset, int whence) {
    task_t *t = proc_current();
    if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
    int gfd = t->fd_table[fd];
    if (gfd < 0) return -EBADF;
    return vfs_lseek(gfd, offset, whence);
}

int64_t sys_dup(int fd) {
    task_t *t = proc_current();
    if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
    int gfd = t->fd_table[fd];
    if (gfd < 0) return -EBADF;
    vfile_t *vf = vfs_get_file(gfd);
    if (!vf) return -EBADF;
    /* Share the same global fd — just add a local fd table entry.
     * This matches Linux semantics: dup shares the open file description. */
    for (int newfd = 0; newfd < MAX_FILES; newfd++) {
        if (t->fd_table[newfd] < 0) {
            t->fd_table[newfd] = gfd;
            t->fd_cloexec[newfd] = 0;
            vf->ref_count++;
            return newfd;
        }
    }
    return -EMFILE;
}

int64_t sys_dup3(int oldfd, int newfd, int flags) {
    if (flags & ~O_CLOEXEC) return -EINVAL;
    task_t *t = proc_current();
    if (!t || oldfd < 0 || oldfd >= MAX_FILES || newfd < 0 || newfd >= MAX_FILES) return -EBADF;
    if (oldfd == newfd) return -EINVAL;
    int old_gfd = t->fd_table[oldfd];
    if (old_gfd < 0) return -EBADF;
    vfile_t *vf = vfs_get_file(old_gfd);
    if (!vf) return -EBADF;
    int cur_gfd = t->fd_table[newfd];
    if (cur_gfd >= 0) {
        vfs_close(cur_gfd);
    }
    t->fd_table[newfd] = old_gfd;
    t->fd_cloexec[newfd] = (flags & O_CLOEXEC) ? 1 : 0;
    vf->ref_count++;
    return newfd;
}

int64_t sys_fcntl(int fd, int cmd, long arg) {
    /* F_DUPFD=0, F_DUPFD_CLOEXEC=1030: share the same global fd */
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        task_t *t = proc_current();
        if (!t) return -ESRCH;
        if (fd < 0 || fd >= MAX_FILES) return -EBADF;
        int gfd = t->fd_table[fd];
        if (gfd < 0) return -EBADF;
        vfile_t *vf = vfs_get_file(gfd);
        if (!vf) return -EBADF;
        int minfd = (int)arg;
        if (minfd < 0) minfd = 0;
        for (int i = minfd; i < MAX_FILES; i++) {
            if (t->fd_table[i] < 0) {
                t->fd_table[i] = gfd;
                t->fd_cloexec[i] = (cmd == F_DUPFD_CLOEXEC) ? 1 : 0;
                vf->ref_count++;
                return i;
            }
        }
        return -EMFILE;
    }
    if (cmd == F_GETFD) {
        task_t *t = proc_current();
        if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
        if (t->fd_table[fd] < 0) return -EBADF;
        return t->fd_cloexec[fd] ? FD_CLOEXEC : 0;
    }
    if (cmd == F_SETFD) {
        task_t *t = proc_current();
        if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
        if (t->fd_table[fd] < 0) return -EBADF;
        t->fd_cloexec[fd] = (arg & FD_CLOEXEC) ? 1 : 0;
        return 0;
    }
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    int64_t r = vfs_fcntl(gfd, cmd, arg);
    if (r >= 0 && cmd == F_SETFL)
        net_set_nonblock((int)gfd, ((int)arg & O_NONBLOCK) != 0);
    return r;
}

int64_t sys_flock(int fd, int operation) {
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_flock((int)gfd, operation);
}

int64_t sys_pipe2(int *pipefd, int flags) {
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    if (!pipefd) return -EFAULT;
    int gfd[2];
    int r = vfs_pipe(gfd);
    if (r == 0) {
        if (flags & O_NONBLOCK) {
            vfile_t *rd = vfs_get_file(gfd[0]);
            vfile_t *wr = vfs_get_file(gfd[1]);
            if (rd) rd->flags |= O_NONBLOCK;
            if (wr) wr->flags |= O_NONBLOCK;
        }
        task_t *t = proc_current();
        int fd0 = syscall_alloc_local_fd_with_flags(t, gfd[0], flags);
        if (fd0 < 0) {
            vfs_close(gfd[1]);
            return fd0;
        }
        int fd1 = syscall_alloc_local_fd_with_flags(t, gfd[1], flags);
        if (fd1 < 0) {
            t->fd_table[fd0] = -1;
            t->fd_cloexec[fd0] = 0;
            vfs_close(gfd[1]);
            return fd1;
        }
        int user_fds[2] = {fd0, fd1};
        if (copy_to_user(pipefd, user_fds, sizeof(user_fds)) < 0) {
            t->fd_table[fd0] = -1;
            t->fd_table[fd1] = -1;
            t->fd_cloexec[fd0] = 0;
            t->fd_cloexec[fd1] = 0;
            vfs_close(gfd[0]);
            vfs_close(gfd[1]);
            return -EFAULT;
        }
    }
    return r;
}

int64_t sys_ioctl(int fd, unsigned long req, void *arg) {
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_ioctl(gfd, req, arg);
}

int64_t sys_sync(void) {
    return vfs_sync();
}

int64_t sys_fsync(int fd) {
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_fsync((int)gfd);
}

int64_t sys_fdatasync(int fd) {
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file((int)gfd);
    if (!vf) return -EBADF;
    kstat_t st;
    if (vfs_fstat((int)gfd, &st) < 0)
        return -EINVAL;
    if ((st.st_mode & S_IFMT) != S_IFREG)
        return -EINVAL;
    return vfs_fsync((int)gfd);
}

int64_t sys_ftruncate(int fd, size_t size) {
    int64_t gfd = syscall_get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_ftruncate(gfd, size);
}

int64_t sys_truncate(const char *path, size_t size) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_truncate(kpath, size);
}

int64_t sys_sendfile(int out_fd, int in_fd, long *off, size_t count) {
    if (count == 0) return 0;
    int64_t out_gfd = syscall_get_global_fd(out_fd);
    if (out_gfd < 0) return out_gfd;
    int64_t in_gfd = syscall_get_global_fd(in_fd);
    if (in_gfd < 0) return in_gfd;
    long user_off = 0;
    if (off && copy_from_user(&user_off, off, sizeof(long)) < 0) return -EFAULT;
    long cur_off = off ? user_off : vfs_lseek(in_gfd, 0, SEEK_CUR);
    long saved = off ? vfs_lseek(in_gfd, 0, SEEK_CUR) : 0;
    int64_t total = 0;
    char *sbuf = kmalloc(4096);
    if (!sbuf) return -ENOMEM;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > 4096) chunk = 4096;
        if (off) vfs_lseek(in_gfd, cur_off, SEEK_SET);
        int64_t n = vfs_read(in_gfd, sbuf, chunk);
        if (n <= 0) break;
        int64_t w = vfs_write(out_gfd, sbuf, (size_t)n);
        if (w < 0) { if (total == 0) total = w; break; }
        total += w;
        cur_off += w;
        if (w < n) break;
    }
    if (off) {
        copy_to_user(off, &cur_off, sizeof(long));
        vfs_lseek(in_gfd, saved, SEEK_SET);
    }
    kfree(sbuf);
    return total;
}

int64_t sys_ppoll(void *fds, int nfds, void *tmo, void *sigmask) {
    struct pollfd { int fd; short events; short revents; };
    struct pollfd *pfds = (struct pollfd *)fds;
    (void)sigmask;
    if (nfds < 0) return -EINVAL;
    if (nfds == 0) {
        task_t *t = proc_current();
        if (tmo) {
            uint64_t ts[2];
            if (copy_from_user(ts, tmo, sizeof(ts)) < 0) return -EFAULT;
            if (ts[1] >= 1000000000ULL) return -EINVAL;
            uint64_t ticks = ts[0] * TICKS_PER_SEC + ts[1] * TICKS_PER_SEC / 1000000000ULL;
            uint64_t until = timer_get_ticks() + (ticks ? ticks : 1);
            while (timer_get_ticks() < until) {
                if (signal_task_has_unblocked(t)) return -EINTR;
                proc_yield();
            }
            if (signal_task_has_unblocked(t)) return -EINTR;
            return 0;
        }
        for (;;) {
            if (signal_task_has_unblocked(t)) return -EINTR;
            if (t) t->state = PROC_BLOCKED;
            sched();
        }
    }
    if (!pfds) return -EFAULT;

    uint64_t timeout_ticks = 0;
    int has_timeout = tmo != NULL;
    if (tmo) {
        uint64_t ts[2];
        if (copy_from_user(ts, tmo, sizeof(ts)) < 0) return -EFAULT;
        if (ts[1] >= 1000000000ULL) return -EINVAL;
        timeout_ticks = ts[0] * TICKS_PER_SEC + ts[1] * TICKS_PER_SEC / 1000000000ULL;
    }
    uint64_t deadline = timer_get_ticks() + (timeout_ticks ? timeout_ticks : 1);

    signal_state_t *saved_ss = NULL;
    uint64_t saved_blocked = 0;
    if (sigmask) {
        task_t *t = proc_current();
        if (!t || !t->signals) return -EINVAL;
        uint64_t user_mask;
        if (copy_from_user(&user_mask, sigmask, sizeof(user_mask)) < 0) return -EFAULT;
        saved_ss = (signal_state_t *)t->signals;
        saved_blocked = saved_ss->blocked;
        saved_ss->blocked = signal_mask_from_user(user_mask) &
            ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
    }
#define PPOLL_RETURN(v) do { if (saved_ss) saved_ss->blocked = saved_blocked; return (v); } while (0)
    for (;;) {
        int ready = 0;
        for (int i = 0; i < nfds; i++) {
            struct pollfd pfd;
            if (copy_from_user(&pfd, &pfds[i], sizeof(pfd)) < 0) PPOLL_RETURN(-EFAULT);
            pfd.revents = 0;
            if (pfd.fd >= 0) {
                int64_t gfd = syscall_get_global_fd(pfd.fd);
                pfd.revents = gfd < 0 ? POLLNVAL : (short)vfs_poll_events((int)gfd, pfd.events);
                if (pfd.revents) ready++;
            }
            if (copy_to_user(&pfds[i].revents, &pfd.revents, sizeof(short)) < 0)
                PPOLL_RETURN(-EFAULT);
        }
        if (ready > 0) PPOLL_RETURN(ready);
        if (has_timeout && timeout_ticks == 0) PPOLL_RETURN(0);
        if (has_timeout && timer_get_ticks() >= deadline) PPOLL_RETURN(0);
        task_t *t = proc_current();
        if (signal_task_has_unblocked(t)) PPOLL_RETURN(-EINTR);
        if (has_timeout) {
            proc_yield();
        } else {
            if (t) t->state = PROC_BLOCKED;
            sched();
        }
    }
#undef PPOLL_RETURN
}

int64_t sys_poll(void *fds, int nfds, int timeout) {
    struct pollfd { int fd; short events; short revents; };
    struct pollfd *pfds = (struct pollfd *)fds;
    if (nfds < 0) return -EINVAL;
    if (nfds == 0) {
        if (timeout > 0) {
            uint64_t until = timer_get_ticks() + MS_TO_TICKS((uint64_t)timeout);
            while (timer_get_ticks() < until) proc_yield();
        }
        return 0;
    }
    if (!pfds) return -EFAULT;

    int has_timeout = timeout >= 0;
    uint64_t deadline = timer_get_ticks() + (timeout > 0 ? MS_TO_TICKS((uint64_t)timeout) : 0);
    for (;;) {
        int ready = 0;
        for (int i = 0; i < nfds; i++) {
            struct pollfd pfd;
            if (copy_from_user(&pfd, &pfds[i], sizeof(pfd)) < 0) return -EFAULT;
            pfd.revents = 0;
            if (pfd.fd >= 0) {
                int64_t gfd = syscall_get_global_fd(pfd.fd);
                pfd.revents = gfd < 0 ? POLLNVAL : (short)vfs_poll_events((int)gfd, pfd.events);
                if (pfd.revents) ready++;
            }
            if (copy_to_user(&pfds[i].revents, &pfd.revents, sizeof(short)) < 0)
                return -EFAULT;
        }
        if (ready > 0) return ready;
        if (has_timeout && timeout == 0) return 0;
        if (has_timeout && timer_get_ticks() >= deadline) return 0;
        proc_yield();
        if (!has_timeout) return 0;
    }
}

int64_t sys_epoll_create1(int flags) {
    if (flags & ~O_CLOEXEC) return -EINVAL;
    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) return -ENOMEM;
    memset(vf, 0, sizeof(*vf));
    vf->ref_count = 1;
    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) { kfree(vf); return -EMFILE; }
    return syscall_alloc_local_fd_with_flags(proc_current(), gfd, flags);
}

/* ============================================================
 * Directory / Path
 * ============================================================ */
static int fd_isset_user(int f, void *s) {
    long mask = 0;
    if (copy_from_user(&mask, &((long *)s)[f / 8 / sizeof(long)], sizeof(long)) < 0) return 0;
    return (mask & (1UL << (f % (8 * sizeof(long))))) != 0;
}

static int fd_clear_user(int f, void *s) {
    long *slot = &((long *)s)[f / 8 / sizeof(long)];
    long mask = 0;
    if (copy_from_user(&mask, slot, sizeof(long)) < 0) return -EFAULT;
    mask &= ~(1UL << (f % (8 * sizeof(long))));
    return copy_to_user(slot, &mask, sizeof(long)) < 0 ? -EFAULT : 0;
}

int64_t sys_select(int nfds, void *readfds, void *writefds,
                   void *exceptfds, void *timeout) {
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    (void)exceptfds;

    if (nfds < 0) return -EINVAL;
    uint64_t timeout_ticks = 0;
    int has_timeout = timeout != NULL;
    if (timeout) {
        uint64_t tv[2];
        if (copy_from_user(tv, timeout, sizeof(tv)) < 0) return -EFAULT;
        if (tv[1] >= 1000000ULL) return -EINVAL;
        timeout_ticks = tv[0] * TICKS_PER_SEC + tv[1] * TICKS_PER_SEC / 1000000ULL;
    }
    uint64_t deadline = timer_get_ticks() + (timeout_ticks ? timeout_ticks : 1);

    for (;;) {
        int ready_count = 0;

        for (int i = 0; i < nfds; i++) {
            int rg = -1;
            if (i >= 0 && i < MAX_FILES && t->fd_table[i] >= 0)
                rg = t->fd_table[i];

            if (readfds && fd_isset_user(i, readfds)) {
                if (rg < 0) return -EBADF;
                int rev = vfs_poll_events(rg, POLLIN);
                if (rev & (POLLIN | POLLHUP | POLLERR)) ready_count++;
                else if (fd_clear_user(i, readfds) < 0) return -EFAULT;
            }
            if (writefds && fd_isset_user(i, writefds)) {
                if (rg < 0) return -EBADF;
                int rev = vfs_poll_events(rg, POLLOUT);
                if (rev & (POLLOUT | POLLERR)) ready_count++;
                else if (fd_clear_user(i, writefds) < 0) return -EFAULT;
            }
            if (exceptfds && fd_isset_user(i, exceptfds)) {
                if (rg < 0) return -EBADF;
                if (fd_clear_user(i, exceptfds) < 0) return -EFAULT;
            }
        }

        if (ready_count > 0) return ready_count;
        if (has_timeout && timeout_ticks == 0) return 0;
        if (has_timeout && timer_get_ticks() >= deadline) return 0;
        proc_yield();
        if (!has_timeout) return 0;
    }
}

int64_t sys_pselect6(int nfds, void *readfds, void *writefds,
                     void *exceptfds, void *timeout, void *sigmask) {
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    if (nfds < 0) return -EINVAL;

    uint64_t timeout_ticks = 0;
    int has_timeout = timeout != NULL;
    if (timeout) {
        uint64_t ts[2];
        if (copy_from_user(ts, timeout, sizeof(ts)) < 0) return -EFAULT;
        if (ts[1] >= 1000000000ULL) return -EINVAL;
        timeout_ticks = ts[0] * TICKS_PER_SEC + ts[1] * TICKS_PER_SEC / 1000000000ULL;
    }
    uint64_t deadline = timer_get_ticks() + (timeout_ticks ? timeout_ticks : 1);

    signal_state_t *saved_ss = NULL;
    uint64_t saved_blocked = 0;
    if (sigmask) {
        uint64_t data[2];
        if (copy_from_user(data, sigmask, sizeof(data)) < 0) return -EFAULT;
        if (data[0]) {
            uint64_t user_mask = 0;
            if (copy_from_user(&user_mask, (void *)data[0], sizeof(user_mask)) < 0)
                return -EFAULT;
            if (!t->signals) return -EINVAL;
            saved_ss = (signal_state_t *)t->signals;
            saved_blocked = saved_ss->blocked;
            saved_ss->blocked = signal_mask_from_user(user_mask) &
                ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
        }
    }
#define PSELECT_RETURN(v) do { if (saved_ss) saved_ss->blocked = saved_blocked; return (v); } while (0)
    for (;;) {
        int ready_count = 0;

        for (int i = 0; i < nfds; i++) {
            int rg = -1;
            if (i >= 0 && i < MAX_FILES && t->fd_table[i] >= 0)
                rg = t->fd_table[i];

            if (readfds && fd_isset_user(i, readfds)) {
                if (rg < 0) PSELECT_RETURN(-EBADF);
                int rev = vfs_poll_events(rg, POLLIN);
                if (rev & (POLLIN | POLLHUP | POLLERR)) ready_count++;
                else if (fd_clear_user(i, readfds) < 0) PSELECT_RETURN(-EFAULT);
            }
            if (writefds && fd_isset_user(i, writefds)) {
                if (rg < 0) PSELECT_RETURN(-EBADF);
                int rev = vfs_poll_events(rg, POLLOUT);
                if (rev & (POLLOUT | POLLERR)) ready_count++;
                else if (fd_clear_user(i, writefds) < 0) PSELECT_RETURN(-EFAULT);
            }
            if (exceptfds && fd_isset_user(i, exceptfds)) {
                if (rg < 0) PSELECT_RETURN(-EBADF);
                if (fd_clear_user(i, exceptfds) < 0) PSELECT_RETURN(-EFAULT);
            }
        }

        if (ready_count > 0) PSELECT_RETURN(ready_count);
        if (has_timeout && timeout_ticks == 0) PSELECT_RETURN(0);
        if (has_timeout && timer_get_ticks() >= deadline) PSELECT_RETURN(0);
        proc_yield();
        if (!has_timeout) PSELECT_RETURN(0);
    }
#undef PSELECT_RETURN
}
