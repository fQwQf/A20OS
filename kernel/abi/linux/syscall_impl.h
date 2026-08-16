#ifndef _LINUX_SYSCALL_IMPL_H
#define _LINUX_SYSCALL_IMPL_H

#include "abi/linux/syscall_entry.h"
#include "abi/current.h"
#include "abi/core_api.h"
#include "arch/syscall_hook.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "mm/swap.h"
#include "core/timer.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/klog.h"
#include "core/timekeeping.h"
#include "core/random.h"
#include "net/socket.h"
#include "sys/usercopy.h"

extern int syscall_sig_diag_count;
extern int syscall_sleep_diag_count;

/* LINUX_ABI_BOUNDARY_CONTRACT: Linux syscall files translate user ABI shapes,
 * flags, and errno values, then call core kernel APIs; they must not become
 * owners of subsystem-global state or bypass shared lifetime rules. */

int syscall_path_at(int dirfd, const char *path, char *out, size_t outsz);

#ifndef LINUX_IO_CHUNK_SIZE
#define LINUX_IO_CHUNK_SIZE 65536
#endif

#if defined(LINUX_SYSCALL_DECLARE_PROTOTYPES)

int64_t sys_read(int fd, char *buf, size_t count);
int64_t sys_write(int fd, const char *buf, size_t count);
int64_t sys_writev(int fd, const void *iov, int iovcnt);
int64_t sys_readv(int fd, const void *iov, int iovcnt);
int64_t sys_preadv(int fd, const void *iov, int iovcnt, long off);
int64_t sys_pwritev(int fd, const void *iov, int iovcnt, long off);
int64_t sys_preadv2(int fd, const void *iov, int iovcnt, long off, int flags);
int64_t sys_pwritev2(int fd, const void *iov, int iovcnt, long off, int flags);
int64_t sys_pread64(int fd, char *buf, size_t count, long off);
int64_t sys_pwrite64(int fd, char *buf, size_t count, long off);
int64_t sys_openat(int dirfd, const char *path, int flags, int mode);
int64_t sys_close(int fd);
int64_t sys_lseek(int fd, long offset, int whence);
int64_t sys_dup(int fd);
int64_t sys_dup3(int oldfd, int newfd, int flags);
int64_t sys_fcntl(int fd, int cmd, long arg);
int64_t sys_flock(int fd, int operation);
int64_t sys_pipe2(int *pipefd, int flags);
int64_t sys_ioctl(int fd, unsigned long req, void *arg);
int64_t sys_sync(void);
int64_t sys_fsync(int fd);
int64_t sys_fdatasync(int fd);
int64_t sys_ftruncate(int fd, long length);
int64_t sys_truncate(const char *path, long length);
int64_t sys_fallocate(int fd, int mode, long off, long len);
int64_t sys_posix_fadvise(int fd, long off, long len, int advice);
int64_t sys_copy_file_range(int fd_in, long *off_in, int fd_out, long *off_out, size_t len, unsigned flags);
int64_t sys_splice(int fd_in, long *off_in, int fd_out, long *off_out, size_t len, unsigned flags);
int64_t sys_vmsplice(int fd, const void *iov, unsigned long nr_segs, unsigned flags);
int64_t sys_tee(int fd_in, int fd_out, size_t len, unsigned flags);
int64_t sys_close_range(unsigned first, unsigned last, unsigned flags);
int64_t sys_sendfile(int out_fd, int in_fd, long *off, size_t count);
int64_t sys_select(int nfds, void *readfds, void *writefds,
                   void *exceptfds, void *timeout);
int64_t sys_pselect6(int nfds, void *readfds, void *writefds,
                     void *exceptfds, void *timeout, void *sigmask);
int64_t sys_poll(void *fds, int nfds, int timeout);
int64_t sys_ppoll(void *fds, int nfds, void *tmo, void *sigmask);
int64_t sys_epoll_create(int size);
int64_t sys_epoll_create1(int flags);
int64_t sys_epoll_ctl(int epfd, int op, int fd, void *event);
int64_t sys_epoll_wait(int epfd, void *events, int maxevents, int timeout);
int64_t sys_epoll_pwait(int epfd, void *events, int maxevents, int timeout, const void *sigmask, size_t sigsetsize);
int64_t sys_eventfd2(unsigned initval, int flags);
int64_t sys_timerfd_create(int clockid, int flags);
int64_t sys_signalfd4(int fd, const void *mask, size_t sigsetsize, int flags);
int64_t sys_timerfd_settime(int fd, int flags, const void *new_value, void *old_value);
int64_t sys_timerfd_gettime(int fd, void *curr_value);
int64_t sys_inotify_init1(int flags);
int64_t sys_socket(int domain, int type, int protocol);
int64_t sys_socketpair(int domain, int type, int protocol, int *sv);
int64_t sys_bind(int fd, const void *addr, size_t addrlen);
int64_t sys_listen(int fd, int backlog);
int64_t sys_accept(int fd, void *addr, void *addrlen);
int64_t sys_accept4(int fd, void *addr, void *addrlen, int flags);
int64_t sys_connect(int fd, const void *addr, size_t addrlen);
int64_t sys_getsockname(int fd, void *addr, void *addrlen);
int64_t sys_getpeername(int fd, void *addr, void *addrlen);
int64_t sys_sendto(int fd, const void *buf, size_t len, int flags,
                   const void *addr, size_t addrlen);
int64_t sys_recvfrom(int fd, void *buf, size_t len, int flags,
                     void *addr, void *addrlen);
int64_t sys_setsockopt(int fd, int level, int optname, const void *optval, size_t optlen);
int64_t sys_getsockopt(int fd, int level, int optname, void *optval, void *optlen);
int64_t sys_shutdown(int fd, int how);
int64_t sys_sendmsg(int fd, const void *msg, int flags);
int64_t sys_recvmsg(int fd, void *msg, int flags);
int64_t sys_sendmmsg(int fd, void *mmsg, unsigned vlen, int flags);
int64_t sys_recvmmsg(int fd, void *mmsg, unsigned vlen, int flags, void *timeout);

int64_t sys_mkdirat(int dirfd, const char *path, int mode);
int64_t sys_unlinkat(int dirfd, const char *path, int flags);
int64_t sys_renameat2(int olddir, const char *oldpath,
                       int newdir, const char *newpath, int flags);
int64_t sys_renameat(int olddir, const char *oldpath,
                     int newdir, const char *newpath);
int64_t sys_chdir(const char *path);
int64_t sys_fchdir(int fd);
int64_t sys_getcwd(char *buf, size_t size);
int64_t sys_fstat(int fd, void *st);
int64_t sys_fstatat(int dirfd, const char *path, void *st, int flags);
int64_t sys_readlinkat(int dirfd, const char *path, char *buf, size_t sz);
int64_t sys_faccessat(int dirfd, const char *path, int mode);
int64_t sys_faccessat2(int dirfd, const char *path, int mode, int flags);
int64_t sys_chmod(const char *path, int mode);
int64_t sys_fchmod(int fd, int mode);
int64_t sys_fchmodat(int dirfd, const char *path, int mode, int flags);
int64_t sys_chown(const char *path, int uid, int gid);
int64_t sys_lchown(const char *path, int uid, int gid);
int64_t sys_fchown(int fd, int uid, int gid);
int64_t sys_fchownat(int dirfd, const char *path, int uid, int gid, int flags);
int64_t sys_setxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int64_t sys_lsetxattr(const char *path, const char *name, const void *value, size_t size, int flags);
int64_t sys_fsetxattr(int fd, const char *name, const void *value, size_t size, int flags);
int64_t sys_getxattr(const char *path, const char *name, void *value, size_t size);
int64_t sys_lgetxattr(const char *path, const char *name, void *value, size_t size);
int64_t sys_fgetxattr(int fd, const char *name, void *value, size_t size);
int64_t sys_listxattr(const char *path, char *list, size_t size);
int64_t sys_llistxattr(const char *path, char *list, size_t size);
int64_t sys_flistxattr(int fd, char *list, size_t size);
int64_t sys_removexattr(const char *path, const char *name);
int64_t sys_lremovexattr(const char *path, const char *name);
int64_t sys_fremovexattr(int fd, const char *name);
int64_t sys_getdents64(int fd, void *dirp, size_t count);
int64_t sys_linkat(int olddirfd, const char *oldpath,
                    int newdirfd, const char *newpath, int flags);
int64_t sys_symlinkat(const char *target, int newdirfd, const char *linkpath);
int64_t sys_statx(int dirfd, const char *path, int flags, unsigned mask, void *buf);
int64_t sys_statfs(const char *path, void *buf);
int64_t sys_fstatfs(int fd, void *buf);
int64_t sys_mount(const char *src, const char *target,
                   const char *fstype, int flags, const char *data);
int64_t sys_umount2(const char *target, int flags);
int64_t sys_utimensat(int dirfd, const char *path, void *times, int flags);
int64_t sys_execveat(int dirfd, const char *path, char **argv, char **envp, int flags);
int64_t sys_chroot(const char *path);
int64_t sys_mknod(const char *path, int mode, unsigned dev);
int64_t sys_mknodat(int dirfd, const char *path, int mode, unsigned dev);

int64_t sys_set_thread_area(void *ptr);
int64_t sys_get_thread_area(void *ptr);
int64_t sys_pause(void);
int64_t sys_utime(const char *path, const void *times);
int64_t sys_utimes(const char *path, const void *times);
int64_t sys_exit(int code);
int64_t sys_exit_group(int code);
int64_t sys_getpid(void);
int64_t sys_getppid(void);
int64_t sys_gettid(void);
int64_t sys_set_tid_address(int *tidptr);
int64_t sys_set_robust_list(void *head, size_t len);
int64_t sys_capget(void *hdrp, void *datap);
int64_t sys_capset(void *hdrp, const void *datap);
int64_t sys_getuid(void);
int64_t sys_geteuid(void);
int64_t sys_getgid(void);
int64_t sys_getegid(void);
int64_t sys_setuid(int uid);
int64_t sys_setgid(int gid);
int64_t sys_seteuid(int euid);
int64_t sys_setegid(int egid);
int64_t sys_setreuid(int ruid, int euid);
int64_t sys_setregid(int rgid, int egid);
int64_t sys_setresuid(int ruid, int euid, int suid);
int64_t sys_getresuid(int *ruid, int *euid, int *suid);
int64_t sys_setresgid(int rgid, int egid, int sgid);
int64_t sys_getresgid(int *rgid, int *egid, int *sgid);
int64_t sys_setfsuid(int uid);
int64_t sys_setfsgid(int gid);
int64_t sys_getpgid(int pid);
int64_t sys_setpgid(int pid, int pgid);
int64_t sys_setsid(void);
int64_t sys_clone(uint64_t flags, void *stack, int *ptid, uint64_t tls, int *ctid);
int64_t sys_execve(const char *path, char **argv, char **envp);
int64_t sys_wait4(int pid, int *status, int options, void *rusage);
int64_t sys_ptrace(int request, int pid, void *addr, void *data);
int64_t sys_waitid(int type, int id, void *info, int options, void *rusage);
int64_t sys_sched_yield(void);
int64_t sys_sched_get_priority_max(int policy);
int64_t sys_sched_get_priority_min(int policy);
int64_t sys_sched_getaffinity(int pid, size_t cpusetsize, void *mask);
int64_t sys_getcpu(unsigned *cpu, unsigned *node, void *cache);
int64_t sys_sched_setaffinity(int pid, size_t cpusetsize, const void *mask);
int64_t sys_sched_getparam(int pid, void *param);
int64_t sys_sched_setparam(int pid, const void *param);
int64_t sys_sched_getscheduler(int pid);
int64_t sys_sched_setscheduler(int pid, int policy, const void *param);
int64_t sys_sched_rr_get_interval(int pid, void *tp);
int64_t sys_getpriority(int which, int who);
int64_t sys_setpriority(int which, int who, int prio);
int64_t sys_nice(int inc);
int64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd);
int64_t sys_prctl(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4);
int64_t sys_arch_prctl(int op, uint64_t addr);
int64_t sys_prlimit64(int pid, int resource, void *new_rlim, void *old_rlim);
int64_t sys_getrlimit(int resource, void *rlim);
int64_t sys_setrlimit(int resource, void *rlim);
int64_t sys_getrusage(int who, void *usage);

int64_t sys_kill(int pid, int sig);
int64_t sys_tkill(int tid, int sig);
int64_t sys_tgkill(int tgid, int tid, int sig);
int64_t sys_sigaction(int signum, void *act, void *oldact, size_t sigsetsize);
int64_t sys_sigprocmask(int how, void *set, void *oldset, size_t sigsetsize);
int64_t sys_sigreturn(trap_context_t *ctx);
int64_t sys_sigsuspend(void *mask, size_t sigsetsize);
int64_t sys_sigaltstack(void *ss, void *old_ss);
int64_t sys_sigtimedwait(const uint64_t *set, void *info, const void *timeout, size_t sigsetsize);
int64_t sys_rt_sigqueueinfo(int tgid, int sig, void *uinfo);
int linux_pidfd_create(int pid, int flags);
int64_t sys_pidfd_send_signal(int pidfd, int sig, void *uinfo, unsigned flags);

int64_t sys_brk(uint64_t addr);
int64_t sys_mmap(uint64_t addr, size_t len, int prot, int flags, int fd, long off);
int64_t sys_munmap(uint64_t addr, size_t len);
int64_t sys_mprotect(uint64_t addr, size_t len, int prot);
int64_t sys_msync(uint64_t addr, size_t len, int flags);
int64_t sys_madvise(uint64_t addr, size_t len, int advice);
int64_t sys_mremap(uint64_t old_addr, size_t old_size, size_t new_size,
                    int flags, uint64_t new_addr);
int64_t sys_shm_open(const char *name, int oflag, int mode);
int64_t sys_memfd_create(const char *name, unsigned flags);
int64_t sys_shmget(int key, size_t size, int shmflg);
int64_t sys_shmat(int shmid, const void *shmaddr, int shmflg);
int64_t sys_shmdt(const void *shmaddr);
int64_t sys_shmctl(int shmid, int cmd, void *buf);
int64_t sys_semget(int key, int nsems, int semflg);
int64_t sys_semctl(int semid, int semnum, int cmd, uint64_t arg);
int64_t sys_semtimedop(int semid, const void *sops, size_t nsops, const void *timeout);
int64_t sys_semop(int semid, const void *sops, size_t nsops);
int64_t sys_bpf(int cmd, void *attr, unsigned size);

int64_t sys_clock_settime(int clk, void *tp);
int64_t sys_clock_gettime(int clk, void *tp);
int64_t sys_clock_gettime32(int clk, void *tp);
int64_t sys_clock_getres(int clk, void *tp);
int64_t sys_nanosleep(void *req, void *rem);
int64_t sys_gettimeofday(void *tv, void *tz);
int64_t sys_settimeofday(void *tv, void *tz);
int64_t sys_times(void *buf);
int64_t sys_time(long *tloc);
int64_t sys_clock_nanosleep(int clk, int flags, const void *req, void *rem);
int64_t sys_getitimer(int which, void *curr_value);
int64_t sys_setitimer(int which, const void *new_value, void *old_value);
int64_t sys_alarm(unsigned seconds);
int64_t sys_timer_create(int clockid, void *sevp, int *timerid);
int64_t sys_timer_delete(int timerid);
int64_t sys_timer_gettime(int timerid, void *curr_value);
int64_t sys_timer_getoverrun(int timerid);
int64_t sys_timer_settime(int timerid, int flags, const void *new_value, void *old_value);
int64_t sys_adjtimex(void *buf);
int64_t sys_clock_adjtime(int clk, void *buf);
int64_t sys_membarrier(int cmd, unsigned flags, int cpu_id);

int64_t sys_uname(void *buf);
int64_t sys_sysinfo(void *info);
int64_t sys_getgroups(int size, int *list);
int64_t sys_setgroups(size_t size, const int *list);
int64_t sys_umask(int mask);
int64_t sys_syslog(int type, char *buf, int len);

int64_t sys_getrandom(void *buf, size_t len, unsigned int flags);
int64_t sys_futex(int *uaddr, int op, int val, void *timeout,
                   int *uaddr2, int val3);

int64_t sys_getsid(int pid);
int64_t sys_rt_sigpending(void *set, size_t sigsetsize);
int64_t sys_sethostname(const char *name, size_t len);
int64_t sys_setdomainname(const char *name, size_t len);
int64_t sys_sync_file_range(int fd, long offset, long nbytes, unsigned flags);
int64_t sys_syncfs(int fd);
int64_t sys_mlock(uint64_t addr, size_t len);
int64_t sys_munlock(uint64_t addr, size_t len);
int64_t sys_mlockall(int flags);
int64_t sys_munlockall(void);
int64_t sys_mincore(uint64_t addr, size_t length, unsigned char *vec);
int64_t sys_personality(unsigned int persona);
int64_t sys_vhangup(void);
int64_t sys_unshare(int flags);
int64_t sys_setns(int fd, int nstype);
int64_t sys_pivot_root(const char *new_root, const char *put_old);
int64_t sys_get_mempolicy(int *policy, unsigned long *nmask,
                          unsigned long maxnode, unsigned long addr,
                          unsigned long flags);
int64_t sys_sched_setattr(int pid, const void *attr, unsigned flags);
int64_t sys_sched_getattr(int pid, void *attr, unsigned size, unsigned flags);
int64_t sys_clone3(void *cl_args, size_t size);
int64_t sys_openat2(int dirfd, const char *pathname, const void *how, size_t size);
int64_t sys_inotify_init(int flags);
int64_t sys_inotify_add_watch(int fd, const char *pathname, uint32_t mask);
int64_t sys_inotify_rm_watch(int fd, int wd);
int64_t sys_get_robust_list(int pid, void *head_ptr, size_t *len_ptr);

/* A20OS extensions: channel IPC bridge (sys_a20_bridge.c). */
int64_t sys_a20_channel_pair(const linux_syscall_args_t *args);
int64_t sys_a20_registry_client_fd(const linux_syscall_args_t *args);

/* Keyring (sys_keyring.c). */
int64_t sys_add_key(const char *type, const char *description,
                    const void *payload, size_t plen, int32_t ringid);
int64_t sys_request_key(const char *type, const char *description,
                        const char *callout_info, int32_t dest_ringid);
int64_t sys_keyctl(int cmd, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);

/* fanotify (sys_fanotify.c). */
int64_t sys_fanotify_init(unsigned flags, unsigned event_f_flags);
int64_t sys_fanotify_mark(int fanotify_fd, unsigned flags, uint64_t mask,
                          int dfd, const char *path);

/* Process accounting (sys_acct.c). */
int64_t sys_acct(const char *filename);

/* Linux AIO (sys_aio.c). */
int64_t sys_io_setup(unsigned nr_events, void *ctxp);
int64_t sys_io_destroy(uint64_t ctx);
int64_t sys_io_submit(uint64_t ctx, long nr, const void *iocbpp);
int64_t sys_io_getevents(uint64_t ctx, long min_nr, long nr, void *events,
                         const void *timeout);
int64_t sys_io_pgetevents(uint64_t ctx, long min_nr, long nr, void *events,
                          const void *timeout, const void *sigarg);
int64_t sys_io_cancel(uint64_t ctx, const void *iocb, void *result);

/* pidfd (sys_pidfd.c). */
int64_t sys_pidfd_open(int pid, unsigned flags);
int64_t sys_pidfd_getfd(int pidfd, int targetfd, unsigned flags);

/* epoll_pwait2 (sys_epoll.c). */
int64_t sys_epoll_pwait2(int epfd, void *events, int maxevents,
                         const void *timeout, const void *sigmask,
                         size_t sigsetsize);

/* Kernel driver modules (sys_module.c). */
int64_t sys_init_module(const void *umod, unsigned long len,
                        const char *uargs);
int64_t sys_finit_module(int fd, const char *uargs, int flags);
int64_t sys_delete_module(const char *name_user, unsigned int flags);

/* Small compatibility group (sys_missing.c). */
int64_t sys_restart_syscall(void);
int64_t sys_kcmp(int pid1, int pid2, int type, unsigned long idx1,
                 unsigned long idx2);
int64_t sys_readahead(int fd, long off, size_t count);
int64_t sys_cachestat(int fd, const void *cstat_range, void *cstat,
                      unsigned flags);
int64_t sys_lookup_dcookie(uint64_t cookie, char *buf, size_t len);
int64_t sys_quotactl(int cmd, const char *special, int id, void *addr);
int64_t sys_quotactl_fd(int fd, int cmd, int id, void *addr);
int64_t sys_remap_file_pages(uint64_t start, size_t size, int prot,
                             uint64_t pgoff, int flags);
int64_t sys_memfd_secret(unsigned flags);
int64_t sys_rseq(void *rseq, uint32_t rseq_len, int flags, uint32_t sig);

/* Cross-process memory (sys_process_vm.c). */
int64_t sys_process_vm_readv(int pid, const void *lvec, unsigned long liovcnt,
                             const void *rvec, unsigned long riovcnt,
                             unsigned long flags);
int64_t sys_process_vm_writev(int pid, const void *lvec, unsigned long liovcnt,
                              const void *rvec, unsigned long riovcnt,
                              unsigned long flags);
int64_t sys_process_madvise(int pid, const void *iov, unsigned long iovcnt,
                            int advice, unsigned long flags);
int64_t sys_process_mrelease(int pidfd, unsigned flags);

/* futex_waitv / futex_requeue (sys_futex.c). */
int64_t sys_futex_waitv(const void *waiters, unsigned nr_futexes,
                        unsigned flags, const void *timeout, uint64_t clockid);
int64_t sys_futex_requeue(int *uaddr, int *uaddr2, int nr_wake,
                          int nr_requeue, uint32_t flags);

/* mempolicy family (sys_mempolicy.c). */
int64_t sys_set_mempolicy(int mode, const unsigned long *nmask,
                          unsigned long maxnode);
int64_t sys_mbind(uint64_t addr, unsigned long len, int mode,
                  const unsigned long *nmask, unsigned long maxnode,
                  unsigned flags);
int64_t sys_migrate_pages(int pid, unsigned long maxnode,
                          const unsigned long *old_nodes,
                          const unsigned long *new_nodes);
int64_t sys_move_pages(int pid, unsigned long nr_pages, const void *pages,
                       const int *nodes, int *status, int flags);
int64_t sys_set_mempolicy_home_node(uint64_t addr, unsigned long len,
                                    int home_node, unsigned long flags);

/* File handles (sys_handle.c). */
int64_t sys_name_to_handle_at(int dirfd, const char *pathname, void *handle,
                              int *mnt_id, int flags);
int64_t sys_open_by_handle_at(int mount_fd, void *handle, int flags);

/* New mount API (sys_mount_api.c). */
int64_t sys_fsopen(const char *fsname, unsigned flags);
int64_t sys_fsconfig(int fs_fd, unsigned cmd, const char *key,
                     const void *value, int aux);
int64_t sys_fsmount(int fs_fd, unsigned flags, unsigned mnt_flags);
int64_t sys_fspick(int dirfd, const char *path, unsigned flags);
int64_t sys_open_tree(int dirfd, const char *path, unsigned flags);
int64_t sys_move_mount(int from_dfd, const char *from_path, int to_dfd,
                       const char *to_path, unsigned flags);
int64_t sys_mount_setattr(int dfd, const char *path, unsigned flags,
                          const void *attr, size_t size);

/* io_uring (sys_io_uring.c). */
int64_t sys_io_uring_setup(unsigned entries, void *params);
int64_t sys_io_uring_enter(int fd, unsigned to_submit, unsigned min_complete,
                           unsigned flags, const void *sig, size_t sigsz);
int64_t sys_io_uring_register(int fd, unsigned opcode, const void *arg,
                              unsigned nr_args);

/* Landlock (sys_landlock.c). */
int64_t sys_landlock_create_ruleset(const void *attr, size_t size,
                                    unsigned flags);
int64_t sys_landlock_add_rule(int ruleset_fd, int rule_type,
                              const void *attr, unsigned flags);
int64_t sys_landlock_restrict_self(int ruleset_fd, unsigned flags);

/* SysV message queues (sys_memfd_shm.c). */
int64_t sys_msgget(int key, int msgflg);
int64_t sys_msgsnd(int msqid, const void *msgp, size_t msgsz, int msgflg);
int64_t sys_msgrcv(int msqid, void *msgp, size_t msgsz, int64_t msgtyp,
                   int msgflg);
int64_t sys_msgctl(int msqid, int cmd, void *buf);

/* POSIX message queues (sys_posix_mq.c). */
int64_t sys_mq_open(const char *name, int oflag, int mode, const void *attr);
int64_t sys_mq_unlink(const char *name);
int64_t sys_mq_getsetattr(int mqdes, const void *newattr, void *oldattr);
int64_t sys_mq_notify(int mqdes, const void *notification);
int64_t sys_mq_timedsend(int mqdes, const char *msg_ptr, size_t msg_len,
                         unsigned msg_prio, const void *abs_timeout);
int64_t sys_mq_timedreceive(int mqdes, char *msg_ptr, size_t msg_len,
                            unsigned *msg_prio, const void *abs_timeout);

/* ioprio / pkey / misc compat (sys_missing.c, proc/sched_compat.c). */
int64_t sys_ioprio_set(int which, int who, int ioprio);
int64_t sys_ioprio_get(int which, int who);
int64_t sys_pkey_alloc(unsigned flags, uint64_t init_access_rights);
int64_t sys_pkey_free(int pkey);
int64_t sys_pkey_mprotect(uint64_t addr, size_t len, int prot, int pkey);
int64_t sys_mlock2(uint64_t addr, size_t len, int flags);
int64_t sys_mseal(uint64_t addr, size_t len, unsigned flags);
int64_t sys_seccomp(unsigned op, unsigned flags, const void *uargs);
int64_t sys_kexec_load(uint64_t entry, uint64_t nr_segments,
                       const void *segments, uint64_t flags);
int64_t sys_kexec_file_load(int kernel_fd, int initrd_fd,
                            uint64_t cmdline_len, const void *cmdline,
                            uint64_t flags);
int64_t sys_nfsservctl(int cmd, const void *arg, void *res);
int64_t sys_map_shadow_stack(uint64_t addr, uint64_t size, unsigned flags);

/* userfaultfd(2): user-space page-fault handling (kernel/ipc/userfaultfd.c). */
int64_t sys_userfaultfd(unsigned flags);

/* perf_event_open(2): software events (kernel/abi/linux/sys_perf.c). */
int64_t sys_perf_event_open(const void *attr, int pid, int cpu, int group_fd,
                            unsigned long flags);

/* futex_wait / futex_wake split syscalls (sys_futex.c). */
int64_t sys_futex_wait(void *uaddr, uint32_t val, const void *timeout,
                       uint32_t flags);
int64_t sys_futex_wake(void *uaddr, uint32_t nr, uint32_t flags);

/* xattr-at variants (sys_xattr.c). */
int64_t sys_setxattrat(int dirfd, const char *pathname, const char *name,
                       const void *value, size_t size, int flags);
int64_t sys_getxattrat(int dirfd, const char *pathname, const char *name,
                       void *value, size_t size, int flags);
int64_t sys_listxattrat(int dirfd, const char *pathname, char *list,
                        size_t size, int flags);
int64_t sys_removexattrat(int dirfd, const char *pathname, const char *name,
                          int flags);

/* RISC-V arch-specific (sys_arch_riscv.c). */
int64_t sys_riscv_hwprobe(const void *pairs, size_t pair_count,
                          size_t cpu_size, const void *cpus, unsigned flags);
int64_t sys_riscv_flush_icache(uint64_t start, uint64_t end, uint64_t flags);

/* rt_tgsigqueueinfo (sys_signal.c). */
int64_t sys_rt_tgsigqueueinfo(int tgid, int tid, int sig, void *uinfo);

/* statmount/listmount/listns/open_tree_attr (sys_mount_api.c). */
int64_t sys_statmount(uint64_t mnt_id, uint64_t flags, void *buf,
                      size_t bufsize, unsigned int mask);
int64_t sys_listmount(uint64_t mnt_id, uint64_t last_mnt_id, uint64_t *list,
                      size_t nr, unsigned int flags);
int64_t sys_listns(unsigned int nstype, uint64_t *nsids, size_t nr);
int64_t sys_open_tree_attr(int dfd, const char *path, unsigned int flags,
                           unsigned int attr_mask, void *attr, size_t size);

/* LoongArch fileattr (sys_fileattr.c). */
int64_t sys_file_getattr(int fd, void *attr);
int64_t sys_file_setattr(int fd, void *attr);

/* LSM introspection (sys_lsm.c). */
int64_t sys_lsm_get_self_attr(unsigned int attr, void *ctx, size_t *size,
                              unsigned int flags);
int64_t sys_lsm_set_self_attr(unsigned int attr, const void *ctx, size_t size,
                              unsigned int flags);
int64_t sys_lsm_list_modules(uint64_t *ids, size_t *size, unsigned int flags);

#endif /* LINUX_SYSCALL_DECLARE_PROTOTYPES */

/* Internal helper shared by the Linux and Native ioctl paths; always
 * declared (not a syscall). */
int64_t sys_ioctl_gfd(int64_t gfd, unsigned long req, void *arg);

#endif /* _LINUX_SYSCALL_IMPL_H */
