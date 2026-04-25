/*
 * A20OS — Complete Syscall Dispatcher
 *
 * Implements the Linux-compatible syscall ABI used by the supported archs.
 */
#include "sys/syscall.h"
#include "fs/vfs.h"
#include "fs/fs.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/timer.h"
#include "drv/uart.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/timekeeping.h"

#define CLD_EXITED     1
#define CLD_KILLED     2
#define CLD_DUMPED     3
#define CLD_STOPPED    5
#define CLD_CONTINUED  6

static int sigsys_diag_count = 0;
static int sleep_diag_count = 0;

enum {
    CLOCK_REALTIME_ID = 0,
    CLOCK_MONOTONIC_ID = 1,
    CLOCK_PROCESS_CPUTIME_ID = 2,
    CLOCK_THREAD_CPUTIME_ID = 3,
    CLOCK_MONOTONIC_RAW_ID = 4,
    CLOCK_REALTIME_COARSE_ID = 5,
    CLOCK_MONOTONIC_COARSE_ID = 6,
    CLOCK_BOOTTIME_ID = 7,
    CLOCK_REALTIME_ALARM_ID = 8,
    CLOCK_BOOTTIME_ALARM_ID = 9,
    CLOCK_TAI_ID = 11,
};

static int clock_is_realtime(int clk) {
    switch (clk) {
    case CLOCK_REALTIME_ID:
    case CLOCK_REALTIME_COARSE_ID:
    case CLOCK_REALTIME_ALARM_ID:
    case CLOCK_TAI_ID:
        return 1;
    default:
        return 0;
    }
}

static int clock_is_monotonic(int clk) {
    switch (clk) {
    case CLOCK_MONOTONIC_ID:
    case CLOCK_MONOTONIC_RAW_ID:
    case CLOCK_MONOTONIC_COARSE_ID:
    case CLOCK_BOOTTIME_ID:
    case CLOCK_BOOTTIME_ALARM_ID:
        return 1;
    default:
        return 0;
    }
}

static uint64_t clamp_stack_rlimit(uint64_t cur, uint64_t max) {
    uint64_t limit = cur < max ? cur : max;
    if (limit > USER_STACK_MAX_SIZE)
        limit = USER_STACK_MAX_SIZE;
    return limit;
}

static uint64_t clamp_nofile_rlimit(uint64_t cur, uint64_t max) {
    uint64_t limit = cur < max ? cur : max;
    if (limit > MAX_FILES)
        limit = MAX_FILES;
    return limit;
}

static void set_uniform_rlimit(uint64_t pair[2], uint64_t limit) {
    pair[0] = limit;
    pair[1] = limit;
}

void syscall_init(void) {
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
    uint64_t num = TRAP_CTX_SYSCALL_NUM(ctx);
    uint64_t a0  = TRAP_CTX_ARG0(ctx);
    uint64_t a1  = TRAP_CTX_ARG1(ctx);
    uint64_t a2  = TRAP_CTX_ARG2(ctx);
    uint64_t a3  = TRAP_CTX_ARG3(ctx);
    uint64_t a4  = TRAP_CTX_ARG4(ctx);
    uint64_t a5  = TRAP_CTX_ARG5(ctx);
    (void)a4; (void)a5;

    int64_t ret = -ENOSYS;
    {
        static int syscall_diag_count = 0;
        task_t *cur = proc_current();
        if (syscall_diag_count < 160 && cur &&
            cur->pid >= ARCH_SYSCALL_TRACE_MIN_PID &&
            num != SYS_read && num != SYS_write) {
            syscall_diag_count++;
            kdebug("[SYSCALL DBG] pid=%d name=%s num=%lu a0=0x%lx a1=0x%lx a2=0x%lx a3=0x%lx\n",
                  cur->pid, cur->name, (unsigned long)num,
                  (unsigned long)a0, (unsigned long)a1,
                  (unsigned long)a2, (unsigned long)a3);
        }
    }

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
    case SYS_faccessat2:  ret = sys_faccessat((int)a0, (const char*)a1, (int)a2); break;
    case SYS_getdents64:  ret = sys_getdents64((int)a0, (void*)a1, (size_t)a2); break;
    case SYS_linkat:      ret = sys_linkat((int)a0, (const char*)a1, (int)a2, (const char*)a3, (int)a4); break;
    case SYS_symlinkat:   ret = sys_symlinkat((const char*)a0, (int)a1, (const char*)a2); break;
    case SYS_statx:       ret = sys_statx((int)a0, (const char*)a1, (int)a2, (unsigned)a3, (void*)a4); break;
    case SYS_statfs:      ret = sys_statfs((const char*)a0, (void*)a1); break;
    case SYS_fstatfs:     ret = sys_fstatfs((int)a0, (void*)a1); break;
    case SYS_mount:       ret = sys_mount((const char*)a0, (const char*)a1, (const char*)a2, (int)a3); break;
    case SYS_umount2:     ret = sys_umount2((const char*)a0, (int)a1); break;
    case SYS_utimensat:   ret = sys_utimensat((int)a0, (const char*)a1, (void*)a2, (int)a3); break;

    case SYS_exit:        ret = sys_exit((int)a0); break;
    case SYS_exit_group:  ret = sys_exit_group((int)a0); break;
    case SYS_waitid:      ret = sys_waitid((int)a0, (int)a1, (void*)a2, (int)a3, (void*)a4); break;
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

    case SYS_clock_settime: ret = sys_clock_settime((int)a0, (void*)a1); break;
    case SYS_clock_gettime: ret = sys_clock_gettime((int)a0, (void*)a1); break;
    case SYS_clock_getres:  ret = sys_clock_getres((int)a0, (void*)a1); break;
    case SYS_nanosleep:     ret = sys_nanosleep((void*)a0, (void*)a1); break;
    case SYS_gettimeofday:  ret = sys_gettimeofday((void*)a0, (void*)a1); break;
    case SYS_settimeofday:  ret = sys_settimeofday((void*)a0, (void*)a1); break;
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

    TRAP_CTX_SET_RET(ctx, ret);
    signal_deliver_user(ctx);
    if (num == SYS_sigsuspend) {
        task_t *cur = proc_current();
        if (cur && cur->signals && cur->sigsuspend_active && !cur->sig_handling) {
            signal_state_t *ss = (signal_state_t *)cur->signals;
            ss->blocked = cur->sigsuspend_old_blocked;
            cur->sigsuspend_active = 0;
            if (sigsys_diag_count < 128) {
                sigsys_diag_count++;
                kdebug("[SIGSYS] sigsuspend-ret pid=%d pending=0x%lx blocked=0x%lx\n",
                      cur->pid, (unsigned long)ss->pending, (unsigned long)ss->blocked);
            }
        }
    }
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
    int64_t gfd = get_global_fd(fd);
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
    int64_t gfd = get_global_fd(fd);
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
    int64_t gfd = get_global_fd(fd);
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
    int64_t gfd = get_global_fd(fd);
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
    int64_t gfd = get_global_fd(fd);
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
        int fd0 = alloc_local_fd(t, gfd[0]);
        if (fd0 < 0) {
            vfs_close(gfd[1]);
            return fd0;
        }
        int fd1 = alloc_local_fd(t, gfd[1]);
        if (fd1 < 0) {
            t->fd_table[fd0] = -1;
            vfs_close(gfd[1]);
            return fd1;
        }
        int user_fds[2] = {fd0, fd1};
        if (copy_to_user(pipefd, user_fds, sizeof(user_fds)) < 0) {
            t->fd_table[fd0] = -1;
            t->fd_table[fd1] = -1;
            vfs_close(gfd[0]);
            vfs_close(gfd[1]);
            return -EFAULT;
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
    (void)tmo; (void)sigmask;
    if (!pfds || nfds <= 0) return 0;
    int ready = 0;
    for (int i = 0; i < nfds; i++) {
        struct pollfd pfd;
        if (copy_from_user(&pfd, &pfds[i], sizeof(pfd)) < 0) return -EFAULT;
        pfd.revents = 0;
        if (pfd.fd < 0) {
            copy_to_user(&pfds[i].revents, &pfd.revents, sizeof(short));
            continue;
        }
        int64_t gfd = get_global_fd(pfd.fd);
        vfile_t *vf = (gfd >= 0) ? vfs_get_file(gfd) : NULL;
        if (vf) {
            if (pfd.events & 0x001) pfd.revents |= 0x001;
            if (pfd.events & 0x004) pfd.revents |= 0x004;
            ready++;
        } else {
            pfd.revents = 0x008;
            ready++;
        }
        copy_to_user(&pfds[i].revents, &pfd.revents, sizeof(short));
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
    task_t *t = proc_current();
    if (!t) return -EFAULT;
    size_t len = strlen(t->cwd) + 1;
    if (size < len) return -ERANGE;
    if (copy_to_user(buf, t->cwd, len) < 0) return -EFAULT;
    return (int64_t)len;
}

static void copy_kstat_to_user(void *st, const kstat_t *kst) {
    uint64_t buf64[128 / 8];
    memset(buf64, 0, sizeof(buf64));
    uint8_t *buf = (uint8_t *)buf64;
    uint64_t *u64 = (uint64_t *)buf;
    uint32_t *u32 = (uint32_t *)buf;
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
    copy_to_user(st, buf, sizeof(buf64));
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
    if (sz > 4096) sz = 4096;
    char *kbuf = kmalloc(4096);
    if (!kbuf) return -ENOMEM;
    int64_t r = vfs_readlinkat(dirfd, kpath, kbuf, sz);
    if (r > 0) {
        if (copy_to_user(buf, kbuf, (size_t)r) < 0) { kfree(kbuf); return -EFAULT; }
    }
    kfree(kbuf);
    return r;
}

int64_t sys_faccessat(int dirfd, const char *path, int mode) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    return vfs_faccessat(dirfd, kpath, mode);
}

int64_t sys_statx(int dirfd, const char *path, int flags, unsigned mask, void *buf) {
    if (!path || !buf) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    if (user_strncpy(kpath, path, MAX_PATH_LEN) < 0) return -EFAULT;
    kstat_t kst;
    int r;

    /* LoongArch libc converts fstat(fd) → statx(fd,"",AT_EMPTY_PATH,…).
     * An empty path with AT_EMPTY_PATH means "stat the fd itself". */
    if ((flags & AT_EMPTY_PATH) && kpath[0] == '\0') {
        int64_t gfd = get_global_fd(dirfd);
        if (gfd < 0) return gfd;
        r = vfs_fstat(gfd, &kst);
    } else {
        r = vfs_fstatat(dirfd, kpath, &kst, flags);
    }
    if (r < 0) return r;

    /* struct statx layout (256 bytes total):
     *   0:  stx_mask          u32
     *   4:  stx_blksize       u32
     *   8:  stx_attributes    u64
     *  16:  stx_nlink         u32
     *  20:  stx_uid           u32
     *  24:  stx_gid           u32
     *  28:  stx_mode          u16 + pad u16
     *  32:  stx_ino           u64
     *  40:  stx_size          u64
     *  48:  stx_blocks        u64
     *  56:  stx_attributes_mask u64
     *  64:  stx_atime {sec:i64, nsec:u32, pad:i32}  = 16 bytes
     *  80:  stx_btime {sec:i64, nsec:u32, pad:i32}  = 16 bytes
     *  96:  stx_ctime {sec:i64, nsec:u32, pad:i32}  = 16 bytes
     * 112:  stx_mtime {sec:i64, nsec:u32, pad:i32}  = 16 bytes
     * 128:  stx_rdev_major    u32
     * 132:  stx_rdev_minor    u32
     * 136:  stx_dev_major     u32
     * 140:  stx_dev_minor     u32
     * 144:  spare[14]         u64 × 14
     */
    uint64_t stx_buf64[256 / 8];
    memset(stx_buf64, 0, sizeof(stx_buf64));
    uint8_t *stx = (uint8_t *)stx_buf64;
    uint32_t *u32 = (uint32_t *)stx;
    uint64_t *u64 = (uint64_t *)stx;
    uint16_t *u16 = (uint16_t *)stx;

    u32[0]  = (uint32_t)(mask | STATX_BASIC_STATS);
    u32[1]  = (uint32_t)kst.st_blksize;
    u32[4]  = kst.st_nlink;
    u32[5]  = kst.st_uid;
    u32[6]  = kst.st_gid;
    u16[14] = (uint16_t)kst.st_mode;
    u16[15] = 0;
    u64[4]  = kst.st_ino;
    u64[5]  = kst.st_size;
    u64[6]  = kst.st_blocks;

    *(int64_t *)(stx + 64)  = (int64_t)kst.st_atime;
    *(uint32_t *)(stx + 72) = (uint32_t)kst.st_atime_nsec;
    *(int64_t *)(stx + 96)  = (int64_t)kst.st_ctime;
    *(uint32_t *)(stx + 104) = (uint32_t)kst.st_ctime_nsec;
    *(int64_t *)(stx + 112)  = (int64_t)kst.st_mtime;
    *(uint32_t *)(stx + 120) = (uint32_t)kst.st_mtime_nsec;

    u32[32] = (uint32_t)(kst.st_rdev >> 8);
    u32[33] = (uint32_t)(kst.st_rdev & 0xff) | (uint32_t)((kst.st_rdev >> 12) & 0xffffff00);
    u32[34] = (uint32_t)(kst.st_dev >> 8);
    u32[35] = (uint32_t)(kst.st_dev & 0xff) | (uint32_t)((kst.st_dev >> 12) & 0xffffff00);

    if (copy_to_user(buf, stx, 256) < 0) return -EFAULT;
    return 0;
}

int64_t sys_getdents64(int fd, void *dirp, size_t count) {
    int64_t gfd = get_global_fd(fd);
    if (gfd < 0) return gfd;
    if (count > 4096) count = 4096;
    char *kbuf = kmalloc(4096);
    if (!kbuf) return -ENOMEM;
    int64_t n = vfs_getdents64(gfd, kbuf, count);
    if (n > 0) {
        if (copy_to_user(dirp, kbuf, (size_t)n) < 0) { kfree(kbuf); return -EFAULT; }
    }
    kfree(kbuf);
    return n;
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
    uint64_t sb[8];
    memset(sb, 0, sizeof(sb));
    sb[0] = 0x4006;
    sb[1] = 4096;
    sb[2] = 1024*1024;
    sb[3] = 512*1024;
    sb[4] = 512*1024;
    if (copy_to_user(buf, sb, 64) < 0) return -EFAULT;
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
    if (sigsys_diag_count < 128) {
        sigsys_diag_count++;
        kdebug("[SIGSYS] wait4 pid=%d opt=0x%x\n", pid, options);
    }
    int kstatus = 0;
    int ret = proc_wait4(pid, status ? &kstatus : NULL, options);
    if (ret >= 0 && status) {
        if (copy_to_user(status, &kstatus, sizeof(int)) < 0) return -EFAULT;
    }
    return ret;
}

int64_t sys_waitid(int type, int id, void *info, int options, void *rusage) {
    (void)rusage;
    if (sigsys_diag_count < 128) {
        sigsys_diag_count++;
        kdebug("[SIGSYS] waitid type=%d id=%d opt=0x%x\n", type, id, options);
    }
    int pid = -1;
    switch (type) {
    case 0: /* P_ALL */
        pid = -1;
        break;
    case 1: /* P_PID */
        pid = id;
        break;
    case 2: /* P_PGID */
        pid = (id == 0) ? 0 : -id;
        break;
    default:
        return -EINVAL;
    }

    int status = 0;
    int ret = proc_wait4(pid, &status, options & 1 /* WNOHANG */);
    if (ret < 0) return ret;

    if (info) {
        uint8_t si[128];
        memset(si, 0, sizeof(si));
        if (ret > 0) {
            int si_code = CLD_EXITED;
            int si_status = 0;
            if ((status & 0xffff) == 0xffff) {
                si_code = CLD_CONTINUED;
            } else if ((status & 0x7f) == 0x7f) {
                si_code = CLD_STOPPED;
                si_status = (status >> 8);
            } else if (status & 0x7f) {
                si_code = (status & 0x80) ? CLD_DUMPED : CLD_KILLED;
                si_status = status & 0x7f;
            } else {
                si_code = CLD_EXITED;
                si_status = (status >> 8) & 0xff;
            }

            ((int *)si)[0] = SIGCHLD;    /* si_signo */
            ((int *)si)[1] = 0;          /* si_errno */
            ((int *)si)[2] = si_code;    /* si_code */
            ((int *)si)[3] = ret;        /* si_pid */
            ((int *)si)[4] = 0;          /* si_uid */
            ((int *)si)[5] = si_status;  /* si_status */
        }
        if (copy_to_user(info, si, sizeof(si)) < 0) return -EFAULT;
    }
    return 0;
}

int64_t sys_sched_yield(void) {
    proc_yield();
    return 0;
}

int64_t sys_reboot(int cmd) {
    if (cmd == 0x424F4F54) firmware_reboot();
    else firmware_shutdown();
    return 0;
}

int64_t sys_prctl(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (op == 15) {
        task_t *t = proc_current();
        if (t) {
            char name[64];
            if (user_strncpy(name, (const char *)a1, sizeof(name)) >= 0)
                proc_set_name(t, name);
        }
    }
    return 0;
}

int64_t sys_prlimit64(int pid, int resource, void *new_rlim, void *old_rlim) {
    (void)pid;
    if (old_rlim) {
        uint64_t r[2] = {0};
        task_t *t = proc_current();
        switch (resource) {
            case 3: set_uniform_rlimit(r, t ? t->rlim_stack : USER_STACK_MAX_SIZE); break;
            case 7: set_uniform_rlimit(r, t ? t->rlim_nofile : MAX_FILES); break;
            default: r[0] = 0; r[1] = (uint64_t)-1; break;
        }
        if (copy_to_user(old_rlim, r, sizeof(r)) < 0) return -EFAULT;
    }
    if (new_rlim) {
        uint64_t r[2];
        if (copy_from_user(r, new_rlim, sizeof(r)) < 0) return -EFAULT;
        task_t *t = proc_current();
        if (!t) return -ESRCH;
        switch (resource) {
            case 3: t->rlim_stack = clamp_stack_rlimit(r[0], r[1]); break;
            case 7: t->rlim_nofile = clamp_nofile_rlimit(r[0], r[1]); break;
            default: break;
        }
    }
    return 0;
}

int64_t sys_getrlimit(int resource, void *rlim) {
    if (!rlim) return -EFAULT;
    uint64_t r[2] = {0};
    task_t *t = proc_current();
    switch (resource) {
        case 3: set_uniform_rlimit(r, t ? t->rlim_stack : USER_STACK_MAX_SIZE); break;
        case 7: set_uniform_rlimit(r, t ? t->rlim_nofile : MAX_FILES); break;
        default: r[0] = 0; r[1] = (uint64_t)-1; break;
    }
    if (copy_to_user(rlim, r, sizeof(r)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_setrlimit(int resource, void *rlim) {
    if (!rlim) return -EFAULT;
    uint64_t r[2];
    if (copy_from_user(r, rlim, sizeof(r)) < 0) return -EFAULT;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    switch (resource) {
        case 3: t->rlim_stack = clamp_stack_rlimit(r[0], r[1]); break;
        case 7: t->rlim_nofile = clamp_nofile_rlimit(r[0], r[1]); break;
        default: break;
    }
    return 0;
}

int64_t sys_getrusage(int who, void *usage) {
    (void)who;
    if (!usage) return -EFAULT;
    uint64_t u[18]; /* 144 bytes / 8 */
    memset(u, 0, sizeof(u));
    task_t *t = proc_current();
    if (t) {
        u[0] = t->total_time / TICKS_PER_SEC;
        u[1] = (t->total_time % TICKS_PER_SEC) * 1000000000UL / TICKS_PER_SEC / 1000;
    }
    if (copy_to_user(usage, u, 144) < 0) return -EFAULT;
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
    uint64_t new_mask;
    if (copy_from_user(&new_mask, mask, sizeof(new_mask)) < 0) return -EFAULT;

    /* Can't block SIGKILL or SIGSTOP */
    new_mask = signal_mask_from_user(new_mask);
    new_mask &= ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
    if (sigsys_diag_count < 128) {
        sigsys_diag_count++;
        kdebug("[SIGSYS] sigsuspend pid=%d old=0x%lx new=0x%lx pending=0x%lx\n",
              t->pid, (unsigned long)old_blocked, (unsigned long)new_mask,
              (unsigned long)ss->pending);
    }
    t->sigsuspend_old_blocked = old_blocked;
    t->sigsuspend_active = 1;
    ss->blocked = new_mask;

    uint64_t deliverable = ss->pending & ~ss->blocked;
    if (!deliverable) {
        t->state = PROC_BLOCKED;
        sched();
    }
    return -EINTR;
}

int64_t sys_sigtimedwait(const uint64_t *set, void *info, const void *timeout) {
    task_t *t = proc_current();
    if (!t || !t->signals || !set) return -EINVAL;

    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t mask;
    if (copy_from_user(&mask, set, sizeof(mask)) < 0) return -EFAULT;
    mask = signal_mask_from_user(mask);
    uint64_t deliverable = ss->pending & ~ss->blocked & mask;

    if (!deliverable) {
        if (timeout) {
            uint64_t to[2];
            if (copy_from_user(to, timeout, sizeof(to)) < 0) return -EFAULT;
            uint64_t sec = to[0];
            uint64_t nsec = to[1];
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
                uint8_t infobuf[128];
                memset(infobuf, 0, 128);
                *(int *)infobuf = sig;
                copy_to_user(info, infobuf, 128);
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
    uint64_t ret = proc_mmap(addr, len, prot, flags, fd, off);
    // 如果 proc_mmap 返回了错误（例如封装 mm_mmap 的错误码）
    // 确保它能正确映射到用户态的 MAP_FAILED
    return (int64_t)ret;
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

int64_t sys_clock_settime(int clk, void *tp) {
    uint64_t ts[2];
    if (clk != CLOCK_REALTIME_ID) return -EINVAL;
    if (!tp) return -EFAULT;
    if (copy_from_user(ts, tp, sizeof(ts)) < 0) return -EFAULT;
    if (ts[1] >= 1000000000ULL) return -EINVAL;
    return timekeeping_set_realtime(ts[0], ts[1]);
}

int64_t sys_clock_gettime(int clk, void *tp) {
    uint64_t ts[2];
    if (!tp) return -EFAULT;
    /* glibc prefers CLOCK_REALTIME_COARSE for date/time formatting. */
    if (clock_is_realtime(clk)) timekeeping_get_realtime(ts);
    else if (clock_is_monotonic(clk)) timekeeping_get_monotonic(ts);
    else return -EINVAL;
    if (copy_to_user(tp, ts, sizeof(ts)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_clock_getres(int clk, void *tp) {
    if (!clock_is_realtime(clk) && !clock_is_monotonic(clk))
        return -EINVAL;
    if (tp) {
        uint64_t ts[2];
        ts[0] = 0;
        ts[1] = 1000000000UL / TICKS_PER_SEC;
        if (copy_to_user(tp, ts, sizeof(ts)) < 0) return -EFAULT;
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
        if (sleep_diag_count < 128) {
            sleep_diag_count++;
            kdebug("[SLEEPDBG] enter pid=%d ticks=%lu until=%lu now=%lu\n",
                  t->pid, (unsigned long)ticks, (unsigned long)until,
                  (unsigned long)timer_get_ticks());
        }
        t->wake_time = until;
        t->state     = PROC_BLOCKED;
        sched();
        if (sleep_diag_count < 128) {
            sleep_diag_count++;
            kdebug("[SLEEPDBG] ret pid=%d now=%lu wake=%lu state=%d\n",
                  t->pid, (unsigned long)timer_get_ticks(),
                  (unsigned long)t->wake_time, t->state);
        }
    } else {
        while (timer_get_ticks() < until) __asm__ volatile("nop");
    }
    return 0;
}

int64_t sys_gettimeofday(void *tv, void *tz) {
    (void)tz;
    if (tv) {
        uint64_t t[2];
        uint64_t ts[2];
        timekeeping_get_realtime(ts);
        t[0] = ts[0];
        t[1] = ts[1] / 1000ULL;
        if (copy_to_user(tv, t, sizeof(t)) < 0) return -EFAULT;
    }
    return 0;
}

int64_t sys_settimeofday(void *tv, void *tz) {
    uint64_t t[2];
    (void)tz;
    if (!tv) return -EINVAL;
    if (copy_from_user(t, tv, sizeof(t)) < 0) return -EFAULT;
    if (t[1] >= 1000000ULL) return -EINVAL;
    return timekeeping_set_realtime(t[0], t[1] * 1000ULL);
}

int64_t sys_times(void *buf) {
    task_t *t = proc_current();
    if (t && buf) {
        uint64_t tm[4];
        memset(tm, 0, sizeof(tm));
        tm[0] = t->total_time;
        if (copy_to_user(buf, tm, sizeof(tm)) < 0) return -EFAULT;
    }
    return (int64_t)(timer_get_ticks());
}

int64_t sys_time(long *tloc) {
    uint64_t ts[2];
    uint64_t t;
    timekeeping_get_realtime(ts);
    t = ts[0];
    if (tloc) {
        long tl = (long)t;
        if (copy_to_user(tloc, &tl, sizeof(long)) < 0) return -EFAULT;
    }
    return (int64_t)t;
}

/* ============================================================
 * System info
 * ============================================================ */

int64_t sys_uname(void *buf) {
    struct uname { char s[65],n[65],r[65],v[65],m[65],d[65]; };
    if (!buf) return -EFAULT;
    struct uname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.s, "Linux");
    strcpy(u.n, "A20OS");
    strcpy(u.r, "6.1.0-A20OS");
    strcpy(u.v, "#1 SMP A20OS 2025");
    strcpy(u.m, ARCH_NAME);
    if (copy_to_user(buf, &u, sizeof(u)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_sysinfo(void *info) {
    if (!info) return -EFAULT;
    uint64_t si[14]; /* 112 bytes */
    memset(si, 0, sizeof(si));
    si[0] = timer_get_ticks() / TICKS_PER_SEC;
    si[1] = 1;
    if (copy_to_user(info, si, sizeof(si)) < 0) return -EFAULT;
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
    uint8_t tmp[128];
    uint64_t seed = timer_get_ticks();
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done > sizeof(tmp) ? sizeof(tmp) : len - done;
        for (size_t i = 0; i < chunk; i++) {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            tmp[i] = (uint8_t)(seed & 0xFF);
        }
        if (copy_to_user((char*)buf + done, tmp, chunk) < 0) return -EFAULT;
        done += chunk;
    }
    return (int64_t)len;
}

int64_t sys_futex(int *uaddr, int op, int val, void *timeout, int *uaddr2, int val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    if (!uaddr) return -EFAULT;
    int opc = op & 0x7F;
    if (opc == 0 || opc == 9) {
        int uval;
        if (copy_from_user(&uval, uaddr, sizeof(int)) < 0) return -EFAULT;
        if (uval != val) return -EAGAIN;
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

/* Read user fd_set bit without direct pointer dereference */
static int fd_isset_user(int f, void *s) {
    long mask = 0;
    if (copy_from_user(&mask, &((long *)s)[f / 8 / sizeof(long)], sizeof(long)) < 0) return 0;
    return (mask & (1UL << (f % (8 * sizeof(long))))) != 0;
}

int64_t sys_select(int nfds, void *readfds, void *writefds,
                   void *exceptfds, void *timeout) {
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    (void)exceptfds;

    int ready_count = 0;

    if (readfds) {
        for (int i = 0; i < nfds; i++) {
            if (fd_isset_user(i, readfds)) {
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
            if (fd_isset_user(i, writefds)) {
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
        uint64_t tv[2];
        if (copy_from_user(tv, timeout, sizeof(tv)) == 0) {
            if (tv[0] == 0 && tv[1] == 0) return 0;
        }
    }

    proc_yield();
    return 0;
}
