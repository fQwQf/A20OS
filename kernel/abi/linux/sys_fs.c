#include "syscall_impl.h"
#include "abi/linux/ioctl.h"
#include "core/sync.h"
#include "drivers/char/uart.h"
#include "fs/pipe.h"
#include "fs/readiness.h"
#include "fs/vfs/file.h"
#include "mm/slab.h"
#include "proc/proc_internal.h"

#define LINUX_POLL_WAIT_TICKS (MS_TO_TICKS(1) ? MS_TO_TICKS(1) : 1)
#define LINUX_POLL_ACTIVE_YIELDS 2

static int linux_vfile_uses_shared_offset(vfile_t *vf);

static uint64_t linux_poll_wait_quantum(void)
{
    uint64_t ticks = LINUX_POLL_WAIT_TICKS;
    return ticks ? ticks : 1;
}

typedef struct {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
} linux_winsize_t;

static proc_wake_reason_t
linux_poll_sleep_until(uint64_t deadline, int has_deadline, int yield_only)
{
    task_t *t = proc_current();
    if (!t) {
        proc_yield();
        return PROC_WAKE_CANCEL;
    }

    if (yield_only) {
        proc_yield();
        return PROC_WAKE_EVENT;
    }

    uint64_t now = timer_get_ticks();
    uint64_t wake = now + linux_poll_wait_quantum();
    if (has_deadline && deadline < wake)
        wake = deadline;
    if (wake <= now) {
        proc_yield();
        return PROC_WAKE_TIMEOUT;
    }

    return proc_park_wait(PROC_WAIT_INTERRUPTIBLE, wake);
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
    return signal_task_set_temporary_mask(
        t, signal_mask_from_user(user_mask), saved_blocked);
}

static void linux_poll_restore_sigmask(task_t *t, signal_state_t *saved_ss,
                                       uint64_t saved_blocked)
{
    if (t && saved_ss)
        signal_task_restore_mask(t, saved_blocked);
}

static void linux_poll_defer_sigmask_restore(task_t *t,
                                             signal_state_t *saved_ss,
                                             uint64_t saved_blocked)
{
    if (!t || !saved_ss)
        return;
    signal_task_defer_mask_restore(t, saved_blocked);
}

struct linux_pollfd {
    int fd;
    short events;
    short revents;
};

static int linux_poll_wait_fds(void *fds, int nfds,
                               uint64_t deadline, bool has_deadline)
{
    if (nfds < 0 || nfds > MAX_FILES)
        return -EINVAL;
    if (!fds && nfds)
        return -EFAULT;

    struct linux_pollfd *user = fds;
    readiness_interest_t *items = nfds ?
        kcalloc((size_t)nfds, sizeof(*items)) : NULL;
    if (nfds && !items)
        return -ENOMEM;

    for (int i = 0; i < nfds; i++) {
        struct linux_pollfd pfd;
        if (copy_from_user(&pfd, &user[i], sizeof(pfd)) < 0) {
            kfree(items);
            return -EFAULT;
        }
        items[i].fd = pfd.fd;
        items[i].events = pfd.events;
    }

    int result;
    do {
        result = readiness_wait_once(items, (size_t)nfds, NULL, 0, 0,
                                     deadline, has_deadline);
    } while (result == READINESS_RETRY);

    if (result >= 0) {
        for (int i = 0; i < nfds; i++) {
            if (copy_to_user(&user[i].revents, &items[i].revents,
                             sizeof(items[i].revents)) < 0) {
                result = -EFAULT;
                break;
            }
        }
    }
    kfree(items);
    return result;
}

static int64_t read_into_user(vfile_t *vf, char *buf, size_t count, int locked)
{
    if (!vf)
        return -EBADF;

    /*
     * Read through a kernel buffer and copy to user afterwards, releasing the
     * shared-offset lock between the two.  copy_to_user() can fault in a
     * missing page, and if that page backs a mapping of the same file the
     * fault handler may re-enter the file read and take vf->offset_lock again
     * - a self-deadlock (LOCK-STALL owner == waiter in sys_read).  The lock
     * therefore never covers a user access.  `locked` is set when the caller
     * (pread64) already holds offset_lock for a fixed-offset read.
     */
    int lock_offset = !locked && linux_vfile_uses_shared_offset(vf);
    int64_t total = 0;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > LINUX_IO_CHUNK_SIZE)
            chunk = LINUX_IO_CHUNK_SIZE;
        char *kbuf = proc_scratch_buffer(chunk);
        if (!kbuf)
            return total > 0 ? total : -ENOMEM;
        if (lock_offset)
            mutex_lock(&vf->offset_lock);
        int64_t n = vfs_read_file(vf, kbuf, chunk);
        if (lock_offset)
            mutex_unlock(&vf->offset_lock);
        if (n <= 0)
            return total > 0 ? total : n;
        if (copy_to_user(buf + total, kbuf, (size_t)n) < 0)
            return total > 0 ? total : -EFAULT;
        total += n;
        if ((size_t)n < chunk)
            break;
    }
    return total;
}

static int64_t write_from_user(vfile_t *vf, const char *buf, size_t count,
                               int locked)
{
    if (!vf)
        return -EBADF;
    if (vfs_is_pipe_vfile(vf) && count > 0) {
        size_t scratch_size = count;
        if (scratch_size > LINUX_IO_CHUNK_SIZE + PIPE_BUF_SIZE)
            scratch_size = LINUX_IO_CHUNK_SIZE + PIPE_BUF_SIZE;
        char *kbuf = proc_scratch_buffer(scratch_size);
        if (!kbuf)
            return -ENOMEM;

        int64_t total = 0;
        while ((size_t)total < count) {
            size_t remaining = count - (size_t)total;
            size_t chunk = remaining;
            if (chunk > LINUX_IO_CHUNK_SIZE)
                chunk = LINUX_IO_CHUNK_SIZE;
            /* Keep a small tail in the same non-atomic pipe write. */
            if (remaining > chunk && remaining - chunk <= PIPE_BUF_SIZE)
                chunk = remaining;
            if (copy_from_user(kbuf, buf + total, chunk) < 0)
                return -EFAULT;
            int64_t n = vfs_write_file(vf, kbuf, chunk);
            if (n <= 0)
                return total > 0 ? total : n;
            total += n;
            if ((size_t)n < chunk)
                break;
        }
        return total;
    }

    int lock_offset = !locked && linux_vfile_uses_shared_offset(vf);
    int64_t total = 0;
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > LINUX_IO_CHUNK_SIZE)
            chunk = LINUX_IO_CHUNK_SIZE;
        char *kbuf = proc_scratch_buffer(chunk);
        if (!kbuf)
            return total > 0 ? total : -ENOMEM;
        if (copy_from_user(kbuf, buf + total, chunk) < 0)
            return total > 0 ? total : -EFAULT;
        if (lock_offset)
            mutex_lock(&vf->offset_lock);
        int64_t n = vfs_write_file(vf, kbuf, chunk);
        if (lock_offset)
            mutex_unlock(&vf->offset_lock);
        if (n <= 0)
            return total > 0 ? total : n;
        total += n;
        if ((size_t)n < chunk)
            break;
    }
    return total;
}

static int linux_vfile_uses_shared_offset(vfile_t *vf)
{
    if (!vf || !vf->vnode || !vf->ops || !vf->ops->lseek)
        return 0;
    /* procfs files are excluded: rendering /proc/<pid>/fdinfo locks the
     * target vfile's offset_lock, and taking vf->offset_lock here first
     * could recurse on the same lock through that path. */
    if (vfs_is_procfs_vfile(vf))
        return 0;
    return 1;
}

static int o_direct_check(vfile_t *vf, const char *buf, size_t count)
{
    if (!vf || !(vf->flags & O_DIRECT))
        return 0;
    if (vf->vnode) {
        uint32_t mode = vf->vnode->mode;
        if (!((mode & S_IFMT) == S_IFREG || (mode & S_IFMT) == S_IFBLK)) {
            return 0;
        }
    }
    int align = 512;
    if ((uintptr_t)buf & (align - 1) || (count & (align - 1)))
        return -EINVAL;
    if (vf->offset & (align - 1))
        return -EINVAL;
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
    int64_t r = read_into_user(vf, buf, count, 0);
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
    int64_t r = write_from_user(vf, buf, count, 0);
    vfs_put_file_ref((int)gfd, vf);
    return r;
}

int64_t sys_pread64(int fd, char *buf, size_t count, long off) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    if (off < 0) return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->ops || !vf->ops->lseek) {
        vfs_put_file_ref((int)gfd, vf);
        return -ESPIPE;
    }
    if (vf && (vf->flags & O_DIRECT)) {
        int is_reg_or_blk = 1;
        if (vf->vnode) {
            uint32_t mode = vf->vnode->mode;
            if (!((mode & S_IFMT) == S_IFREG || (mode & S_IFMT) == S_IFBLK)) {
                is_reg_or_blk = 0;
            }
        }
        if (is_reg_or_blk) {
            int align = 512;
            if ((uintptr_t)buf & (align - 1) || (count & (align - 1)) ||
                (off < 0) || ((long)off & (align - 1))) {
                vfs_put_file_ref((int)gfd, vf);
                return -EINVAL;
            }
        }
    }

    mutex_lock(&vf->offset_lock);
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    if (saved < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref((int)gfd, vf);
        return saved;
    }
    long seek_r = vf->ops->lseek(vf, off, SEEK_SET);
    if (seek_r < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref((int)gfd, vf);
        return seek_r;
    }
    int64_t total = read_into_user(vf, buf, count, 1);
    long restore_r = vf->ops->lseek(vf, saved, SEEK_SET);
    mutex_unlock(&vf->offset_lock);
    vfs_put_file_ref((int)gfd, vf);
    if (restore_r < 0 && total >= 0)
        return restore_r;
    return total;
}

int64_t sys_pwrite64(int fd, char *buf, size_t count, long off) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    if (off < 0) return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    if (!vf->ops || !vf->ops->lseek) {
        vfs_put_file_ref((int)gfd, vf);
        return -ESPIPE;
    }
    if (vf && (vf->flags & O_DIRECT)) {
        int is_reg_or_blk = 1;
        if (vf->vnode) {
            uint32_t mode = vf->vnode->mode;
            if (!((mode & S_IFMT) == S_IFREG || (mode & S_IFMT) == S_IFBLK)) {
                is_reg_or_blk = 0;
            }
        }
        if (is_reg_or_blk) {
            int align = 512;
            if ((uintptr_t)buf & (align - 1) || (count & (align - 1)) ||
                (off < 0) || ((long)off & (align - 1))) {
                vfs_put_file_ref((int)gfd, vf);
                return -EINVAL;
            }
        }
    }

    mutex_lock(&vf->offset_lock);
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    if (saved < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref((int)gfd, vf);
        return saved;
    }
    long seek_r = vf->ops->lseek(vf, off, SEEK_SET);
    if (seek_r < 0) {
        mutex_unlock(&vf->offset_lock);
        vfs_put_file_ref((int)gfd, vf);
        return seek_r;
    }
    int saved_flags = vf->flags;
    vf->flags &= ~O_APPEND;
    int64_t total = write_from_user(vf, buf, count, 1);
    vf->flags = saved_flags;
    long restore_r = vf->ops->lseek(vf, saved, SEEK_SET);
    mutex_unlock(&vf->offset_lock);
    vfs_put_file_ref((int)gfd, vf);
    if (restore_r < 0 && total >= 0)
        return restore_r;
    return total;
}

int64_t sys_writev(int fd, const void *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    struct iovec { char *base; size_t len; };
    int64_t total = 0;
    int lock_offset = linux_vfile_uses_shared_offset(vf);
    if (lock_offset)
        mutex_lock(&vf->offset_lock);
    for (int i = 0; i < iovcnt; i++) {
        struct iovec v;
        if (copy_from_user(&v, (const char *)iov + (size_t)i * sizeof(struct iovec), sizeof(v)) < 0) { total = -EFAULT; break; }
        if (!v.base || v.len == 0) continue;
        int ar = o_direct_check(vf, v.base, v.len);
        if (ar < 0) { total = ar; break; }
        int64_t n = write_from_user(vf, v.base, v.len, 0);
        if (n < 0) { total = n; break; }
        total += n;
        if ((size_t)n < v.len) break;
    }
    if (lock_offset)
        mutex_unlock(&vf->offset_lock);
    vfs_put_file_ref((int)gfd, vf);
    return total;
}

int64_t sys_readv(int fd, const void *iov, int iovcnt) {
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    struct iovec { char *base; size_t len; };
    int64_t total = 0;
    int lock_offset = linux_vfile_uses_shared_offset(vf);
    if (lock_offset)
        mutex_lock(&vf->offset_lock);
    for (int i = 0; i < iovcnt; i++) {
        struct iovec v;
        if (copy_from_user(&v, (const char *)iov + (size_t)i * sizeof(struct iovec), sizeof(v)) < 0) { total = -EFAULT; break; }
        if (!v.base || v.len == 0) continue;
        int ar = o_direct_check(vf, v.base, v.len);
        if (ar < 0) { total = ar; break; }
        int64_t n = read_into_user(vf, v.base, v.len, 0);
        if (n <= 0) { total = total > 0 ? total : n; break; }
        total += n;
        if ((size_t)n < v.len) break;
    }
    if (lock_offset)
        mutex_unlock(&vf->offset_lock);
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
    int64_t r = fdtable_close_current(fd);
    ktrace_syscall("[SYS] close: fd=%d ret=%ld\n", fd, (long)r);
    return r;
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
    req &= 0xffffffffUL;

    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    if (req == TIOCGPGRP || req == PPC64_TIOCGPGRP) {
        int pgid = uart_get_foreground_pgid();
        return copy_to_user(arg, &pgid, sizeof(pgid)) < 0 ? -EFAULT : 0;
    }
    if (req == TIOCSPGRP || req == PPC64_TIOCSPGRP) {
        int pgid = 0;
        if (copy_from_user(&pgid, arg, sizeof(pgid)) < 0) return -EFAULT;
        if (pgid <= 0) return -EINVAL;
        uart_set_foreground_pgid(pgid);
        return 0;
    }
    if (req == TIOCGWINSZ || req == PPC64_TIOCGWINSZ) {
        int r = vfs_ioctl((int)gfd, req, arg);
        if (r != -ENOTTY)
            return r;

        /* Child tools such as rust-lld inherit pipes, not the tty itself. */
        linux_winsize_t ws = { .ws_row = 24, .ws_col = 80 };
        return copy_to_user(arg, &ws, sizeof(ws)) < 0 ? -EFAULT : 0;
    }
    if (req == TIOCGPTN) {
        int pty_number = 0;
        return copy_to_user(arg, &pty_number, sizeof(pty_number)) < 0 ?
            -EFAULT : 0;
    }
    if (req == TIOCSPTLCK || req == TIOCSCTTY)
        return 0;
    if (req == FIONBIO || req == PPC64_FIONBIO)
        return 0;
    if (req == FIONREAD || req == PPC64_FIONREAD) {
        vfile_t *vf = vfs_get_file_ref((int)gfd);
        if (!vf)
            return -EBADF;
        if (pipe_vfile_is(vf)) {
            int available = pipe_get_available(vf);
            vfs_put_file_ref((int)gfd, vf);
            if (available < 0)
                return available;
            return copy_to_user(arg, &available, sizeof(available)) < 0 ?
                -EFAULT : 0;
        }
        vfs_put_file_ref((int)gfd, vf);
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
    return vfs_fdatasync((int)gfd);
}

int64_t sys_sync_file_range(int fd, long offset, long nbytes, unsigned flags) {
    (void)offset;
    (void)nbytes;
    (void)flags;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return -EBADF;
    return vfs_fsync((int)gfd);
}

int64_t sys_syncfs(int fd) {
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return -EBADF;
    return vfs_fsync((int)gfd);
}

int64_t sys_ftruncate(int fd, long length) {
    if (length < 0) return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    return vfs_ftruncate(gfd, (size_t)length);
}

int64_t sys_truncate(const char *path, long length) {
    if (length < 0) return -EINVAL;
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_truncate(kpath, (size_t)length);
}

int64_t sys_sendfile(int out_fd, int in_fd, long *off, size_t count) {
    if (count == 0) return 0;
    int64_t out_gfd = fdtable_get_current(out_fd);
    if (out_gfd < 0) return out_gfd;
    int64_t in_gfd = fdtable_get_current(in_fd);
    if (in_gfd < 0) return in_gfd;
    {
        vfile_t *ovf = vfs_get_file_ref((int)out_gfd);
        if (ovf && !vfs_should_write(ovf->flags)) {
            vfs_put_file_ref((int)out_gfd, ovf);
            return -EBADF;
        }
        vfs_put_file_ref((int)out_gfd, ovf);
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
            if ((ts[0] || ts[1]) && ticks == 0)
                ticks = 1;
            uint64_t until = timer_get_ticks() + (ticks ? ticks : 1);
            while (timer_get_ticks() < until) {
                if (signal_task_has_unblocked(t)) PPOLL_SIGNAL_RETURN(-ERESTARTSYS);
                if (linux_poll_sleep_until(until, 1, 0) ==
                    PROC_WAKE_TIMEOUT_CAPACITY)
                    PPOLL_RETURN(-EAGAIN);
            }
            if (signal_task_has_unblocked(t)) PPOLL_SIGNAL_RETURN(-ERESTARTSYS);
            PPOLL_RETURN(0);
        }
        for (;;) {
            if (signal_task_has_unblocked(t)) PPOLL_SIGNAL_RETURN(-ERESTARTSYS);
            (void)linux_poll_sleep_until(0, 0, 0);
        }
    }
    if (!fds) PPOLL_RETURN(-EFAULT);

    uint64_t timeout_ticks = 0;
    int has_timeout = tmo != NULL;
    if (tmo) {
        uint64_t ts[2];
        if (copy_from_user(ts, tmo, sizeof(ts)) < 0) PPOLL_RETURN(-EFAULT);
        if (ts[1] >= 1000000000ULL) PPOLL_RETURN(-EINVAL);
        timeout_ticks = ts[0] * TICKS_PER_SEC + ts[1] * TICKS_PER_SEC / 1000000000ULL;
        if ((ts[0] || ts[1]) && timeout_ticks == 0)
            timeout_ticks = 1;
    }
    uint64_t deadline = timer_get_ticks() + timeout_ticks;
    int result = linux_poll_wait_fds(fds, nfds, deadline, has_timeout);
    if (result == -EINTR)
        PPOLL_SIGNAL_RETURN(-EINTR);
    PPOLL_RETURN(result);
#undef PPOLL_RETURN
#undef PPOLL_SIGNAL_RETURN
}

int64_t sys_poll(void *fds, int nfds, int timeout) {
    task_t *t = proc_current();
    if (nfds < 0) return -EINVAL;
    if (nfds == 0) {
        if (timeout == 0)
            return 0;
        if (timeout > 0) {
            uint64_t ticks = MS_TO_TICKS((uint64_t)timeout);
            if (ticks == 0)
                ticks = 1;
            uint64_t until = timer_get_ticks() + ticks;
            while (timer_get_ticks() < until) {
                if (signal_task_has_unblocked(t)) return -ERESTARTSYS;
                if (linux_poll_sleep_until(until, 1, 0) ==
                    PROC_WAKE_TIMEOUT_CAPACITY)
                    return -EAGAIN;
            }
            return signal_task_has_unblocked(t) ? -ERESTARTSYS : 0;
        }
        for (;;) {
            if (signal_task_has_unblocked(t)) return -ERESTARTSYS;
            (void)linux_poll_sleep_until(0, 0, 0);
        }
    }
    if (!fds) return -EFAULT;

    int has_timeout = timeout >= 0;
    uint64_t timeout_ticks = 0;
    if (timeout > 0) {
        timeout_ticks = MS_TO_TICKS((uint64_t)timeout);
        if (timeout_ticks == 0)
            timeout_ticks = 1;
    }
    uint64_t deadline = timer_get_ticks() + timeout_ticks;
    int result = linux_poll_wait_fds(fds, nfds, deadline, has_timeout);
    return result == -EINTR ? -ERESTARTSYS : result;
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

#define SELECT_INTEREST_READ   1u
#define SELECT_INTEREST_WRITE  2u
#define SELECT_INTEREST_EXCEPT 3u

static int linux_select_wait(task_t *task, int nfds, void *readfds,
                             void *writefds, void *exceptfds,
                             uint64_t deadline, bool has_deadline)
{
    if (!task || nfds < 0 || nfds > MAX_FILES)
        return nfds < 0 ? -EINVAL : -EBADF;
    size_t capacity = (size_t)nfds * 3;
    readiness_interest_t *items = capacity ?
        kcalloc(capacity, sizeof(*items)) : NULL;
    if (capacity && !items)
        return -ENOMEM;

    size_t count = 0;
    for (int fd = 0; fd < nfds; fd++) {
        bool want_read = readfds && fd_isset_user(fd, readfds);
        bool want_write = writefds && fd_isset_user(fd, writefds);
        bool want_except = exceptfds && fd_isset_user(fd, exceptfds);
        if (!(want_read || want_write || want_except))
            continue;
        if (fdtable_get(task, fd) < 0) {
            kfree(items);
            return -EBADF;
        }
        if (want_read)
            items[count++] = (readiness_interest_t){
                .fd = fd, .events = POLLIN, .cookie = SELECT_INTEREST_READ
            };
        if (want_write)
            items[count++] = (readiness_interest_t){
                .fd = fd, .events = POLLOUT, .cookie = SELECT_INTEREST_WRITE
            };
        if (want_except)
            items[count++] = (readiness_interest_t){
                .fd = fd, .events = POLLPRI, .cookie = SELECT_INTEREST_EXCEPT
            };
    }

    int result;
    do {
        result = readiness_wait_once(items, count, NULL, 0, 0,
                                     deadline, has_deadline);
    } while (result == READINESS_RETRY);
    if (result < 0) {
        kfree(items);
        return result;
    }

    int ready = 0;
    for (size_t i = 0; i < count; i++) {
        readiness_interest_t *item = &items[i];
        if (item->revents & POLLNVAL) {
            kfree(items);
            return -EBADF;
        }
        void *set;
        short mask;
        if (item->cookie == SELECT_INTEREST_READ) {
            set = readfds;
            mask = POLLIN | POLLHUP | POLLERR;
        } else if (item->cookie == SELECT_INTEREST_WRITE) {
            set = writefds;
            mask = POLLOUT | POLLHUP | POLLERR;
        } else {
            set = exceptfds;
            mask = POLLPRI | POLLHUP | POLLERR;
        }
        if (item->revents & mask)
            ready++;
        else if (fd_clear_user(item->fd, set) < 0) {
            kfree(items);
            return -EFAULT;
        }
    }
    kfree(items);
    return ready;
}

int64_t sys_select(int nfds, void *readfds, void *writefds,
                   void *exceptfds, void *timeout) {
    task_t *t = proc_current();
    if (!t) return -ESRCH;

    if (nfds < 0) return -EINVAL;
    uint64_t timeout_ticks = 0;
    int has_timeout = timeout != NULL;
    if (timeout) {
        uint64_t tv[2];
        if (copy_from_user(tv, timeout, sizeof(tv)) < 0) return -EFAULT;
        if (tv[1] >= 1000000ULL) return -EINVAL;
        timeout_ticks = tv[0] * TICKS_PER_SEC + tv[1] * TICKS_PER_SEC / 1000000ULL;
        if ((tv[0] || tv[1]) && timeout_ticks == 0)
            timeout_ticks = 1;
    }
    uint64_t deadline = timer_get_ticks() + timeout_ticks;
    int result = linux_select_wait(t, nfds, readfds, writefds, exceptfds,
                                   deadline, has_timeout);
    return result == -EINTR ? -ERESTARTSYS : result;
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
        if ((ts[0] || ts[1]) && timeout_ticks == 0)
            timeout_ticks = 1;
    }
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
            int mask_ret = signal_task_set_temporary_mask(
                t, signal_mask_from_user(user_mask), &saved_blocked);
            if (mask_ret < 0)
                return mask_ret;
        }
    }
#define PSELECT_RETURN(v) do { \
    if (saved_ss) signal_task_restore_mask(t, saved_blocked); \
    return (v); \
} while (0)
#define PSELECT_SIGNAL_RETURN(v) do { \
    if (saved_ss) signal_task_defer_mask_restore(t, saved_blocked); \
    return (v); \
} while (0)
    uint64_t deadline = timer_get_ticks() + timeout_ticks;
    int result = linux_select_wait(t, nfds, readfds, writefds, exceptfds,
                                   deadline, has_timeout);
    if (result == -EINTR)
        PSELECT_SIGNAL_RETURN(-EINTR);
    PSELECT_RETURN(result);
#undef PSELECT_RETURN
#undef PSELECT_SIGNAL_RETURN
}
