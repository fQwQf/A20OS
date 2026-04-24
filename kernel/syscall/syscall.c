/*
 * A20OS — Complete Syscall Dispatcher
 *
 * Implements the full Linux-compatible RISC-V syscall ABI.
 */
#include "syscall.h"
#include "vfs.h"
#include "fs.h"
#include "proc.h"
#include "signal.h"
#include "mm.h"
#include "vm.h"
#include "timer.h"
#include "uart.h"
#include "stdio.h"
#include "string.h"
#include "consts.h"
#include "defs.h"
#include "klog.h"
#include "sbi.h"

syscall_prof_t sys_prof[SYSCALL_PROFILE_MAX];

static inline uint64_t syscall_profile_now(void) {
    return timer_get_ticks();
}

static inline void syscall_profile_record(uint64_t num, uint64_t start, uint64_t end) {
    if (num >= SYSCALL_PROFILE_MAX) return;

    uint64_t elapsed = (end >= start) ? (end - start) : 0;
    syscall_prof_t *prof = &sys_prof[num];
    __atomic_fetch_add(&prof->count, 1, __ATOMIC_RELAXED);
    __atomic_fetch_add(&prof->cycles, elapsed, __ATOMIC_RELAXED);
}

void syscall_profile_reset(void) {
    memset(sys_prof, 0, sizeof(sys_prof));
}

void syscall_init(void) {
    syscall_profile_reset();
    kdebug("[SYSCALL] Initialized\n");
}

static inline int64_t get_global_fd(int fd) {
    task_t *t = proc_current();
    if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
    int gfd = t->fd_table[fd];
    if (gfd < 0) return -EBADF;
    return gfd;
}

/* Allocate a local fd slot and store the global fd.
 * Returns local fd on success, or -EMFILE if the per-process table is full.
 * On failure, the global fd is closed. */
static int alloc_local_fd(task_t *t, int gfd) {
    if (!t || gfd < 0) return -EBADF;
    for (int i = 0; i < MAX_FILES; i++) {
        if (t->fd_table[i] < 0) {
            t->fd_table[i] = gfd;
            return i;
        }
    }
    vfs_close(gfd);
    return -EMFILE;
}

int64_t syscall_dispatch(trap_context_t *ctx) {
    uint64_t num = ctx->x[17];
    uint64_t start_time = syscall_profile_now();
    uint64_t a0  = ctx->x[10];
    uint64_t a1  = ctx->x[11];
    uint64_t a2  = ctx->x[12];
    uint64_t a3  = ctx->x[13];
    uint64_t a4  = ctx->x[14];
    uint64_t a5  = ctx->x[15];
    (void)a4; (void)a5;

    int64_t ret = -ENOSYS;

    switch (num) {
    case SYS_read:        ret = sys_read((int)a0, (char*)a1, (size_t)a2); break;
    case SYS_write:       ret = sys_write((int)a0, (const char*)a1, (size_t)a2); break;
    case SYS_writev:      ret = sys_writev((int)a0, (const void*)a1, (int)a2); break;
    case SYS_readv:       ret = sys_readv((int)a0, (const void*)a1, (int)a2); break;
    case SYS_openat:      ret = sys_openat((int)a0, (const char*)a1, (int)a2, (int)a3); break;
    case SYS_close:       ret = sys_close((int)a0); break;
    case SYS_lseek:       ret = sys_lseek((int)a0, (long)a1, (int)a2); break;
    case SYS_dup:         ret = sys_dup((int)a0); break;
    case SYS_dup3:        ret = sys_dup3((int)a0, (int)a1, (int)a2); break;
    case SYS_fcntl:       ret = sys_fcntl((int)a0, (int)a1, (long)a2); break;
    case SYS_pipe2:       ret = sys_pipe2((int*)a0, (int)a1); break;
    case SYS_ioctl:       ret = sys_ioctl((int)a0, (unsigned long)a1, (void*)a2); break;
    case SYS_pread64:     ret = sys_pread64((int)a0, (char*)a1, (size_t)a2, (long)a3); break;
    case SYS_pwrite64:    ret = sys_pwrite64((int)a0, (char*)a1, (size_t)a2, (long)a3); break;
    case SYS_sync:        ret = sys_sync(); break;
    case SYS_fsync:       ret = sys_fsync((int)a0); break;
    case SYS_ftruncate:   ret = sys_ftruncate((int)a0, (size_t)a1); break;
    case SYS_truncate:    ret = sys_truncate((const char*)a0, (size_t)a1); break;
    case SYS_sendfile:    ret = sys_sendfile((int)a0, (int)a1, (long*)a2, (size_t)a3); break;
    case SYS_select:      ret = sys_select((int)a0, (void*)a1, (void*)a2, (void*)a3, (void*)a4); break;
    case SYS_ppoll:       ret = sys_ppoll((void*)a0, (int)a1, (void*)a2, (void*)a3); break;
    case SYS_epoll_create1: ret = sys_epoll_create1((int)a0); break;

    case SYS_mkdirat:     ret = sys_mkdirat((int)a0, (const char*)a1, (int)a2); break;
    case SYS_unlinkat:    ret = sys_unlinkat((int)a0, (const char*)a1, (int)a2); break;
    case SYS_renameat2:   ret = sys_renameat2((int)a0, (const char*)a1, (int)a2, (const char*)a3, (int)a4); break;
    case SYS_chdir:       ret = sys_chdir((const char*)a0); break;
    case SYS_getcwd:      ret = sys_getcwd((char*)a0, (size_t)a1); break;
    case SYS_fstat:       ret = sys_fstat((int)a0, (void*)a1); break;
    case SYS_fstatat:     ret = sys_fstatat((int)a0, (const char*)a1, (void*)a2, (int)a3); break;
    case SYS_readlinkat:  ret = sys_readlinkat((int)a0, (const char*)a1, (char*)a2, (size_t)a3); break;
    case SYS_faccessat:   ret = sys_faccessat((int)a0, (const char*)a1, (int)a2); break;
    case SYS_getdents64:  ret = sys_getdents64((int)a0, (void*)a1, (size_t)a2); break;
    case SYS_linkat:      ret = sys_linkat((int)a0, (const char*)a1, (int)a2, (const char*)a3, (int)a4); break;
    case SYS_symlinkat:   ret = sys_symlinkat((const char*)a0, (int)a1, (const char*)a2); break;
    case SYS_statfs:      ret = sys_statfs((const char*)a0, (void*)a1); break;
    case SYS_fstatfs:     ret = sys_fstatfs((int)a0, (void*)a1); break;
    case SYS_mount:       ret = sys_mount((const char*)a0, (const char*)a1, (const char*)a2, (int)a3); break;
    case SYS_umount2:     ret = sys_umount2((const char*)a0, (int)a1); break;
    case SYS_utimensat:   ret = sys_utimensat((int)a0, (const char*)a1, (void*)a2, (int)a3); break;

    case SYS_exit:
        syscall_profile_record(num, start_time, syscall_profile_now());
        return sys_exit((int)a0);
    case SYS_exit_group:
        syscall_profile_record(num, start_time, syscall_profile_now());
        return sys_exit_group((int)a0);
    case SYS_getpid:      ret = sys_getpid(); break;
    case SYS_getppid:     ret = sys_getppid(); break;
    case SYS_gettid:      ret = sys_gettid(); break;
    case SYS_set_tid_address: ret = sys_set_tid_address((int*)a0); break;
    case SYS_set_robust_list: ret = sys_set_robust_list((void*)a0, (size_t)a1); break;
    case SYS_getuid:      ret = sys_getuid(); break;
    case SYS_geteuid:     ret = sys_geteuid(); break;
    case SYS_getgid:      ret = sys_getgid(); break;
    case SYS_getegid:     ret = sys_getegid(); break;
    case SYS_getpgid:     ret = sys_getpgid((int)a0); break;
    case SYS_setpgid:     ret = sys_setpgid((int)a0, (int)a1); break;
    case SYS_setsid:      ret = sys_setsid(); break;
    case SYS_clone:       ret = sys_clone(a0, (void*)a1, (int*)a2, (int*)a3, a4); break;
    case SYS_execve:      ret = sys_execve((const char*)a0, (char**)a1, (char**)a2); break;
    case SYS_wait4:       ret = sys_wait4((int)a0, (int*)a1, (int)a2, (void*)a3); break;
    case SYS_sched_yield: ret = sys_sched_yield(); break;
    case SYS_reboot:      ret = sys_reboot((int)a0); break;
    case SYS_prctl:       ret = sys_prctl((int)a0, a1, a2, a3, a4); break;
    case SYS_prlimit64:   ret = sys_prlimit64((int)a0, (int)a1, (void*)a2, (void*)a3); break;
    case SYS_getrlimit:   ret = sys_getrlimit((int)a0, (void*)a1); break;
    case SYS_setrlimit:   ret = sys_setrlimit((int)a0, (void*)a1); break;
    case SYS_getrusage:   ret = sys_getrusage((int)a0, (void*)a1); break;

    case SYS_kill:        ret = sys_kill((int)a0, (int)a1); break;
    case SYS_tgkill:      ret = sys_tgkill((int)a0, (int)a1, (int)a2); break;
    case SYS_sigaction:   ret = sys_sigaction((int)a0, (void*)a1, (void*)a2); break;
    case SYS_sigprocmask: ret = sys_sigprocmask((int)a0, (void*)a1, (void*)a2); break;
    case SYS_sigtimedwait: ret = sys_sigtimedwait((const uint64_t*)a0, (void*)a1, (const void*)a2); break;
    case SYS_sigreturn:
        ret = sys_sigreturn();
        if (ret == 0) {
            task_t *cur = proc_current();
            if (cur && cur->sig_handling) {
                *ctx = cur->sig_saved_ctx;
                cur->sig_handling = 0;
            }
        }
        syscall_profile_record(num, start_time, syscall_profile_now());
        signal_deliver_user(ctx);
        return ret;
    case SYS_sigsuspend:  ret = sys_sigsuspend((void*)a0); break;

    case SYS_brk:         ret = sys_brk(a0); break;
    case SYS_mmap:        ret = sys_mmap(a0, (size_t)a1, (int)a2, (int)a3, (int)a4, (long)a5); break;
    case SYS_munmap:      ret = sys_munmap(a0, (size_t)a1); break;
    case SYS_mprotect:    ret = sys_mprotect(a0, (size_t)a1, (int)a2); break;
    case SYS_madvise:     ret = sys_madvise(a0, (size_t)a1, (int)a2); break;
    case SYS_mremap:      ret = sys_mremap(a0, (size_t)a1, (size_t)a2, (int)a3, (uint64_t)a4); break;
    case SYS_shm_open:    ret = sys_shm_open((const char*)a0, (int)a1, (int)a2); break;

    case SYS_clock_gettime: ret = sys_clock_gettime((int)a0, (void*)a1); break;
    case SYS_clock_getres:  ret = sys_clock_getres((int)a0, (void*)a1); break;
    case SYS_nanosleep:     ret = sys_nanosleep((void*)a0, (void*)a1); break;
    case SYS_gettimeofday:  ret = sys_gettimeofday((void*)a0, (void*)a1); break;
    case SYS_times:         ret = sys_times((void*)a0); break;
    case SYS_time:          ret = sys_time((long*)a0); break;

    case SYS_uname:       ret = sys_uname((void*)a0); break;
    case SYS_sysinfo:     ret = sys_sysinfo((void*)a0); break;
    case SYS_getgroups:   ret = sys_getgroups((int)a0, (int*)a1); break;
    case SYS_setgroups:   ret = sys_setgroups((size_t)a0, (const int*)a1); break;
    case SYS_umask:       ret = sys_umask((int)a0); break;
    case SYS_syslog:      ret = sys_syslog((int)a0, (char*)a1, (int)a2); break;

    case SYS_getrandom:   ret = sys_getrandom((void*)a0, (size_t)a1, (int)a2); break;
    case SYS_futex:       ret = sys_futex((int*)a0, (int)a1, (int)a2, (void*)a3, (int*)a4, (int)a5); break;

    default:
        if (num < 300)
            kdebug("[SYSCALL] Unimplemented: %lu\n", (unsigned long)num);
        ret = -ENOSYS;
        break;
    }

    ctx->x[10] = (uint64_t)ret;
    syscall_profile_record(num, start_time, syscall_profile_now());
    signal_deliver_user(ctx);
    return ret;
}

/* ============================================================
 * File I/O
 * ============================================================ */

int64_t sys_read(int fd, char *buf, size_t count) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_read(gfd, buf, count);
}

int64_t sys_write(int fd, const char *buf, size_t count) {
    if (!buf) return -EFAULT;
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_write(gfd, buf, count);
}

int64_t sys_pread64(int fd, char *buf, size_t count, long off) {
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    long curoff = vfs_lseek(gfd, 0, SEEK_CUR);
    vfs_lseek(gfd, off, SEEK_SET);
    int64_t r = vfs_read(gfd, buf, count);
    vfs_lseek(gfd, curoff, SEEK_SET);
    return r;
}

int64_t sys_pwrite64(int fd, char *buf, size_t count, long off) {
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    long curoff = vfs_lseek(gfd, 0, SEEK_CUR);
    vfs_lseek(gfd, off, SEEK_SET);
    int64_t r = vfs_write(gfd, buf, count);
    vfs_lseek(gfd, curoff, SEEK_SET);
    return r;
}

int64_t sys_writev(int fd, const void *iov, int iovcnt) {
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    struct iovec { char *base; size_t len; };
    const struct iovec *v = (const struct iovec *)iov;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!v[i].base || v[i].len == 0) continue;
        int64_t n = vfs_write(gfd, v[i].base, v[i].len);
        if (n < 0) return n;
        total += n;
    }
    return total;
}

int64_t sys_readv(int fd, const void *iov, int iovcnt) {
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    struct iovec { char *base; size_t len; };
    const struct iovec *v = (const struct iovec *)iov;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!v[i].base || v[i].len == 0) continue;
        int64_t n = vfs_read(gfd, v[i].base, v[i].len);
        if (n < 0) return n;
        total += n;
        if ((size_t)n < v[i].len) break;
    }
    return total;
}

int64_t sys_openat(int dirfd, const char *path, int flags, int mode) {
    (void)dirfd;
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    int gfd = vfs_open(kpath, flags, mode);
    if (gfd < 0) return gfd;
    task_t *t = proc_current();
    return alloc_local_fd(t, gfd);
}

int64_t sys_close(int fd) {
    task_t *t = proc_current();
    if (!t || fd < 0 || fd >= MAX_FILES) return -EBADF;
    int gfd = t->fd_table[fd];
    if (gfd < 0) return -EBADF;
    t->fd_table[fd] = -1;
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
            vf->ref_count++;
            return newfd;
        }
    }
    return -EMFILE;
}

int64_t sys_dup3(int oldfd, int newfd, int flags) {
    (void)flags;
    task_t *t = proc_current();
    if (!t || oldfd < 0 || oldfd >= MAX_FILES || newfd < 0 || newfd >= MAX_FILES) return -EBADF;
    int old_gfd = t->fd_table[oldfd];
    if (old_gfd < 0) return -EBADF;
    if (oldfd == newfd) return newfd;
    vfile_t *vf = vfs_get_file(old_gfd);
    if (!vf) return -EBADF;
    int cur_gfd = t->fd_table[newfd];
    if (cur_gfd >= 0) {
        vfs_close(cur_gfd);
    }
    t->fd_table[newfd] = old_gfd;
    vf->ref_count++;
    return newfd;
}

int64_t sys_fcntl(int fd, int cmd, long arg) {
    /* F_DUPFD=0, F_DUPFD_CLOEXEC=1030: share the same global fd */
    if (cmd == 0 || cmd == 1030) {
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
                vf->ref_count++;
                return i;
            }
        }
        return -EMFILE;
    }
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_fcntl(gfd, cmd, arg);
}

int64_t sys_pipe2(int *pipefd, int flags) {
    (void)flags;
    if (!pipefd) return -EFAULT;
    int gfd[2];
    int r = vfs_pipe(gfd);
    if (r == 0) {
        task_t *t = proc_current();
        pipefd[0] = alloc_local_fd(t, gfd[0]);
        if (pipefd[0] < 0) {
            vfs_close(gfd[1]);
            return pipefd[0];
        }
        pipefd[1] = alloc_local_fd(t, gfd[1]);
        if (pipefd[1] < 0) {
            t->fd_table[pipefd[0]] = -1;
            vfs_close(gfd[1]);
            return pipefd[1];
        }
    }
    return r;
}

int64_t sys_ioctl(int fd, unsigned long req, void *arg) {
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_ioctl(gfd, req, arg);
}

int64_t sys_sync(void) {
    return 0;
}

int64_t sys_fsync(int fd) {
    (void)fd;
    return 0;
}

int64_t sys_ftruncate(int fd, size_t size) {
    int64_t gfd = get_global_fd(fd);
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
    int64_t out_gfd = get_global_fd(out_fd);
    if (out_gfd < 0) return out_gfd;
    int64_t in_gfd = get_global_fd(in_fd);
    if (in_gfd < 0) return in_gfd;
    long cur_off = off ? *off : vfs_lseek(in_gfd, 0, SEEK_CUR);
    long saved = off ? vfs_lseek(in_gfd, 0, SEEK_CUR) : 0;
    int64_t total = 0;
    char sbuf[4096];
    while ((size_t)total < count) {
        size_t chunk = count - (size_t)total;
        if (chunk > sizeof(sbuf)) chunk = sizeof(sbuf);
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
        *off = cur_off;
        vfs_lseek(in_gfd, saved, SEEK_SET);
    }
    return total;
}

int64_t sys_ppoll(void *fds, int nfds, void *tmo, void *sigmask) {
    struct pollfd { int fd; short events; short revents; } *pfds = (void*)fds;
    (void)tmo; (void)sigmask;
    if (!pfds || nfds <= 0) return 0;
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        pfds[i].revents = 0;
        if (pfds[i].fd < 0) continue;
        int64_t gfd = get_global_fd(pfds[i].fd);
        vfile_t *vf = (gfd >= 0) ? vfs_get_file(gfd) : NULL;
        if (vf) {
            if (pfds[i].events & 0x001) pfds[i].revents |= 0x001;
            if (pfds[i].events & 0x004) pfds[i].revents |= 0x004;
            ready++;
        } else {
            pfds[i].revents = 0x008;
            ready++;
        }
    }
    return ready;
}

int64_t sys_epoll_create1(int flags) {
    (void)flags;
    vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
    if (!vf) return -ENOMEM;
    memset(vf, 0, sizeof(*vf));
    vf->ref_count = 1;
    int efd = vfs_alloc_fd(vf);
    if (efd < 0) { kfree(vf); return -EMFILE; }
    return efd;
}

/* ============================================================
 * Directory / Path
 * ============================================================ */

int64_t sys_mkdirat(int dirfd, const char *path, int mode) {
    (void)dirfd;
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_mkdir(kpath, mode);
}

int64_t sys_unlinkat(int dirfd, const char *path, int flags) {
    (void)dirfd;
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    if (flags & AT_REMOVEDIR) return vfs_rmdir(kpath);
    return vfs_unlink(kpath);
}

int64_t sys_renameat2(int olddir, const char *oldpath,
                       int newdir, const char *newpath, int flags) {
    (void)olddir; (void)newdir; (void)flags;
    if (!oldpath || !newpath) return -EFAULT;
    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (user_strncpy(kold, oldpath, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(knew, newpath, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_rename(kold, knew);
}

int64_t sys_chdir(const char *path) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_chdir(kpath);
}

int64_t sys_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) return -EFAULT;
    return vfs_getcwd(buf, size);
}

static void copy_kstat_to_user(void *st, const kstat_t *kst) {
    /* Match RISC-V glibc struct stat layout (128 bytes, asm-generic/stat.h):
     *   unsigned long st_dev       offset 0
     *   unsigned long st_ino       offset 8
     *   unsigned int  st_mode      offset 16
     *   unsigned int  st_nlink     offset 20
     *   unsigned int  st_uid       offset 24
     *   unsigned int  st_gid       offset 28
     *   unsigned long st_rdev      offset 32
     *   unsigned long __pad1       offset 40
     *   unsigned long st_size      offset 48
     *   unsigned int  st_blksize   offset 56
     *   unsigned int  __pad2       offset 60
     *   unsigned long st_blocks    offset 64
     *   unsigned long st_atime     offset 72
     *   unsigned long st_atime_nsec offset 80
     *   unsigned long st_mtime     offset 88
     *   unsigned long st_mtime_nsec offset 96
     *   unsigned long st_ctime     offset 104
     *   unsigned long st_ctime_nsec offset 112
     *   unsigned int  __unused4    offset 120
     *   unsigned int  __unused5    offset 124
     */
    uint64_t *u64 = (uint64_t *)st;
    uint32_t *u32 = (uint32_t *)st;
    u64[0]  = kst->st_dev;
    u64[1]  = kst->st_ino;
    u32[4]  = kst->st_mode;
    u32[5]  = kst->st_nlink;
    u32[6]  = kst->st_uid;
    u32[7]  = kst->st_gid;
    u64[4]  = kst->st_rdev;
    u64[5]  = 0;            /* __pad1 */
    u64[6]  = kst->st_size;
    u32[14] = kst->st_blksize;
    u32[15] = 0;            /* __pad2 */
    u64[8]  = kst->st_blocks;
    u64[9]  = kst->st_atime;
    u64[10] = kst->st_atime_nsec;
    u64[11] = kst->st_mtime;
    u64[12] = kst->st_mtime_nsec;
    u64[13] = kst->st_ctime;
    u64[14] = kst->st_ctime_nsec;
    u32[30] = 0;            /* __unused4 */
    u32[31] = 0;            /* __unused5 */
}

int64_t sys_fstat(int fd, void *st) {
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    kstat_t kst;
    int r = vfs_fstat(gfd, &kst);
    if (r < 0) return r;
    if (!st) return -EFAULT;
    copy_kstat_to_user(st, &kst);
    return 0;
}

int64_t sys_fstatat(int dirfd, const char *path, void *st, int flags) {
    (void)dirfd; (void)flags;
    if (!path || !st) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    kstat_t kst;
    int r = vfs_fstatat(dirfd, kpath, &kst, flags);
    if (r < 0) return r;
    copy_kstat_to_user(st, &kst);
    return 0;
}

int64_t sys_readlinkat(int dirfd, const char *path, char *buf, size_t sz) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_readlinkat(dirfd, kpath, buf, sz);
}

int64_t sys_faccessat(int dirfd, const char *path, int mode) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_faccessat(dirfd, kpath, mode);
}

int64_t sys_getdents64(int fd, void *dirp, size_t count) {
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    return vfs_getdents64(gfd, dirp, count);
}

int64_t sys_linkat(int olddirfd, const char *oldpath,
                    int newdirfd, const char *newpath, int flags) {
    (void)olddirfd; (void)newdirfd; (void)flags;
    if (!oldpath || !newpath) return -EFAULT;
    char kold[MAX_PATH_LEN], knew[MAX_PATH_LEN];
    if (user_strncpy(kold, oldpath, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(knew, newpath, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_link(kold, knew);
}

int64_t sys_symlinkat(const char *target, int newdirfd, const char *linkpath) {
    (void)newdirfd;
    if (!target || !linkpath) return -EFAULT;
    char ktarget[MAX_PATH_LEN], klink[MAX_PATH_LEN];
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(klink, linkpath, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_symlink(ktarget, klink);
}

int64_t sys_statfs(const char *path, void *buf) {
    (void)path;
    if (!buf) return -EFAULT;
    uint64_t *sb = (uint64_t *)buf;
    memset(sb, 0, 64);
    sb[0] = 0x4006;
    sb[1] = 4096;
    sb[2] = 1024*1024;
    sb[3] = 512*1024;
    sb[4] = 512*1024;
    return 0;
}

int64_t sys_fstatfs(int fd, void *buf) {
    (void)fd;
    return sys_statfs(NULL, buf);
}

int64_t sys_mount(const char *src, const char *target,
                   const char *fstype, int flags) {
    if (!src || !target || !fstype) return -EFAULT;
    char ksrc[MAX_PATH_LEN], ktarget[MAX_PATH_LEN], kfstype[32];
    if (user_strncpy(ksrc, src, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    if (user_strncpy(kfstype, fstype, 32) < 0) return -EFAULT;
    return vfs_mount(ksrc, ktarget, kfstype, flags);
}

int64_t sys_umount2(const char *target, int flags) {
    (void)flags;
    if (!target) return -EFAULT;
    char ktarget[MAX_PATH_LEN];
    if (user_strncpy(ktarget, target, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_umount(ktarget);
}

int64_t sys_utimensat(int dirfd, const char *path, void *times, int flags) {
    (void)dirfd; (void)path; (void)times; (void)flags;
    return 0;
}

/* ============================================================
 * Process
 * ============================================================ */

int64_t sys_exit(int code) {
    proc_exit(code);
    return 0;
}

int64_t sys_exit_group(int code) {
    proc_exit(code);
    return 0;
}

int64_t sys_getpid(void) {
    task_t *t = proc_current();
    return t ? t->pid : 0;
}

int64_t sys_getppid(void) {
    task_t *t = proc_current();
    return t ? t->ppid : 0;
}

int64_t sys_gettid(void) {
    task_t *t = proc_current();
    return t ? t->pid : 0;
}

int64_t sys_set_tid_address(int *tidptr) {
    (void)tidptr;
    return sys_getpid();
}

int64_t sys_set_robust_list(void *head, size_t len) {
    (void)head; (void)len;
    return 0;
}

int64_t sys_getuid(void) { return 0; }
int64_t sys_geteuid(void) { return 0; }
int64_t sys_getgid(void) { return 0; }
int64_t sys_getegid(void) { return 0; }

int64_t sys_getpgid(int pid) {
    (void)pid;
    return sys_getpid();
}

int64_t sys_setpgid(int pid, int pgid) {
    (void)pid; (void)pgid;
    return 0;
}

int64_t sys_setsid(void) {
    return sys_getpid();
}

int64_t sys_clone(uint64_t flags, void *stack, int *ptid, int *ctid, uint64_t tls) {
    return proc_clone(flags, (uint64_t)stack, ptid, ctid, tls);
}

int64_t sys_execve(const char *path, char **argv, char **envp) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long r = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (r < 0) return -EFAULT;
    return proc_exec(kpath, argv, envp);
}

int64_t sys_wait4(int pid, int *status, int options, void *rusage) {
    (void)rusage;
    return proc_wait4(pid, status, options);
}

int64_t sys_sched_yield(void) {
    proc_yield();
    return 0;
}

int64_t sys_reboot(int cmd) {
    if (cmd == 0x424F4F54) sbi_reboot();
    else sbi_shutdown();
    return 0;
}

int64_t sys_prctl(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (op == 15) {
        task_t *t = proc_current();
        if (t) proc_set_name(t, (const char *)a1);
    }
    return 0;
}

int64_t sys_prlimit64(int pid, int resource, void *new_rlim, void *old_rlim) {
    (void)pid;
    if (old_rlim) {
        uint64_t *r = (uint64_t *)old_rlim;
        task_t *t = proc_current();
        switch (resource) {
            case 3: r[0] = 0; r[1] = t ? t->rlim_stack : 8*1024*1024; break;
            case 7: r[0] = 0; r[1] = t ? t->rlim_nofile : MAX_FILES; break;
            default: r[0] = 0; r[1] = (uint64_t)-1; break;
        }
    }
    if (new_rlim) {
        uint64_t *r = (uint64_t *)new_rlim;
        task_t *t = proc_current();
        if (!t) return -ESRCH;
        switch (resource) {
            case 3: t->rlim_stack = r[1]; break;
            case 7: t->rlim_nofile = r[1]; break;
            default: break;
        }
    }
    return 0;
}

int64_t sys_getrlimit(int resource, void *rlim) {
    if (!rlim) return -EFAULT;
    uint64_t *r = (uint64_t *)rlim;
    task_t *t = proc_current();
    switch (resource) {
        case 3: r[0] = 0; r[1] = t ? t->rlim_stack : 8*1024*1024; break;
        case 7: r[0] = 0; r[1] = t ? t->rlim_nofile : MAX_FILES; break;
        default: r[0] = 0; r[1] = (uint64_t)-1; break;
    }
    return 0;
}

int64_t sys_setrlimit(int resource, void *rlim) {
    if (!rlim) return -EFAULT;
    uint64_t *r = (uint64_t *)rlim;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    switch (resource) {
        case 3: t->rlim_stack = r[1]; break;
        case 7: t->rlim_nofile = r[1]; break;
        default: break;
    }
    return 0;
}

int64_t sys_getrusage(int who, void *usage) {
    (void)who;
    if (!usage) return -EFAULT;
    memset(usage, 0, 144);
    task_t *t = proc_current();
    if (t) {
        uint64_t *u = (uint64_t *)usage;
        u[0] = t->total_time / TICKS_PER_SEC;
        u[1] = (t->total_time % TICKS_PER_SEC) * 1000000000UL / TICKS_PER_SEC / 1000;
    }
    return 0;
}

/* ============================================================
 * Signal
 * ============================================================ */

int64_t sys_kill(int pid, int sig) {
    return proc_kill(pid, sig);
}

int64_t sys_tgkill(int tgid, int tid, int sig) {
    (void)tgid;
    return proc_kill(tid, sig);
}

int64_t sys_sigaction(int signum, void *act, void *oldact) {
    return sys_sigaction_impl(signum, (const sigaction_t *)act, (sigaction_t *)oldact);
}

int64_t sys_sigprocmask(int how, void *set, void *oldset) {
    return sys_sigprocmask_impl(how, (const uint64_t *)set, (uint64_t *)oldset);
}

int64_t sys_sigreturn(void) {
    task_t *t = proc_current();
    if (t && t->signals) {
        signal_state_t *ss = (signal_state_t *)t->signals;
        if (t->sig_handling) {
            ss->blocked = t->sig_old_blocked;
        }
    }
    return 0;
}

int64_t sys_sigsuspend(void *mask) {
    task_t *t = proc_current();
    if (!t || !t->signals || !mask) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t old_blocked = ss->blocked;
    uint64_t new_mask = *(const uint64_t *)mask;

    /* Can't block SIGKILL or SIGSTOP */
    new_mask &= ~((1ULL << SIGKILL) | (1ULL << SIGSTOP));
    ss->blocked = new_mask;

    uint64_t deliverable = ss->pending & ~ss->blocked;
    if (!deliverable) {
        t->state = PROC_BLOCKED;
        sched();
    }

    /* Deliver/clear any signals that were pending with the temporary mask */
    signal_deliver();

    ss->blocked = old_blocked;
    return -EINTR;
}

int64_t sys_sigtimedwait(const uint64_t *set, void *info, const void *timeout) {
    task_t *t = proc_current();
    if (!t || !t->signals || !set) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t mask = *set;
    uint64_t deliverable = ss->pending & ~ss->blocked & mask;

    if (!deliverable) {
        if (timeout) {
            uint64_t sec = ((const uint64_t *)timeout)[0];
            uint64_t nsec = ((const uint64_t *)timeout)[1];
            if (sec == 0 && nsec == 0)
                return -EAGAIN;
        }
        t->state = PROC_BLOCKED;
        sched();
        deliverable = ss->pending & ~ss->blocked & mask;
        if (!deliverable)
            return -EAGAIN;
    }

    for (int sig = 1; sig < NSIG; sig++) {
        if (deliverable & (1ULL << sig)) {
            ss->pending &= ~(1ULL << sig);
            if (info) {
                memset(info, 0, 128);
                *(int *)info = sig;
            }
            return sig;
        }
    }
    return -EAGAIN;
}

/* ============================================================
 * Memory
 * ============================================================ */

int64_t sys_brk(uint64_t addr) {
    return (int64_t)proc_brk(addr);
}

int64_t sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off) {
    return (int64_t)proc_mmap(addr, len, prot, flags, fd, off);
}

int64_t sys_munmap(uint64_t addr, size_t len) {
    return proc_munmap(addr, len);
}

int64_t sys_mprotect(uint64_t addr, size_t len, int prot) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;
    return mm_mprotect(t->mm, addr, len, prot);
}

int64_t sys_madvise(uint64_t addr, size_t len, int advice) {
    (void)addr; (void)len; (void)advice;
    return 0;
}

int64_t sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size, int flags, uint64_t new_addr) {
#define MREMAP_MAYMOVE   1
#define MREMAP_FIXED     2
    task_t *t = proc_current();
    if (!t || !t->mm) return -EINVAL;

    if (new_size == 0) return -EINVAL;
    if (old_addr & (PAGE_SIZE - 1)) return -EINVAL;

    size_t old_len = ROUND_UP(old_size, PAGE_SIZE);
    size_t new_len = ROUND_UP(new_size, PAGE_SIZE);

    vm_area_t *vma = mm_find_vma(t->mm, old_addr);
    if (!vma || vma->start != old_addr) return -EFAULT;
    if (old_size == 0) old_len = vma->end - vma->start;
    if (!old_len) return -EINVAL;

    if (new_len <= old_len) {
        if (new_len < old_len)
            mm_munmap(t->mm, old_addr + new_len, old_len - new_len);
        return (int64_t)old_addr;
    }

    uint64_t grow_by = new_len - old_len;
    uint64_t new_end = old_addr + new_len;
    int can_grow = 1;

    if ((flags & MREMAP_FIXED) && new_addr != old_addr)
        can_grow = 0;
    for (vm_area_t *v = t->mm->mmap; v; v = v->next) {
        if (v == vma) continue;
        if (v->start < new_end && v->end > old_addr + old_len) {
            can_grow = 0;
            break;
        }
    }

    if (can_grow) {
        vma->end = new_end;
        t->mm->total_vm += grow_by / PAGE_SIZE;
        return (int64_t)old_addr;
    }

    if (!(flags & MREMAP_MAYMOVE)) return -ENOMEM;

    int prot = ((vma->pte_flags & PTE_R) ? PROT_READ : 0) |
               ((vma->pte_flags & PTE_W) ? PROT_WRITE : 0) |
               ((vma->pte_flags & PTE_X) ? PROT_EXEC : 0);
    uint64_t target = (flags & MREMAP_FIXED) ? new_addr : 0;
    uint64_t dst = mm_mmap(t->mm, target, new_len, prot,
                            (target ? MAP_FIXED : 0) | MAP_ANONYMOUS | MAP_PRIVATE);
    if ((int64_t)dst < 0) return dst;

    for (uint64_t off = 0; off < old_len; off += PAGE_SIZE) {
        paddr_t pa = pt_translate(t->mm->pgdir, old_addr + off);
        if (pa == 0) continue;
        void *src = (void *)((uint64_t)pa + PAGE_OFFSET);

        void *frame = frame_alloc();
        if (!frame) {
            mm_munmap(t->mm, dst, new_len);
            return -ENOMEM;
        }
        memcpy(frame, src, PAGE_SIZE);
        paddr_t frame_pa = (paddr_t)((uint64_t)(uintptr_t)frame - PAGE_OFFSET);
        int r = pt_map(t->mm->pgdir, dst + off, frame_pa, vma->pte_flags);
        if (r < 0) {
            frame_free(frame);
            mm_munmap(t->mm, dst, new_len);
            return -ENOMEM;
        }
    }

    mm_munmap(t->mm, old_addr, old_len);
    return (int64_t)dst;
}

int64_t sys_shm_open(const char *name, int oflag, int mode) {
    (void)name; (void)oflag; (void)mode;
    return -ENOSYS;
}

/* ============================================================
 * Time
 * ============================================================ */

int64_t sys_clock_gettime(int clk, void *tp) {
    (void)clk;
    if (!tp) return -EFAULT;
    uint64_t ticks = timer_get_ticks();
    uint64_t *ts = (uint64_t *)tp;
    ts[0] = ticks / TICKS_PER_SEC;
    ts[1] = (ticks % TICKS_PER_SEC) * 1000000000UL / TICKS_PER_SEC;
    return 0;
}

int64_t sys_clock_getres(int clk, void *tp) {
    (void)clk;
    if (tp) {
        uint64_t *ts = (uint64_t *)tp;
        ts[0] = 0;
        ts[1] = 1000000000UL / TICKS_PER_SEC;
    }
    return 0;
}

int64_t sys_nanosleep(void *req, void *rem) {
    (void)rem;
    if (!req) return -EFAULT;
    uint64_t ts[2];
    if (copy_from_user(ts, req, sizeof(ts)) < 0) return -EFAULT;
    uint64_t sec  = ts[0];
    uint64_t nsec = ts[1];
    uint64_t ticks = sec * TICKS_PER_SEC + nsec * TICKS_PER_SEC / 1000000000UL;
    uint64_t until = timer_get_ticks() + ticks;

    task_t *t = proc_current();
    if (t) {
        t->wake_time = until;
        t->state     = PROC_BLOCKED;
        sched();
    } else {
        while (timer_get_ticks() < until) __asm__ volatile("nop");
    }
    return 0;
}

int64_t sys_gettimeofday(void *tv, void *tz) {
    (void)tz;
    if (tv) {
        uint64_t ticks = timer_get_ticks();
        uint64_t *t = (uint64_t *)tv;
        t[0] = ticks / TICKS_PER_SEC;
        t[1] = (ticks % TICKS_PER_SEC) * 1000000UL / TICKS_PER_SEC;
    }
    return 0;
}

int64_t sys_times(void *buf) {
    task_t *t = proc_current();
    if (t && buf) {
        uint64_t *tm = (uint64_t *)buf;
        memset(tm, 0, 32);
        tm[0] = t->total_time;
    }
    return (int64_t)(timer_get_ticks());
}

int64_t sys_time(long *tloc) {
    uint64_t t = timer_get_ticks() / TICKS_PER_SEC;
    if (tloc) *tloc = (long)t;
    return (int64_t)t;
}

/* ============================================================
 * System info
 * ============================================================ */

int64_t sys_uname(void *buf) {
    struct uname { char s[65],n[65],r[65],v[65],m[65],d[65]; };
    struct uname *u = (struct uname *)buf;
    if (!u) return -EFAULT;
    memset(u, 0, sizeof(*u));
    strcpy(u->s, "Linux");
    strcpy(u->n, "A20OS");
    strcpy(u->r, "6.1.0-A20OS");
    strcpy(u->v, "#1 SMP A20OS 2025");
#ifdef ARCH_RISCV64
    strcpy(u->m, "riscv64");
#elif defined(ARCH_LOONGARCH64)
    strcpy(u->m, "loongarch64");
#else
    strcpy(u->m, "riscv64");
#endif
    return 0;
}

int64_t sys_sysinfo(void *info) {
    if (!info) return -EFAULT;
    memset(info, 0, 112);
    uint64_t *si = (uint64_t *)info;
    si[0] = timer_get_ticks() / TICKS_PER_SEC;
    si[1] = 1;
    return 0;
}

int64_t sys_getgroups(int size, int *list) {
    (void)size; (void)list;
    return 0;
}

int64_t sys_setgroups(size_t size, const int *list) {
    (void)size; (void)list;
    return 0;
}

int64_t sys_umask(int newmask) {
    task_t *t = proc_current();
    if (!t) return 022;
    int old = t->umask;
    t->umask = newmask & 0777;
    return old;
}

int64_t sys_syslog(int type, char *buf, int len) {
    (void)type; (void)buf; (void)len;
    return 0;
}

/* ============================================================
 * Random / Misc
 * ============================================================ */

int64_t sys_getrandom(void *buf, size_t len, int flags) {
    (void)flags;
    if (!buf) return -EFAULT;
    uint8_t *p = (uint8_t *)buf;
    uint64_t seed = timer_get_ticks();
    for (size_t i = 0; i < len; i++) {
        seed ^= seed << 13;
        seed ^= seed >> 7;
        seed ^= seed << 17;
        p[i] = (uint8_t)(seed & 0xFF);
    }
    return (int64_t)len;
}

int64_t sys_futex(int *uaddr, int op, int val, void *timeout, int *uaddr2, int val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    if (!uaddr) return -EFAULT;
    int opc = op & 0x7F;
    if (opc == 0 || opc == 9) {
        if (*uaddr != val) return -EAGAIN;
        proc_yield();
        return 0;
    } else if (opc == 1 || opc == 10) {
        return 1;
    }
    return -ENOSYS;
}

/* ============================================================
 * select / poll
 * ============================================================ */

#define FD_ISSET(f, s) (((s)[(f)/8/sizeof(long)] & (1UL<<((f)%(8*sizeof(long))))) != 0)

int64_t sys_select(int nfds, void *readfds, void *writefds,
                   void *exceptfds, void *timeout) {
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    (void)exceptfds;

    int ready_count = 0;

    if (readfds) {
        for (int i = 0; i < nfds; i++) {
            if (FD_ISSET(i, (long *)readfds)) {
                if (i == 0) {
                    extern int uart_has_input(void);
                    if (uart_has_input()) ready_count++;
                } else if (i >= 0 && i < MAX_FILES && t->fd_table[i] >= 0) {
                    ready_count++;
                }
            }
        }
    }

    if (writefds) {
        for (int i = 0; i < nfds; i++) {
            if (FD_ISSET(i, (long *)writefds)) {
                if (i == 1 || i == 2) {
                    ready_count++;
                } else if (i >= 0 && i < MAX_FILES && t->fd_table[i] >= 0) {
                    ready_count++;
                }
            }
        }
    }

    if (ready_count > 0) return ready_count;

    if (timeout) {
        uint64_t *tv = (uint64_t *)timeout;
        if (tv[0] == 0 && tv[1] == 0) return 0;
    }

    proc_yield();
    return 0;
}
