#ifndef _SYSCALL_H
#define _SYSCALL_H

#include "types.h"
#include "trap.h"

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
int64_t sys_pipe2(int pipefd[2], int flags);
int64_t sys_fstat(int fd, void *st);
int64_t sys_fstatat(int dirfd, const char *path, void *st, int flags);
int64_t sys_statfs(const char *path, void *buf);
int64_t sys_mkdirat(int dirfd, const char *path, int mode);
int64_t sys_unlinkat(int dirfd, const char *path, int flags);
int64_t sys_renameat2(int olddir, const char *oldpath, int newdir, const char *newpath, int flags);
int64_t sys_chdir(const char *path);
int64_t sys_getcwd(char *buf, size_t size);
int64_t sys_mount(const char *src, const char *target, const char *fstype, int flags);
int64_t sys_exit(int code);
int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_clone(uint64_t flags, void *stack, int ptid, int ctid, uint64_t tls);
int64_t sys_execve(const char *path, char **argv, char **envp);
int64_t sys_wait4(int pid, int *status, int options, void *rusage);
int64_t sys_sched_yield(void);
int64_t sys_prctl(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_sigaction(int signum, void *act, void *oldact);
int64_t sys_sigprocmask(int how, void *set, void *oldset);
int64_t sys_uname(void *buf);
int64_t sys_clock_gettime(int clk, void *tp);
int64_t sys_clock_getres(int clk, void *tp);
int64_t sys_nanosleep(void *req, void *rem);
int64_t sys_gettimeofday(void *tv, void *tz);
int64_t sys_time(long *tloc);
int64_t sys_getrandom(void *buf, size_t len, int flags);
int64_t sys_sysinfo(void *info);
int64_t sys_umask(int mask);
int64_t sys_select(int nfds, void *readfds, void *writefds,
                  void *exceptfds, void *timeout);

#endif /* _SYSCALL_H */
