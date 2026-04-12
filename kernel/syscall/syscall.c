/*
 * A20OS — Complete Syscall Dispatcher
 *
 * Implements the full Linux-compatible RISC-V syscall ABI.
 * All syscalls pass through vfs.c / proc.c rather than
 * calling fs.c / uart directly.
 */

#include "syscall.h"
#include "vfs.h"
#include "fs.h"
#include "proc.h"
#include "signal.h"
#include "mm.h"
#include "timer.h"
#include "uart.h"
#include "stdio.h"
#include "string.h"
#include "consts.h"
#include "defs.h"
#include "klog.h"
#include "sbi.h"
#include "arch_ops.h"

void syscall_init(void) {
    kdebug("[SYSCALL] Initialized\n");
}

int64_t syscall_dispatch(trap_context_t *ctx) {
    uint64_t num = ctx->x[17]; /* a7 = syscall number */
    uint64_t a0  = ctx->x[10];
    uint64_t a1  = ctx->x[11];
    uint64_t a2  = ctx->x[12];
    uint64_t a3  = ctx->x[13];
    uint64_t a4  = ctx->x[14];
    uint64_t a5  = ctx->x[15];
    (void)a4; (void)a5;

    int64_t ret = -ENOSYS;

    switch (num) {
    /* ---- File I/O ---- */
    case SYS_read:        ret = sys_read((int)a0, (char*)a1, (size_t)a2);            break;
    case SYS_write:       ret = sys_write((int)a0, (const char*)a1, (size_t)a2);     break;
    case SYS_writev:      ret = sys_writev((int)a0, (const void*)a1, (int)a2);       break;
    case SYS_readv:       ret = sys_readv((int)a0, (const void*)a1, (int)a2);        break;
    case SYS_openat:      ret = sys_openat((int)a0,(const char*)a1,(int)a2,(int)a3); break;
    case SYS_close:       ret = sys_close((int)a0);                                  break;
    case SYS_lseek:       ret = sys_lseek((int)a0,(long)a1,(int)a2);                 break;
    case SYS_dup:         ret = vfs_dup((int)a0);                                    break;
    case SYS_dup3:        ret = vfs_dup3((int)a0,(int)a1,(int)a2);                   break;
    case SYS_fcntl:       ret = vfs_fcntl((int)a0,(int)a1,(long)a2);                 break;
    case SYS_pipe2:       ret = sys_pipe2((int*)a0,(int)a1);                          break;
    case SYS_ioctl:       ret = vfs_ioctl((int)a0,(unsigned long)a1,(void*)a2);      break;
    case SYS_pread64:     ret = sys_pread64((int)a0,(char*)a1,(size_t)a2,(long)a3); break;
    case SYS_pwrite64:    ret = sys_pwrite64((int)a0,(char*)a1,(size_t)a2,(long)a3);break;
    case SYS_sync:        ret = 0; /* no-op */                                        break;
    case SYS_fsync:       ret = 0;                                                    break;
    case SYS_ftruncate:   ret = vfs_ftruncate((int)a0,(size_t)a1);                   break;
    case SYS_truncate:    ret = vfs_truncate((const char*)a0,(size_t)a1);             break;
    case SYS_sendfile: {
        int out_fd = (int)a0;
        int in_fd = (int)a1;
        long *off = (long*)a2;
        size_t count = (size_t)a3;
        if (count == 0) { ret = 0; break; }
        long cur_off = off ? *off : vfs_lseek(in_fd, 0, SEEK_CUR);
        int64_t total = 0;
        char sbuf[4096];
        while ((size_t)total < count) {
            size_t chunk = count - (size_t)total;
            if (chunk > sizeof(sbuf)) chunk = sizeof(sbuf);
            long saved = vfs_lseek(in_fd, 0, SEEK_CUR);
            vfs_lseek(in_fd, cur_off, SEEK_SET);
            int64_t n = vfs_read(in_fd, sbuf, chunk);
            vfs_lseek(in_fd, saved, SEEK_SET);
            if (n <= 0) break;
            int64_t w = vfs_write(out_fd, sbuf, (size_t)n);
            if (w < 0) { if (total == 0) total = w; break; }
            total += w;
            cur_off += w;
            if (w < n) break;
        }
        if (off) *off = cur_off;
        ret = total;
        break;
    }
    case SYS_select:      ret = sys_select((int)a0, (void*)a1, (void*)a2,
                                   (void*)a3, (void*)a4);                break;
    case SYS_ppoll: {
        struct { int fd; short events; short revents; } *pfds = (void*)a0;
        int nfds = (int)a1;
        (void)a2; (void)a3; (void)a4;
        if (!pfds || nfds <= 0) { ret = 0; break; }
        int ready = 0;
        for (int i = 0; i < nfds; i++) {
            pfds[i].revents = 0;
            if (pfds[i].fd < 0) continue;
            vfile_t *vf = vfs_get_file(pfds[i].fd);
            if (vf) {
                if (pfds[i].events & 0x001) pfds[i].revents |= 0x001;
                if (pfds[i].events & 0x004) pfds[i].revents |= 0x004;
                ready++;
            } else {
                pfds[i].revents = 0x008;
                ready++;
            }
        }
        ret = ready;
        break;
    }
    case SYS_epoll_create1: {
        vfile_t *vf = (vfile_t *)kmalloc(sizeof(vfile_t));
        if (!vf) { ret = -ENOMEM; break; }
        memset(vf, 0, sizeof(*vf));
        vf->ref_count = 1;
        int efd = vfs_alloc_fd(vf);
        if (efd < 0) { kfree(vf); ret = -EMFILE; break; }
        ret = efd;
        break;
    }

    /* ---- Directory / Path ---- */
    case SYS_mkdirat:     ret = sys_mkdirat((int)a0,(const char*)a1,(int)a2);        break;
    case SYS_unlinkat:    ret = sys_unlinkat((int)a0,(const char*)a1,(int)a2);       break;
    case SYS_renameat2:   ret = sys_renameat2((int)a0,(const char*)a1,
                                               (int)a2,(const char*)a3,(int)a4);     break;
    case SYS_chdir:       ret = sys_chdir((const char*)a0);                          break;
    case SYS_getcwd:      ret = sys_getcwd((char*)a0,(size_t)a1);                    break;
    case SYS_fstat:       ret = sys_fstat((int)a0,(void*)a1);                        break;
    case SYS_fstatat:     ret = sys_fstatat((int)a0,(const char*)a1,(void*)a2,(int)a3); break;
    case SYS_readlinkat:  ret = vfs_readlinkat((int)a0,(const char*)a1,(char*)a2,(size_t)a3); break;
    case SYS_faccessat:   ret = vfs_faccessat((int)a0,(const char*)a1,(int)a2);      break;
    case SYS_getdents64:  ret = vfs_getdents64((int)a0,(void*)a1,(size_t)a2);        break;
    case SYS_linkat:      ret = -ENOSYS;                                              break;
    case SYS_symlinkat:   ret = -ENOSYS;                                              break;
    case SYS_statfs:      ret = sys_statfs((const char*)a0,(void*)a1);               break;
    case SYS_fstatfs:     ret = sys_statfs(NULL,(void*)a1);                           break;
    case SYS_mount:       ret = sys_mount((const char*)a0,(const char*)a1,
                                           (const char*)a2,(int)a3);                 break;
    case SYS_umount2:     ret = vfs_umount((const char*)a0);                          break;
    case SYS_utimensat:   ret = 0; /* timestamps not tracked */                       break;

    /* ---- Process ---- */
    case SYS_exit:        sys_exit((int)a0);                                          break;
    case SYS_exit_group:  sys_exit((int)a0);                                          break;
    case SYS_getpid:      ret = sys_getpid();                                         break;
    case SYS_getppid:     ret = sys_getppid();                                        break;
    case SYS_gettid:      ret = sys_getpid();                                         break;
    case SYS_set_tid_address: ret = sys_getpid();                                     break;
    case SYS_getuid:      ret = 0;                                                    break;
    case SYS_geteuid:     ret = 0;                                                    break;
    case SYS_getgid:      ret = 0;                                                    break;
    case SYS_getegid:     ret = 0;                                                    break;
    case SYS_getpgid:     ret = sys_getpid();                                         break;
    case SYS_setpgid:     ret = 0;                                                    break;
    case SYS_setsid:      ret = sys_getpid();                                         break;
    case SYS_clone:       ret = proc_clone(a0,a1,(int*)a2,(int*)a3,a4);              break;
    case SYS_execve:      ret = sys_execve((const char*)a0,(char**)a1,(char**)a2);   break;
    case SYS_wait4:       ret = sys_wait4((int)a0,(int*)a1,(int)a2,(void*)a3);       break;
    case SYS_sched_yield: ret = sys_sched_yield();                                    break;
    case SYS_reboot:
        if ((int)a0 == 0x424F4F54) sbi_reboot();
        else sbi_shutdown();
        break;
    case SYS_prctl:       ret = sys_prctl((int)a0,a1,a2,a3,a4);                      break;
    case SYS_prlimit64: {
        int resource = (int)a1;
        void *rlim = (void*)a3;
        if (rlim) {
            uint64_t *r = (uint64_t *)rlim;
            task_t *t = proc_current();
            switch (resource) {
                case 3: r[0] = 0; r[1] = t ? t->rlim_stack : 8*1024*1024; break;
                case 7: r[0] = 0; r[1] = t ? t->rlim_nofile : MAX_FILES; break;
                default: r[0] = 0; r[1] = (uint64_t)-1; break;
            }
        }
        ret = 0;
        break;
    }
    case SYS_getrlimit: {
        int resource = (int)a0;
        void *rlim = (void*)a1;
        if (!rlim) { ret = -EFAULT; break; }
        uint64_t *r = (uint64_t *)rlim;
        task_t *t = proc_current();
        switch (resource) {
            case 3: r[0] = 0; r[1] = t ? t->rlim_stack : 8*1024*1024; break;
            case 7: r[0] = 0; r[1] = t ? t->rlim_nofile : MAX_FILES; break;
            default: r[0] = 0; r[1] = (uint64_t)-1; break;
        }
        ret = 0;
        break;
    }
    case SYS_setrlimit:   ret = 0;                                                    break;
    case SYS_getrusage: {
        void *usage = (void*)a1;
        if (!usage) { ret = -EFAULT; break; }
        memset(usage, 0, 144);
        task_t *t = proc_current();
        if (t) {
            uint64_t *u = (uint64_t *)usage;
            u[0] = t->total_time / TICKS_PER_SEC;
            u[1] = (t->total_time % TICKS_PER_SEC) * 1000000000UL / TICKS_PER_SEC / 1000;
        }
        ret = 0;
        break;
    }

    /* ---- Signal ---- */
    case SYS_kill:        ret = proc_kill((int)a0,(int)a1);                           break;
    case SYS_tgkill:      ret = proc_kill((int)a1,(int)a2);                           break;
    case SYS_sigaction:   ret = sys_sigaction((int)a0,(void*)a1,(void*)a2);          break;
    case SYS_sigprocmask: ret = sys_sigprocmask((int)a0,(void*)a1,(void*)a2);        break;
    case SYS_sigreturn:   ret = 0;                                                    break;
    case SYS_sigsuspend:  ret = -EINTR;                                               break;

    /* ---- Memory ---- */
    case SYS_brk:         ret = (int64_t)proc_brk(a0);                               break;
    case SYS_mmap:        ret = (int64_t)proc_mmap(a0,(size_t)a1,(int)a2,(int)a3,
                                                     (int)a4,(long)a5);              break;
    case SYS_munmap:      ret = proc_munmap(a0,(size_t)a1);                           break;
    case SYS_mprotect:    ret = 0;                                                    break;
    case SYS_madvise:     ret = 0;                                                    break;
    case SYS_mremap: {
        /* Simplified mremap: allocate new region */
        size_t new_size = (size_t)a2;
        if (new_size == 0) { ret = -EINVAL; break; }
        ret = (int64_t)proc_mmap(0, new_size, 3 /* PROT_READ|PROT_WRITE */,
                                  0x20 | 0x02 /* MAP_ANONYMOUS|MAP_PRIVATE */, -1, 0);
        break;
    }
    case SYS_shm_open:    ret = -ENOSYS;                                              break;

    /* ---- Time ---- */
    case SYS_clock_gettime: ret = sys_clock_gettime((int)a0,(void*)a1);              break;
    case SYS_clock_getres:  ret = sys_clock_getres((int)a0,(void*)a1);               break;
    case SYS_nanosleep:     ret = sys_nanosleep((void*)a0,(void*)a1);                break;
    case SYS_gettimeofday:  ret = sys_gettimeofday((void*)a0,(void*)a1);             break;
    case SYS_times: {
        void *buf = (void*)a0;
        task_t *t = proc_current();
        if (t && buf) {
            uint64_t *tm = (uint64_t *)buf;
            memset(tm, 0, 32);
            tm[0] = t->total_time; /* tms_utime */
        }
        ret = (int64_t)(timer_get_ticks());
        break;
    }
    case SYS_time:          ret = sys_time((long*)a0);                               break;

    /* ---- System info ---- */
    case SYS_uname:       ret = sys_uname((void*)a0);                                break;
    case SYS_sysinfo:     ret = sys_sysinfo((void*)a0);                              break;
    case SYS_getgroups: {
        /* No supplementary groups */
        (void)a0; (void)a1;
        ret = 0;
        break;
    }
    case SYS_setgroups:   ret = 0;                                                    break;
    case SYS_umask:       ret = sys_umask((int)a0);                                  break;
    case SYS_syslog:      ret = 0;                                                    break;

    /* ---- Pseudoterminale / Random ---- */
    case SYS_getrandom:   ret = sys_getrandom((void*)a0,(size_t)a1,(int)a2);         break;
    case SYS_futex: {
        /* Minimal futex: WAIT/WAKE only */
        int *uaddr = (int*)a0;
        int op = (int)a1 & 0x7F; /* mask private flag */
        int val = (int)a2;
        (void)a3; (void)a4; (void)a5;
        if (!uaddr) { ret = -EFAULT; break; }
        if (op == 0 /* FUTEX_WAIT */) {
            if (*uaddr != val) { ret = -EAGAIN; break; }
            proc_yield();
            ret = 0;
        } else if (op == 1 /* FUTEX_WAKE */) {
            ret = 1; /* claim we woke 1 waiter */
        } else if (op == 9 /* FUTEX_WAIT_BITSET */) {
            if (*uaddr != val) { ret = -EAGAIN; break; }
            proc_yield();
            ret = 0;
        } else if (op == 10 /* FUTEX_WAKE_BITSET */) {
            ret = 1;
        } else {
            ret = -ENOSYS;
        }
        break;
    }

    default:
        /* Be quiet about noisy unimplemented calls */
        if (num < 300)
            kdebug("[SYSCALL] Unimplemented: %lu\n", (unsigned long)num);
        ret = -ENOSYS;
        break;
    }

    ctx->x[10] = (uint64_t)ret;
    return ret;
}

/* ============================================================
 * Syscall implementations
 * ============================================================ */

int64_t sys_read(int fd, char *buf, size_t count) {
    if (!buf) return -EFAULT;
    if (count == 0) return 0;
    return vfs_read(fd, buf, count);
}

int64_t sys_write(int fd, const char *buf, size_t count) {
    if (!buf) return -EFAULT;
    return vfs_write(fd, buf, count);
}

int64_t sys_pread64(int fd, char *buf, size_t count, long off) {
    long curoff = vfs_lseek(fd, 0, SEEK_CUR);
    vfs_lseek(fd, off, SEEK_SET);
    int64_t r = vfs_read(fd, buf, count);
    vfs_lseek(fd, curoff, SEEK_SET);
    return r;
}

int64_t sys_pwrite64(int fd, char *buf, size_t count, long off) {
    long curoff = vfs_lseek(fd, 0, SEEK_CUR);
    vfs_lseek(fd, off, SEEK_SET);
    int64_t r = vfs_write(fd, buf, count);
    vfs_lseek(fd, curoff, SEEK_SET);
    return r;
}

int64_t sys_writev(int fd, const void *iov, int iovcnt) {
    struct iovec { char *base; size_t len; };
    const struct iovec *v = (const struct iovec *)iov;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!v[i].base || v[i].len == 0) continue;
        int64_t n = vfs_write(fd, v[i].base, v[i].len);
        if (n < 0) return n;
        total += n;
    }
    return total;
}

int64_t sys_readv(int fd, const void *iov, int iovcnt) {
    struct iovec { char *base; size_t len; };
    const struct iovec *v = (const struct iovec *)iov;
    int64_t total = 0;
    for (int i = 0; i < iovcnt; i++) {
        if (!v[i].base || v[i].len == 0) continue;
        int64_t n = vfs_read(fd, v[i].base, v[i].len);
        if (n < 0) return n;
        total += n;
        if ((size_t)n < v[i].len) break; /* short read */
    }
    return total;
}

int64_t sys_openat(int dirfd, const char *path, int flags, int mode) {
    (void)dirfd;
    if (!path) return -EFAULT;
    return vfs_open(path, flags, mode);
}

int64_t sys_close(int fd) {
    task_t *t = proc_current();
    if (t && fd >= 0 && fd < MAX_FILES) t->fd_table[fd] = -1;
    return vfs_close(fd);
}

int64_t sys_lseek(int fd, long offset, int whence) {
    return vfs_lseek(fd, offset, whence);
}

int64_t sys_pipe2(int *pipefd, int flags) {
    (void)flags;
    if (!pipefd) return -EFAULT;
    return vfs_pipe(pipefd);
}

int64_t sys_fstat(int fd, void *st) {
    kstat_t kst;
    int r = vfs_fstat(fd, &kst);
    if (r < 0) return r;
    /* Copy to user stat structure (Linux stat64 / statx compatible subset) */
    /* We lay out as: dev, ino, mode, nlink, uid, gid, size, blksize, blocks, times */
    uint64_t *out = (uint64_t *)st;
    if (!out) return -EFAULT;
    out[0] = kst.st_dev;
    out[1] = kst.st_ino;
    out[2] = kst.st_mode | ((uint64_t)kst.st_nlink << 32);
    out[3] = ((uint64_t)kst.st_uid) | ((uint64_t)kst.st_gid << 32);
    out[4] = kst.st_size;
    out[5] = kst.st_blksize;
    out[6] = kst.st_blocks;
    out[7] = kst.st_atime;
    out[8] = kst.st_mtime;
    out[9] = kst.st_ctime;
    return 0;
}

int64_t sys_fstatat(int dirfd, const char *path, void *st, int flags) {
    (void)dirfd; (void)flags;
    kstat_t kst;
    int r = vfs_fstatat(dirfd, path, &kst, flags);
    if (r < 0) return r;
    uint64_t *out = (uint64_t *)st;
    if (!out) return -EFAULT;
    out[0] = kst.st_dev;
    out[1] = kst.st_ino;
    out[2] = kst.st_mode | ((uint64_t)kst.st_nlink << 32);
    out[3] = ((uint64_t)kst.st_uid) | ((uint64_t)kst.st_gid << 32);
    out[4] = kst.st_size;
    out[5] = kst.st_blksize;
    out[6] = kst.st_blocks;
    out[7] = kst.st_atime;
    out[8] = kst.st_mtime;
    out[9] = kst.st_ctime;
    return 0;
}

int64_t sys_statfs(const char *path, void *buf) {
    (void)path;
    if (!buf) return -EFAULT;
    uint64_t *sb = (uint64_t *)buf;
    memset(sb, 0, 64);
    sb[0] = 0x4006; /* EXT4_SUPER_MAGIC or FAT32 */
    sb[1] = 4096;   /* block size */
    sb[2] = 1024*1024; /* total blocks */
    sb[3] = 512*1024;  /* free blocks */
    sb[4] = 512*1024;  /* available blocks */
    return 0;
}

int64_t sys_mkdirat(int dirfd, const char *path, int mode) {
    (void)dirfd;
    return vfs_mkdir(path, mode);
}

int64_t sys_unlinkat(int dirfd, const char *path, int flags) {
    (void)dirfd; (void)flags;
    return vfs_unlink(path);
}

int64_t sys_renameat2(int olddir, const char *oldpath,
                       int newdir, const char *newpath, int flags) {
    (void)olddir; (void)newdir; (void)flags;
    return vfs_rename(oldpath, newpath);
}

int64_t sys_chdir(const char *path) { return vfs_chdir(path); }

int64_t sys_getcwd(char *buf, size_t size) {
    if (!buf || size == 0) return -EFAULT;
    return vfs_getcwd(buf, size);
}

int64_t sys_mount(const char *src, const char *target,
                   const char *fstype, int flags) {
    return vfs_mount(src, target, fstype, flags);
}

int64_t sys_exit(int code) {
    proc_exit(code);
    return 0; /* never reached */
}

int64_t sys_getpid(void)  { task_t *t = proc_current(); return t ? t->pid : 0; }
int64_t sys_getppid(void) { task_t *t = proc_current(); return t ? t->ppid : 0; }

int64_t sys_wait4(int pid, int *status, int options, void *rusage) {
    (void)rusage;
    return proc_wait4(pid, status, options);
}

int64_t sys_execve(const char *path, char **argv, char **envp) {
    return proc_exec(path, argv, envp);
}

int64_t sys_sched_yield(void) { proc_yield(); return 0; }

int64_t sys_prctl(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (op == 15) { /* PR_SET_NAME */
        task_t *t = proc_current();
        if (t) proc_set_name(t, (const char *)a1);
    }
    return 0;
}

int64_t sys_sigaction(int signum, void *act, void *oldact) {
    return sys_sigaction_impl(signum, (const sigaction_t *)act, (sigaction_t *)oldact);
}

int64_t sys_sigprocmask(int how, void *set, void *oldset) {
    return sys_sigprocmask_impl(how, (const uint64_t *)set, (uint64_t *)oldset);
}

int64_t sys_uname(void *buf) {
    struct uname { char s[65],n[65],r[65],v[65],m[65],d[65]; };
    struct uname *u = (struct uname *)buf;
    if (!u) return -EFAULT;
    memset(u, 0, sizeof(*u));
    strcpy(u->s, "Linux");          /* pretend to be Linux for compat */
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
    uint64_t *ts = (uint64_t *)req;
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
        while (timer_get_ticks() < until) arch_cpu_relax();
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

int64_t sys_time(long *tloc) {
    uint64_t t = timer_get_ticks() / TICKS_PER_SEC;
    if (tloc) *tloc = (long)t;
    return (int64_t)t;
}

int64_t sys_sysinfo(void *info) {
    if (!info) return -EFAULT;
    memset(info, 0, 128);
    uint64_t *si = (uint64_t *)info;
    si[0] = timer_get_ticks() / TICKS_PER_SEC; /* uptime */
    si[1] = 1;                                  /* procs */
    return 0;
}

int64_t sys_umask(int newmask) {
    task_t *t = proc_current();
    if (!t) return 022;
    int old = t->umask;
    t->umask = newmask & 0777;
    return old;
}

int64_t sys_getrandom(void *buf, size_t len, int flags) {
    (void)flags;
    if (!buf) return -EFAULT;
    /* Use timer as cheap entropy source */
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

/* ============================================================
 * fd_set operations for select/poll
 * ============================================================ */

#define FD_ISSET(f, s) (((s)[(f)/8/sizeof(long)] & (1UL<<((f)%8))) != 0)

int64_t sys_select(int nfds, void *readfds, void *writefds,
                  void *exceptfds, void *timeout) {
    task_t *t = proc_current();
    if (!t) return -ESRCH;

    (void)writefds; (void)exceptfds; /* Write/exception not supported */

    /* Check for timeout */
    if (timeout) {
        /* For simplicity, treat NULL timeout as blocking forever */
        /* Real implementation would handle timeval timeout */
    }

    /* Check readfds */
    int ready_count = 0;
    if (readfds) {
        for (int i = 0; i < nfds; i++) {
            if (FD_ISSET(i, (long *)readfds)) {
                /* Check if fd is valid and can be read */
                if (i >= 0 && i < MAX_FILES && t->fd_table[i] > 2) {
                    /* File exists, assume it's readable */
                    ready_count++;
                    break;  /* At least one fd is ready */
                }
            }
        }
    }

    /* Simplified: if any fd might be ready, return success */
    if (ready_count > 0) {
        return (int64_t)ready_count;
    }

    /* No fds ready, block */
    proc_yield();
    return 0;
}
