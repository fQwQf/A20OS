/*
 * musl arch/a20/bits/syscall.h — Linux syscall number to A20 mapping.
 *
 * musl expects __NR_* macros with Linux numbers. We remap
 * commonly used Linux syscalls to their A20 equivalents via
 * semantic bridge functions in syscall_bridge.c.
 */
#ifndef _A20_BITS_SYSCALL_H
#define _A20_BITS_SYSCALL_H

#define __NR_exit         0x0200
#define __NR_exit_group   0x0200
#define __NR_read         0x0401
#define __NR_write        0x0402
#define __NR_open         0x0400
#define __NR_close        0x0100
#define __NR_lseek        0x0105
#define __NR_fstat        0x0403
#define __NR_stat         0x0403
#define __NR_mmap         0x0303
#define __NR_munmap       0x0301
#define __NR_mprotect     0x0302
#define __NR_brk          0x0300
#define __NR_clone        0x0201
#define __NR_fork         0x0201
#define __NR_execve       0x0201
#define __NR_wait4        0x0202
#define __NR_kill         0x0203
#define __NR_getpid       0x0204
#define __NR_socket       0x0600
#define __NR_bind         0x0601
#define __NR_connect      0x0602
#define __NR_listen       0x0604
#define __NR_accept       0x0603
#define __NR_sendmsg      0x0605
#define __NR_recvmsg      0x0606
#define __NR_socketpair   0x0607
#define __NR_shutdown     0x0609
#define __NR_dup          0x0101
#define __NR_dup2         0x0101
#define __NR_pipe         0x0504
#define __NR_clock_gettime 0x0700
#define __NR_clock_settime 0x0704
#define __NR_nanosleep    0x0207
#define __NR_gettimeofday 0x0700
#define __NR_settimeofday 0x0704
#define __NR_madvise      0x0306
#define __NR_fcntl        0x0407
#define __NR_ioctl        0x0407
#define __NR_getuid       0x0204
#define __NR_getgid       0x0204
#define __NR_geteuid      0x0204
#define __NR_getegid      0x0204
#define __NR_setuid       0x0204
#define __NR_setgid       0x0204
#define __NR_uname        0x0A00
#define __NR_chdir        0x0400
#define __NR_getcwd       0x0400
#define __NR_mkdir        0x0404
#define __NR_rmdir        0x0405
#define __NR_unlink       0x0405
#define __NR_rename       0x0406
#define __NR_link         0x0409
#define __NR_symlink      0x040A
#define __NR_readlink     0x040B
#define __NR_chmod        0x0407
#define __NR_fchmod       0x0407
#define __NR_chown        0x0407
#define __NR_fchown       0x0407
#define __NR_umask        0x0407
#define __NR_getrlimit    0x020B
#define __NR_setrlimit    0x020C
#define __NR_getrusage    0x020D
#define __NR_sigaction    0x0203
#define __NR_sigprocmask  0x0203
#define __NR_rt_sigreturn 0x0203
#define __NR_sched_yield  0x0208
#define __NR_select       0x0502
#define __NR_poll         0x0502
#define __NR_epoll_create 0x0500
#define __NR_epoll_ctl    0x0501
#define __NR_epoll_wait   0x0502
#define __NR_eventfd      0x0500
#define __NR_timerfd_create 0x0701
#define __NR_timerfd_settime 0x0702
#define __NR_futex        0x0207
#define __NR_getdents     0x0408
#define __NR_fsync        0x0410
#define __NR_fdatasync    0x0410
#define __NR_sync         0x0410
#define __NR_mount        0x040E
#define __NR_umount2      0x040F
#define __NR_reboot       0x0A02
#define __NR_sysinfo      0x0A00

#define SYS_exit          __NR_exit
#define SYS_exit_group    __NR_exit_group
#define SYS_read          __NR_read
#define SYS_write         __NR_write
#define SYS_open          __NR_open
#define SYS_close         __NR_close
#define SYS_lseek         __NR_lseek
#define SYS_mmap          __NR_mmap
#define SYS_munmap        __NR_munmap
#define SYS_mprotect      __NR_mprotect
#define SYS_brk           __NR_brk
#define SYS_clone         __NR_clone
#define SYS_fork          __NR_fork
#define SYS_execve        __NR_execve
#define SYS_wait4         __NR_wait4
#define SYS_kill          __NR_kill
#define SYS_getpid        __NR_getpid
#define SYS_socket        __NR_socket
#define SYS_clock_gettime __NR_clock_gettime
#define SYS_nanosleep     __NR_nanosleep

#endif
