#include "syscall_impl.h"
#include "abi/linux/ioctl.h"
#include "drv/uart.h"
#include "fs/vfs/file.h"
#include "proc/proc_internal.h"

#define LINUX_POLL_WAIT_TICKS MS_TO_TICKS(20)

static uint64_t linux_poll_wait_quantum(void)
{
    uint64_t ticks = LINUX_POLL_WAIT_TICKS;
    return ticks ? ticks : 1;
}

static void linux_poll_sleep_until(uint64_t deadline, int has_deadline)
{
    task_t *t = proc_current();
    if (!t) {
        proc_yield();
        return;
    }

    uint64_t now = timer_get_ticks();
    uint64_t wake = now + linux_poll_wait_quantum();
    if (has_deadline && deadline < wake)
        wake = deadline;
    if (wake <= now) {
        proc_yield();
        return;
    }

    proc_block_until(t, wake);
    sched();
    if (t->state == PROC_BLOCKED)
        t->state = PROC_RUNNING;
    proc_set_wake_time(t, 0);
}

static int linux_poll_apply_sigmask(task_t *t, void *sigmask,
                                    signal_state_t **saved_ss,
                                    uint64_t *saved_blocked)
{
    *saved_ss = NULL;
    *saved_blocked = 0;
    if (!sigmask)
        return 0;
    if (!t || !t->signals)
        return -EINVAL;

    uint64_t user_mask;
    if (copy_from_user(&user_mask, sigmask, sizeof(user_mask)) < 0)
        return -EFAULT;

    *saved_ss = (signal_state_t *)t->signals;
    *saved_blocked = t->sig_blocked;
    t->sig_blocked = signal_mask_from_user(user_mask) &
        ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
    return 0;
}

static void linux_poll_restore_sigmask(task_t *t, signal_state_t *saved_ss,
                                       uint64_t saved_blocked)
{
    if (t && saved_ss)
        t->sig_blocked = saved_blocked;
}

static void linux_poll_defer_sigmask_restore(task_t *t,
                                             signal_state_t *saved_ss,
                                             uint64_t saved_blocked)
{
    if (!t || !saved_ss)
        return;
    t->sigsuspend_old_blocked = saved_blocked;
    t->sigsuspend_active = 1;
}

static int64_t read_into_user(vfile_t *vf, char *buf, size_t count)
{
    if (!vf)
        return -EBADF;
    int64_t total = 0;
    int short_read_ok = vfs_is_pipe_vfile(vf) || vfs_is_char_device_vfile(vf);
    while ((size_t)total < count) {
        void *kaddr;
        size_t chunk;
        if (user_buffer_segment(buf + total, count - (size_t)total, 1, &kaddr, &chunk) < 0)
            return -EFAULT;
        if (chunk > LINUX_IO_CHUNK_SIZE)
            chunk = LINUX_IO_CHUNK_SIZE;
        int64_t n = vfs_read_file(vf, kaddr, chunk);
        if (n <= 0)
            return total > 0 ? total : n;
        total += n;
        if (short_read_ok)
            break;
        if ((size_t)n < chunk)
            break;
    }
    return total;
}

static int64_t write_from_user(vfile_t *vf, const char *buf, size_t count)
{
    if (!vf)
        return -EBADF;
    int64_t total = 0;
    while ((size_t)total < count) {
        void *kaddr;
        size_t chunk;
        if (user_buffer_segment(buf + total, count - (size_t)total, 0, &kaddr, &chunk) < 0)
            return -EFAULT;
        if (chunk > LINUX_IO_CHUNK_SIZE)
            chunk = LINUX_IO_CHUNK_SIZE;
        int64_t n = vfs_write_file(vf, kaddr, chunk);
        if (n <= 0)
            return total > 0 ? total : n;
        total += n;
        if ((size_t)n < chunk)
            break;
    }
    return total;
}

static int o_direct_check(vfile_t *vf, const char *buf, size_t count)
{
    if (!vf || !(vf->flags & O_DIRECT))
        return 0;
    int align = 512;
    if ((uintptr_t)buf & (align - 1) || (count & (align - 1)))
        return -EINVAL;
    if (vf->ops && vf->ops->lseek) {
        long off = vf->ops->lseek(vf, 0, SEEK_CUR);
        if (off >= 0 && (off & (align - 1)))
            return -EINVAL;
    }
    return 0;
}

int64_t sys_read(int fd, char *buf, size_t count) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    int ar = o_direct_check(vf, buf, count);
    if (ar < 0) { vfs_put_file_ref((int)gfd, vf); return ar; }
    int64_t r = read_into_user(vf, buf, count);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}

int64_t sys_write(int fd, const char *buf, size_t count) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    int ar = o_direct_check(vf, buf, count);
    if (ar < 0) { vfs_put_file_ref((int)gfd, vf); return ar; }
    int64_t r = write_from_user(vf, buf, count);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}

int64_t sys_pread64(int fd, char *buf, size_t count, long off) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf_check = vfs_get_file_ref((int)gfd);
    if (vf_check && (vf_check->flags & O_DIRECT)) {
        int align = 512;
        if ((uintptr_t)buf & (align - 1) || (count & (align - 1)) ||
            (off < 0) || ((long)off & (align - 1))) {
            vfs_put_file_ref((int)gfd, vf_check);
            return -EINVAL;
        }
    }
    vfs_put_file_ref((int)gfd, vf_check);
    long curoff = vfs_lseek(gfd, 0, SEEK_CUR);
    if (curoff < 0) return curoff;
    long sr = vfs_lseek(gfd, off, SEEK_SET);
    if (sr < 0) return sr;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    int64_t total = read_into_user(vf, buf, count);
    vfs_put_file_ref((int)gfd, vf);
    vfs_lseek(gfd, curoff, SEEK_SET);
    return total;
}

int64_t sys_pwrite64(int fd, char *buf, size_t count, long off) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf_check = vfs_get_file_ref((int)gfd);
    if (vf_check && (vf_check->flags & O_DIRECT)) {
        int align = 512;
        if ((uintptr_t)buf & (align - 1) || (count & (align - 1)) ||
            (off < 0) || ((long)off & (align - 1))) {
            vfs_put_file_ref((int)gfd, vf_check);
            return -EINVAL;
        }
    }
    vfs_put_file_ref((int)gfd, vf_check);
    long curoff = vfs_lseek(gfd, 0, SEEK_CUR);
    if (curoff < 0) return curoff;
    long sr = vfs_lseek(gfd, off, SEEK_SET);
    if (sr < 0) return sr;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    int64_t total = write_from_user(vf, buf, count);
    vfs_put_file_ref((int)gfd, vf);
    vfs_lseek(gfd, curoff, SEEK_SET);
    return total;
}

int64_t sys_writev(int fd, const void *iov, int iovcnt) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    struct iovec { char *base; size_t len; };
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        struct iovec v;
        if (copy_from_user(&v, (const char *)iov + (size_t)i * sizeof(struct iovec), sizeof(v)) < 0) { total = -EFAULT; break; }
        if (!v.base || v.len == 0) continue;
        int ar = o_direct_check(vf, v.base, v.len);
        if (ar < 0) { total = ar; break; }
        int64_t n = write_from_user(vf, v.base, v.len);
        if (n < 0) { total = n; break; }
        total += n;
        if ((size_t)n < v.len) break;
    }
    vfs_put_file_ref((int)gfd, vf);
    return total;
}

int64_t sys_readv(int fd, const void *iov, int iovcnt) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    struct iovec { char *base; size_t len; };
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        struct iovec v;
        if (copy_from_user(&v, (const char *)iov + (size_t)i * sizeof(struct iovec), sizeof(v)) < 0) { total = -EFAULT; break; }
        if (!v.base || v.len == 0) continue;
        int ar = o_direct_check(vf, v.base, v.len);
        if (ar < 0) { total = ar; break; }
        int64_t n = read_into_user(vf, v.base, v.len);
        if (n <= 0) { total = total > 0 ? total : n; break; }
        total += n;
        if ((size_t)n < v.len) break;
    }
    vfs_put_file_ref((int)gfd, vf);
    return total;
}
int64_t sys_openat(int dirfd, const char *path, int flags, int mode) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    char full[MAX_PATH_LEN];
    int pr = syscall_path_at(dirfd, kpath, full, sizeof(full));
    if (pr < 0) {
        return pr;
    }
    int gfd = vfs_open(full, flags, mode);
    if (gfd < 0) {
        return gfd;
    }
    task_t *t = proc_current();
    return fdtable_install(t, gfd, flags);
}

int64_t sys_close(int fd) {
    return fdtable_close_current(fd);
}

int64_t sys_lseek(int fd, long offset, int whence) {
    int gfd = fdtable_get_current(fd);
    if (gfd < 0) return -EBADF;
    return vfs_lseek(gfd, offset, whence);
}

int64_t sys_dup(int fd) {
    return fdtable_dup(proc_current(), fd, 0, 0);
}

int64_t sys_dup3(int oldfd, int newfd, int flags) {
    return fdtable_dup_to(proc_current(), oldfd, newfd, flags);
}

int64_t sys_fcntl(int fd, int cmd, long arg) {
    /* F_DUPFD=0, F_DUPFD_CLOEXEC=1030: share the same global fd */
    if (cmd == F_DUPFD || cmd == F_DUPFD_CLOEXEC) {
        task_t *t = proc_current();
        if (!t) return -ESRCH;
        int minfd = (int)arg;
        int dflags = (cmd == F_DUPFD_CLOEXEC) ? O_CLOEXEC : 0;
        return fdtable_dup(t, fd, minfd, dflags);
    }
    if (cmd == F_GETFD) {
        return fdtable_get_cloexec(proc_current(), fd);
    }
    if (cmd == F_SETFD) {
        return fdtable_set_cloexec(proc_current(), fd, (arg & FD_CLOEXEC) != 0);
    }
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    int64_t r = vfs_fcntl(gfd, cmd, arg);
    if (r >= 0 && cmd == F_SETFL)
        net_set_nonblock((int)gfd, ((int)arg & O_NONBLOCK) != 0);
    return r;
}

int64_t sys_flock(int fd, int operation) {
    int64_t gfd = fdtable_get_current(fd);
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
            vfile_t *rd = vfs_get_file_ref(gfd[0]);
            vfile_t *wr = vfs_get_file_ref(gfd[1]);
            if (rd) rd->flags |= O_NONBLOCK;
            if (wr) wr->flags |= O_NONBLOCK;
            vfs_put_file_ref(gfd[0], rd);
            vfs_put_file_ref(gfd[1], wr);
        }
        task_t *t = proc_current();
        int fd0 = fdtable_install(t, gfd[0], flags);
        if (fd0 < 0) {
            vfs_close(gfd[1]);
            return fd0;
        }
        int fd1 = fdtable_install(t, gfd[1], flags);
        if (fd1 < 0) {
            fdtable_close(t, fd0);
            return fd1;
        }
        int user_fds[2] = {fd0, fd1};
        if (copy_to_user(pipefd, user_fds, sizeof(user_fds)) < 0) {
            fdtable_close(t, fd0);
            fdtable_close(t, fd1);
            return -EFAULT;
        }
    }
    return r;
}

int64_t sys_ioctl(int fd, unsigned long req, void *arg) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    if (req == TIOCGPGRP) {
        int pgid = uart_get_foreground_pgid();
        return copy_to_user(arg, &pgid, sizeof(pgid)) < 0 ? -EFAULT : 0;
    }
    if (req == TIOCSPGRP) {
        int pgid = 0;
        if (copy_from_user(&pgid, arg, sizeof(pgid)) < 0) return -EFAULT;
        if (pgid <= 0) return -EINVAL;
        uart_set_foreground_pgid(pgid);
        return 0;
    }
    return vfs_ioctl(gfd, req, arg);
}

int64_t sys_sync(void) {
    return vfs_sync();
}

int64_t sys_fsync(int fd) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    return vfs_fsync((int)gfd);
}

int64_t sys_fdatasync(int fd) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    vfs_put_file_ref((int)gfd, vf);
    kstat_t st;
    if (vfs_fstat((int)gfd, &st) < 0)
        return -EINVAL;
    if ((st.st_mode & S_IFMT) != S_IFREG)
        return -EINVAL;
    return vfs_fsync((int)gfd);
}

int64_t sys_sync_file_range(int fd, long offset, long nbytes, unsigned flags) {
    (void)offset;
    (void)nbytes;
    (void)flags;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return -EBADF;
    return vfs_fsync((int)gfd);
}

int64_t sys_ftruncate(int fd, size_t size) {
    int64_t gfd = fdtable_get_current(fd);
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
    int64_t out_gfd = fdtable_get_current(out_fd);
    if (out_gfd < 0) return out_gfd;
    int64_t in_gfd = fdtable_get_current(in_fd);
    if (in_gfd < 0) return in_gfd;
    {
        vfile_t *ovf = vfs_get_file_ref(out_fd);
        if (ovf && !vfs_should_write(ovf->flags)) {
            vfs_put_file_ref(out_fd, ovf);
            return -EBADF;
        }
        vfs_put_file_ref(out_fd, ovf);
    }
    long user_off = 0;
    if (off && copy_from_user(&user_off, off, sizeof(long)) < 0) return -EFAULT;
    if (off && user_off < 0) return -EINVAL;
    long cur_off = off ? user_off : vfs_lseek(in_gfd, 0, SEEK_CUR);
    long saved = off ? vfs_lseek(in_gfd, 0, SEEK_CUR) : 0;
    int64_t total = 0;
    char *sbuf = proc_scratch_buffer(LINUX_IO_CHUNK_SIZE);
    if (!sbuf) return -ENOMEM;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > LINUX_IO_CHUNK_SIZE) chunk = LINUX_IO_CHUNK_SIZE;
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
    return total;
}

int64_t sys_ppoll(void *fds, int nfds, void *tmo, void *sigmask) {
    struct pollfd { int fd; short events; short revents; };
    struct pollfd *pfds = (struct pollfd *)fds;
    if (nfds < 0) return -EINVAL;
    task_t *t = proc_current();
    signal_state_t *saved_ss = NULL;
    uint64_t saved_blocked = 0;
    int mask_ret = linux_poll_apply_sigmask(t, sigmask, &saved_ss, &saved_blocked);
    if (mask_ret < 0)
        return mask_ret;
#define PPOLL_RETURN(v) do { linux_poll_restore_sigmask(t, saved_ss, saved_blocked); return (v); } while (0)
#define PPOLL_SIGNAL_RETURN(v) do { linux_poll_defer_sigmask_restore(t, saved_ss, saved_blocked); return (v); } while (0)
    if (nfds == 0) {
        if (tmo) {
            uint64_t ts[2];
            if (copy_from_user(ts, tmo, sizeof(ts)) < 0) PPOLL_RETURN(-EFAULT);
            if (ts[1] >= 1000000000ULL) PPOLL_RETURN(-EINVAL);
            uint64_t ticks = ts[0] * TICKS_PER_SEC + ts[1] * TICKS_PER_SEC / 1000000000ULL;
            uint64_t until = timer_get_ticks() + (ticks ? ticks : 1);
            while (timer_get_ticks() < until) {
                if (signal_task_has_unblocked(t)) PPOLL_SIGNAL_RETURN(-ERESTARTSYS);
                linux_poll_sleep_until(until, 1);
            }
            if (signal_task_has_unblocked(t)) PPOLL_SIGNAL_RETURN(-ERESTARTSYS);
            PPOLL_RETURN(0);
        }
        for (;;) {
            if (signal_task_has_unblocked(t)) PPOLL_SIGNAL_RETURN(-ERESTARTSYS);
            linux_poll_sleep_until(0, 0);
        }
    }
    if (!pfds) PPOLL_RETURN(-EFAULT);

    uint64_t timeout_ticks = 0;
    int has_timeout = tmo != NULL;
    if (tmo) {
        uint64_t ts[2];
        if (copy_from_user(ts, tmo, sizeof(ts)) < 0) PPOLL_RETURN(-EFAULT);
        if (ts[1] >= 1000000000ULL) PPOLL_RETURN(-EINVAL);
        timeout_ticks = ts[0] * TICKS_PER_SEC + ts[1] * TICKS_PER_SEC / 1000000000ULL;
    }
    uint64_t deadline = timer_get_ticks() + (timeout_ticks ? timeout_ticks : 1);

    for (;;) {
        int ready = 0;
        for (int i = 0; i < nfds; i++) {
            struct pollfd pfd;
            if (copy_from_user(&pfd, &pfds[i], sizeof(pfd)) < 0) PPOLL_RETURN(-EFAULT);
            pfd.revents = 0;
            if (pfd.fd >= 0) {
                int64_t gfd = fdtable_get_current(pfd.fd);
                pfd.revents = gfd < 0 ? POLLNVAL : (short)vfs_poll_events((int)gfd, pfd.events);
                if (pfd.revents) ready++;
            }
            if (copy_to_user(&pfds[i].revents, &pfd.revents, sizeof(short)) < 0)
                PPOLL_RETURN(-EFAULT);
        }
        if (ready > 0) PPOLL_RETURN(ready);
        if (has_timeout && timeout_ticks == 0) PPOLL_RETURN(0);
        if (has_timeout && timer_get_ticks() >= deadline) PPOLL_RETURN(0);
        if (signal_task_has_unblocked(t)) PPOLL_SIGNAL_RETURN(-EINTR);
        linux_poll_sleep_until(deadline, has_timeout);
    }
#undef PPOLL_RETURN
#undef PPOLL_SIGNAL_RETURN
}

int64_t sys_poll(void *fds, int nfds, int timeout) {
    struct pollfd { int fd; short events; short revents; };
    struct pollfd *pfds = (struct pollfd *)fds;
    task_t *t = proc_current();
    if (nfds < 0) return -EINVAL;
    if (nfds == 0) {
        if (timeout == 0)
            return 0;
        if (timeout > 0) {
            uint64_t until = timer_get_ticks() + MS_TO_TICKS((uint64_t)timeout);
            while (timer_get_ticks() < until) {
                if (signal_task_has_unblocked(t)) return -ERESTARTSYS;
                linux_poll_sleep_until(until, 1);
            }
            return signal_task_has_unblocked(t) ? -ERESTARTSYS : 0;
        }
        for (;;) {
            if (signal_task_has_unblocked(t)) return -ERESTARTSYS;
            linux_poll_sleep_until(0, 0);
        }
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
                int64_t gfd = fdtable_get_current(pfd.fd);
                pfd.revents = gfd < 0 ? POLLNVAL : (short)vfs_poll_events((int)gfd, pfd.events);
                if (pfd.revents) ready++;
            }
            if (copy_to_user(&pfds[i].revents, &pfd.revents, sizeof(short)) < 0)
                return -EFAULT;
        }
        if (ready > 0) return ready;
        if (has_timeout && timeout == 0) return 0;
        if (has_timeout && timer_get_ticks() >= deadline) return 0;
        if (signal_task_has_unblocked(t)) return -ERESTARTSYS;
        linux_poll_sleep_until(deadline, has_timeout);
    }
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

static int select_filter_user(task_t *t, int nfds, void *readfds,
                              void *writefds, void *exceptfds)
{
    for (int i = 0; i < nfds; i++) {
        int rg = fdtable_get(t, i);

        if (readfds && fd_isset_user(i, readfds)) {
            int keep = 0;
            if (rg >= 0) {
                int rev = vfs_poll_events(rg, POLLIN);
                keep = (rev & (POLLIN | POLLHUP | POLLERR)) != 0;
            }
            if (!keep && fd_clear_user(i, readfds) < 0)
                return -EFAULT;
        }
        if (writefds && fd_isset_user(i, writefds)) {
            int keep = 0;
            if (rg >= 0) {
                int rev = vfs_poll_events(rg, POLLOUT);
                keep = (rev & (POLLOUT | POLLERR)) != 0;
            }
            if (!keep && fd_clear_user(i, writefds) < 0)
                return -EFAULT;
        }
        if (exceptfds && fd_isset_user(i, exceptfds)) {
            if (fd_clear_user(i, exceptfds) < 0)
                return -EFAULT;
        }
    }
    return 0;
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
            int rg = fdtable_get(t, i);

            if (readfds && fd_isset_user(i, readfds)) {
                if (rg < 0) return -EBADF;
                int rev = vfs_poll_events(rg, POLLIN);
                if (rev & (POLLIN | POLLHUP | POLLERR)) ready_count++;
            }
            if (writefds && fd_isset_user(i, writefds)) {
                if (rg < 0) return -EBADF;
                int rev = vfs_poll_events(rg, POLLOUT);
                if (rev & (POLLOUT | POLLERR)) ready_count++;
            }
            if (exceptfds && fd_isset_user(i, exceptfds)) {
                if (rg < 0) return -EBADF;
            }
        }

        if (ready_count > 0) {
            int fr = select_filter_user(t, nfds, readfds, writefds, exceptfds);
            return fr < 0 ? fr : ready_count;
        }
        if (has_timeout && timeout_ticks == 0) {
            int fr = select_filter_user(t, nfds, readfds, writefds, exceptfds);
            return fr < 0 ? fr : 0;
        }
        if (has_timeout && timer_get_ticks() >= deadline) {
            int fr = select_filter_user(t, nfds, readfds, writefds, exceptfds);
            return fr < 0 ? fr : 0;
        }
        if (signal_task_has_unblocked(t))
            return -ERESTARTSYS;
        linux_poll_sleep_until(deadline, has_timeout);
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
            saved_blocked = t->sig_blocked;
            t->sig_blocked = signal_mask_from_user(user_mask) &
                ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
        }
    }
#define PSELECT_RETURN(v) do { if (saved_ss) t->sig_blocked = saved_blocked; return (v); } while (0)
#define PSELECT_SIGNAL_RETURN(v) do { \
    if (saved_ss) { \
        t->sigsuspend_old_blocked = saved_blocked; \
        t->sigsuspend_active = 1; \
    } \
    return (v); \
} while (0)
    for (;;) {
        int ready_count = 0;

        for (int i = 0; i < nfds; i++) {
            int rg = fdtable_get(t, i);

            if (readfds && fd_isset_user(i, readfds)) {
                if (rg < 0) PSELECT_RETURN(-EBADF);
                int rev = vfs_poll_events(rg, POLLIN);
                if (rev & (POLLIN | POLLHUP | POLLERR)) ready_count++;
            }
            if (writefds && fd_isset_user(i, writefds)) {
                if (rg < 0) PSELECT_RETURN(-EBADF);
                int rev = vfs_poll_events(rg, POLLOUT);
                if (rev & (POLLOUT | POLLERR)) ready_count++;
            }
            if (exceptfds && fd_isset_user(i, exceptfds)) {
                if (rg < 0) PSELECT_RETURN(-EBADF);
            }
        }

        if (ready_count > 0) {
            int fr = select_filter_user(t, nfds, readfds, writefds, exceptfds);
            PSELECT_RETURN(fr < 0 ? fr : ready_count);
        }
        if (has_timeout && timeout_ticks == 0) {
            int fr = select_filter_user(t, nfds, readfds, writefds, exceptfds);
            PSELECT_RETURN(fr < 0 ? fr : 0);
        }
        if (has_timeout && timer_get_ticks() >= deadline) {
            int fr = select_filter_user(t, nfds, readfds, writefds, exceptfds);
            PSELECT_RETURN(fr < 0 ? fr : 0);
        }
        if (signal_task_has_unblocked(t))
            PSELECT_SIGNAL_RETURN(-EINTR);
        linux_poll_sleep_until(deadline, has_timeout);
    }
#undef PSELECT_RETURN
#undef PSELECT_SIGNAL_RETURN
}
