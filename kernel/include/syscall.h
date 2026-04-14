#ifndef _SYSCALL_H
#define _SYSCALL_H

#include "types.h"
#include "trap.h"
#include "signal.h"

void syscall_init(void);
int64_t syscall_dispatch(trap_context_t *ctx);

int64_t sys_read(int fd, char *buf, size_t count);
int64_t sys_write(int fd, const char *buf, size_t count);
int64_t sys_writev(int fd, const void *iov, int iovcnt);
int64_t sys_readv(int fd, const void *iov, int iovcnt);
int64_t sys_pread64(int fd, char *buf, size_t count, long off);
int64_t sys_pwrite64(int fd, char *buf, size_t count, long off);
int64_t sys_openat(int dirfd, const char *path, int flags, int mode);
int64_t sys_close(int fd);
int64_t sys_lseek(int fd, long offset, int whence);
int64_t sys_dup(int fd);
int64_t sys_dup3(int oldfd, int newfd, int flags);
int64_t sys_fcntl(int fd, int cmd, long arg);
int64_t sys_pipe2(int *pipefd, int flags);
int64_t sys_ioctl(int fd, unsigned long req, void *arg);
int64_t sys_sync(void);
int64_t sys_fsync(int fd);
int64_t sys_ftruncate(int fd, size_t size);
int64_t sys_truncate(const char *path, size_t size);
int64_t sys_sendfile(int out_fd, int in_fd, long *off, size_t count);
int64_t sys_select(int nfds, void *readfds, void *writefds,
                   void *exceptfds, void *timeout);
int64_t sys_ppoll(void *fds, int nfds, void *tmo, void *sigmask);
int64_t sys_epoll_create1(int flags);

int64_t sys_mkdirat(int dirfd, const char *path, int mode);
int64_t sys_unlinkat(int dirfd, const char *path, int flags);
int64_t sys_renameat2(int olddir, const char *oldpath,
                       int newdir, const char *newpath, int flags);
int64_t sys_chdir(const char *path);
int64_t sys_getcwd(char *buf, size_t size);
int64_t sys_fstat(int fd, void *st);
int64_t sys_fstatat(int dirfd, const char *path, void *st, int flags);
int64_t sys_readlinkat(int dirfd, const char *path, char *buf, size_t sz);
int64_t sys_faccessat(int dirfd, const char *path, int mode);
int64_t sys_getdents64(int fd, void *dirp, size_t count);
int64_t sys_linkat(int olddirfd, const char *oldpath,
                    int newdirfd, const char *newpath, int flags);
int64_t sys_symlinkat(const char *target, int newdirfd, const char *linkpath);
int64_t sys_statfs(const char *path, void *buf);
int64_t sys_fstatfs(int fd, void *buf);
int64_t sys_mount(const char *src, const char *target,
                   const char *fstype, int flags);
int64_t sys_umount2(const char *target, int flags);
int64_t sys_utimensat(int dirfd, const char *path, void *times, int flags);

int64_t sys_exit(int code);
int64_t sys_exit_group(int code);
int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_gettid(void);
int64_t sys_set_tid_address(int *tidptr);
int64_t sys_set_robust_list(void *head, size_t len);
int64_t sys_getuid(void);
int64_t sys_geteuid(void);
int64_t sys_getgid(void);
int64_t sys_getegid(void);
int64_t sys_getpgid(int pid);
int64_t sys_setpgid(int pid, int pgid);
int64_t sys_setsid(void);
int64_t sys_clone(uint64_t flags, void *stack, int *ptid, int *ctid, uint64_t tls);
int64_t sys_execve(const char *path, char **argv, char **envp);
int64_t sys_wait4(int pid, int *status, int options, void *rusage);
int64_t sys_sched_yield(void);
int64_t sys_reboot(int cmd);
int64_t sys_prctl(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_prlimit64(int pid, int resource, void *new_rlim, void *old_rlim);
int64_t sys_getrlimit(int resource, void *rlim);
int64_t sys_setrlimit(int resource, void *rlim);
int64_t sys_getrusage(int who, void *usage);

int64_t sys_kill(int pid, int sig);
int64_t sys_tgkill(int tgid, int tid, int sig);
int64_t sys_sigaction(int signum, void *act, void *oldact);
int64_t sys_sigprocmask(int how, void *set, void *oldset);
int64_t sys_sigreturn(void);
int64_t sys_sigsuspend(void *mask);

int64_t sys_brk(uint64_t addr);
int64_t sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off);
int64_t sys_munmap(uint64_t addr, size_t len);
int64_t sys_mprotect(uint64_t addr, size_t len, int prot);
int64_t sys_madvise(uint64_t addr, size_t len, int advice);
int64_t sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size,
                    int flags, uint64_t new_addr);
int64_t sys_shm_open(const char *name, int oflag, int mode);

int64_t sys_clock_gettime(int clk, void *tp);
int64_t sys_clock_getres(int clk, void *tp);
int64_t sys_nanosleep(void *req, void *rem);
int64_t sys_gettimeofday(void *tv, void *tz);
int64_t sys_times(void *buf);
int64_t sys_time(long *tloc);

int64_t sys_uname(void *buf);
int64_t sys_sysinfo(void *info);
int64_t sys_getgroups(int size, int *list);
int64_t sys_setgroups(size_t size, const int *list);
int64_t sys_umask(int mask);
int64_t sys_syslog(int type, char *buf, int len);

int64_t sys_getrandom(void *buf, size_t len, int flags);
int64_t sys_futex(int *uaddr, int op, int val, void *timeout,
                   int *uaddr2, int val3);

#endif /* _SYSCALL_H */
