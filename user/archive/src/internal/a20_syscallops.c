/*
 * musl A20 syscall operations — Linux syscall → A20 Native ABI mapping.
 *
 * Each __sys_* function translates Linux POSIX calling conventions
 * to A20 handle-based syscalls via ecall.
 */
#include "syscall.h"
#include <stdint.h>
#include <stddef.h>
#include <errno.h>

#define A20_OK           0
#define A20_ERR_PERM     1
#define A20_ERR_NO_ENTRY 2
#define A20_ERR_IO       4
#define A20_ERR_BAD_HANDLE 5
#define A20_ERR_NO_MEMORY  6
#define A20_ERR_ACCESS     7
#define A20_ERR_FAULT      8
#define A20_ERR_BUSY       9
#define A20_ERR_EXISTS     10
#define A20_ERR_NOT_SUPPORTED 11
#define A20_ERR_INVALID_ARGUMENT 12
#define A20_ERR_NO_SPACE  13
#define A20_ERR_WOULD_BLOCK 18
#define A20_ERR_TIMED_OUT 19

static long a20_to_errno(long r)
{
    if (r >= 0) return r;
    switch (-r) {
    case 1:  return -EPERM;
    case 2:  return -ENOENT;
    case 4:  return -EIO;
    case 5:  return -EBADF;
    case 6:  return -ENOMEM;
    case 7:  return -EACCES;
    case 8:  return -EFAULT;
    case 9:  return -EBUSY;
    case 10: return -EEXIST;
    case 11: return -ENOSYS;
    case 12: return -EINVAL;
    case 13: return -ENOSPC;
    case 18: return -EAGAIN;
    case 19: return -ETIMEDOUT;
    default: return -EINVAL;
    }
}

/* ---- File I/O ---- */

long __sys_openat(int fd, const char *path, int flags, int mode)
{
    (void)fd;
    long r = __syscall3(0x0400, (long)path, flags, mode);
    return a20_to_errno(r);
}

long __sys_read(int fd, void *buf, size_t count)
{
    long r = __syscall3(0x0401, fd, (long)buf, count);
    return a20_to_errno(r);
}

long __sys_write(int fd, const void *buf, size_t count)
{
    long r = __syscall3(0x0402, fd, (long)buf, count);
    return a20_to_errno(r);
}

long __sys_close(int fd)
{
    long r = __syscall1(0x0100, fd);
    return a20_to_errno(r);
}

long __sys_lseek(int fd, long offset, int whence)
{
    long r = __syscall3(0x0105, fd, offset, whence);
    return a20_to_errno(r);
}

long __sys_fstat(int fd, void *st)
{
    long r = __syscall2(0x0403, fd, (long)st);
    return a20_to_errno(r);
}

long __sys_fstatat(int fd, const char *path, void *st, int flags)
{
    (void)flags;
    long r = __syscall3(0x0400, (long)path, 0, 0);
    if (r >= 0) r = __syscall2(0x0403, r, (long)st);
    return a20_to_errno(r);
}

long __sys_readlinkat(int fd, const char *path, char *buf, size_t bufsz)
{
    (void)fd;
    long r = __syscall3(0x040B, (long)path, (long)buf, bufsz);
    return a20_to_errno(r);
}

long __sys_getdents64(int fd, void *dirp, size_t count)
{
    long r = __syscall3(0x0408, fd, (long)dirp, count);
    return a20_to_errno(r);
}

long __sys_mkdirat(int fd, const char *path, int mode)
{
    (void)fd;
    long r = __syscall3(0x0404, (long)path, 1, mode);
    return a20_to_errno(r);
}

long __sys_unlinkat(int fd, const char *path, int flags)
{
    (void)fd;
    long r = __syscall2(0x0405, (long)path, flags);
    return a20_to_errno(r);
}

long __sys_renameat2(int oldfd, const char *old, int newfd, const char *newp, int flags)
{
    (void)oldfd; (void)newfd; (void)flags;
    long r = __syscall2(0x0406, (long)old, (long)newp);
    return a20_to_errno(r);
}

long __sys_linkat(int oldfd, const char *old, int newfd, const char *newp, int flags)
{
    (void)oldfd; (void)newfd; (void)flags;
    long r = __syscall2(0x0409, (long)old, (long)newp);
    return a20_to_errno(r);
}

long __sys_symlinkat(const char *target, int fd, const char *linkpath)
{
    (void)fd;
    long r = __syscall2(0x040A, (long)target, (long)linkpath);
    return a20_to_errno(r);
}

long __sys_fchmod(int fd, int mode) { (void)fd; (void)mode; return 0; }
long __sys_fchmodat(int fd, const char *path, int mode, int flags) { (void)fd; (void)path; (void)mode; (void)flags; return 0; }
long __sys_fchown(int fd, int uid, int gid) { (void)fd; (void)uid; (void)gid; return 0; }
long __sys_fchownat(int fd, const char *path, int uid, int gid, int flags) { (void)fd; (void)path; (void)uid; (void)gid; (void)flags; return 0; }
long __sys_utimensat(int fd, const char *path, const void *ts, int flags) { (void)fd; (void)path; (void)ts; (void)flags; return 0; }
long __sys_ftruncate(int fd, long length) { (void)fd; (void)length; return 0; }
long __sys_fallocate(int fd, int mode, long off, long len) { (void)fd; (void)mode; (void)off; (void)len; return 0; }
long __sys_fadvise64(int fd, long off, long len, int advice) { (void)fd; (void)off; (void)len; (void)advice; return 0; }
long __sys_sync(void) { return __syscall0(0x0410); }
long __sys_fsync(int fd) { (void)fd; return 0; }
long __sys_fdatasync(int fd) { (void)fd; return 0; }
long __sys_statfs(const char *path, void *buf) { (void)path; (void)buf; return -ENOSYS; }
long __sys_fstatfs(int fd, void *buf) { (void)fd; (void)buf; return -ENOSYS; }

/* ---- Network I/O ---- */

long __sys_socket(int domain, int type, int proto)
{
    return a20_to_errno(__syscall3(0x0600, domain, type, proto));
}

long __sys_bind(int fd, const void *addr, int len)
{
    return a20_to_errno(__syscall3(0x0601, fd, (long)addr, len));
}

long __sys_listen(int fd, int backlog)
{
    return a20_to_errno(__syscall2(0x0604, fd, backlog));
}

long __sys_accept(int fd, void *addr, void *addrlen)
{
    return a20_to_errno(__syscall3(0x0603, fd, (long)addr, (long)addrlen));
}

long __sys_connect(int fd, const void *addr, int len)
{
    return a20_to_errno(__syscall3(0x0602, fd, (long)addr, len));
}

long __sys_sendto(int fd, const void *buf, size_t len, int flags, const void *addr, int alen)
{
    return a20_to_errno(__syscall6(0x0605, fd, (long)buf, len, flags, (long)addr, alen));
}

long __sys_recvfrom(int fd, void *buf, size_t len, int flags, void *addr, void *alen)
{
    return a20_to_errno(__syscall6(0x0606, fd, (long)buf, len, flags, (long)addr, (long)alen));
}

long __sys_sendmsg(int fd, const void *msg, int flags)
{
    return a20_to_errno(__syscall3(0x0605, fd, (long)msg, flags));
}

long __sys_recvmsg(int fd, void *msg, int flags)
{
    return a20_to_errno(__syscall3(0x0606, fd, (long)msg, flags));
}

long __sys_setsockopt(int fd, int level, int opt, const void *val, int len)
{ (void)fd; (void)level; (void)opt; (void)val; (void)len; return 0; }

long __sys_getsockopt(int fd, int level, int opt, void *val, void *len)
{ (void)fd; (void)level; (void)opt; (void)val; (void)len; return 0; }

long __sys_shutdown(int fd, int how)
{
    return a20_to_errno(__syscall2(0x0609, fd, how));
}

long __sys_getsockname(int fd, void *addr, void *alen)
{
    return a20_to_errno(__syscall3(0x0608, fd, (long)addr, (long)alen));
}

long __sys_getpeername(int fd, void *addr, void *alen)
{
    return a20_to_errno(__syscall3(0x0608, fd, (long)addr, (long)alen));
}

long __sys_socketpair(int domain, int type, int proto, int *sv)
{
    long r = a20_to_errno(__syscall3(0x0607, domain, type, proto));
    if (r >= 0) { sv[0] = (int)r; sv[1] = (int)r + 1; }
    return r;
}

/* ---- Process ---- */

long __sys_clone(unsigned long flags, void *stack, int *ptid, int *ctid, unsigned long tls)
{ (void)flags; (void)stack; (void)ptid; (void)ctid; (void)tls; return -ENOSYS; }

long __sys_execve(const char *path, char **argv, char **envp)
{ (void)path; (void)argv; (void)envp; return -ENOSYS; }

long __sys_wait4(int pid, int *status, int options, void *rusage)
{
    return a20_to_errno(__syscall4(0x0202, pid, (long)status, options, (long)rusage));
}

long __sys_exit(int code) { __syscall1(0x0200, code); for(;;); }
long __sys_exit_group(int code) { __syscall1(0x0200, code); for(;;); }

long __sys_getpid(void) { return a20_to_errno(__syscall0(0x0204)); }
long __sys_getppid(void) { return 1; }
long __sys_gettid(void) { return a20_to_errno(__syscall0(0x0204)); }

/* ---- Memory ---- */

long __sys_mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
    return a20_to_errno(__syscall6(0x0303, (long)addr, len, prot, flags, fd, off));
}

long __sys_munmap(void *addr, size_t len)
{
    return a20_to_errno(__syscall2(0x0301, (long)addr, len));
}

long __sys_mprotect(void *addr, size_t len, int prot)
{
    return a20_to_errno(__syscall3(0x0302, (long)addr, len, prot));
}

long __sys_msync(void *addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return 0; }
long __sys_madvise(void *addr, size_t len, int advice) { (void)addr; (void)len; (void)advice; return 0; }
long __sys_mremap(void *old, size_t oldlen, size_t newlen, int flags, void *newaddr)
{ (void)old; (void)oldlen; (void)newlen; (void)flags; (void)newaddr; return -ENOSYS; }

long __sys_brk(long addr) { return a20_to_errno(__syscall1(0x0300, addr)); }

/* ---- Signal ---- */

long __sys_rt_sigaction(int sig, const void *act, void *oact, size_t setsize)
{ (void)sig; (void)act; (void)oact; (void)setsize; return 0; }

long __sys_rt_sigprocmask(int how, const void *set, void *oset, size_t setsize)
{ (void)how; (void)set; (void)oset; (void)setsize; return 0; }

long __sys_kill(int pid, int sig) { return a20_to_errno(__syscall2(0x0203, pid, sig)); }
long __sys_tgkill(int tgid, int tid, int sig) { (void)tgid; (void)tid; return a20_to_errno(__syscall2(0x0203, tid, sig)); }

/* ---- Time ---- */

long __sys_clock_gettime(int clk, void *ts)
{
    return a20_to_errno(__syscall2(0x0700, clk, (long)ts));
}

long __sys_clock_settime(int clk, const void *ts)
{
    return a20_to_errno(__syscall2(0x0704, clk, (long)ts));
}

long __sys_nanosleep(const void *req, void *rem)
{
    return a20_to_errno(__syscall2(0x0207, (long)req, (long)rem));
}

/* ---- System ---- */

long __sys_uname(void *buf) { return a20_to_errno(__syscall1(0x0A00, (long)buf)); }
long __sys_sysinfo(void *info) { return a20_to_errno(__syscall1(0x0A00, (long)info)); }
long __sys_getrandom(void *buf, size_t len, int flags)
{ (void)flags; return a20_to_errno(__syscall3(0x0A01, (long)buf, len, flags)); }

/* ---- Poll/Select/Epoll ---- */

long __sys_ppoll(struct pollfd *fds, int nfds, const void *timeout, const void *sigmask)
{ (void)fds; (void)nfds; (void)timeout; (void)sigmask; return -ENOSYS; }

long __sys_pselect6(int nfds, void *rfds, void *wfds, void *efds, const void *timeout, const void *sigmask)
{ (void)nfds; (void)rfds; (void)wfds; (void)efds; (void)timeout; (void)sigmask; return -ENOSYS; }

long __sys_epoll_create1(int flags)
{ (void)flags; return a20_to_errno(__syscall1(0x0500, 0)); }

long __sys_epoll_ctl(int epfd, int op, int fd, void *ev)
{ (void)epfd; (void)op; (void)fd; (void)ev; return 0; }

long __sys_epoll_pwait(int epfd, void *evs, int maxev, int timeout, const void *sigmask)
{ (void)epfd; (void)evs; (void)maxev; (void)timeout; (void)sigmask; return -ENOSYS; }

/* ---- Pipe ---- */

long __sys_pipe2(int *pipefd, int flags)
{
    (void)flags;
    return a20_to_errno(__syscall2(0x0504, (long)pipefd, flags));
}
