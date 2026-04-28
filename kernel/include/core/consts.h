#ifndef _CONSTS_H
#define _CONSTS_H

#include "core/types.h"

/* ---------- Generic page size (same on all supported archs) ---------- */
#define PAGE_SIZE          4096UL
#define PAGE_SIZE_BITS     12
#define PAGE_OFFSET_MASK   ((1UL << PAGE_SIZE_BITS) - 1)
#define PMD_SHIFT          21
#define PMD_SIZE           (1UL << PMD_SHIFT)
#define PMD_ORDER          (PMD_SHIFT - PAGE_SIZE_BITS)
#define PMD_PAGE_COUNT     (PMD_SIZE / PAGE_SIZE)

/* ---------- Kernel / user stack sizes ---------- */
#define KERNEL_STACK_SIZE        (64 * 1024)
#define USER_STACK_INITIAL_PAGES 16
#define USER_STACK_MAX_SIZE      (8 * 1024 * 1024UL)

/* ---------- Limits ---------- */
#define MAX_PROCS          256
#define MAX_FILES          256
#define MAX_PATH_LEN       512
#define MAX_NAME_LEN       256
#define MAX_ARGS           256
#define MAX_ARG_STRLEN     (128 * 1024)
#define MAX_ARG_STRINGS    256
#define MAX_ARG_BYTES      (USER_STACK_MAX_SIZE / 4)
#define MAX_CMD_LEN        4096
#define MAX_HISTORY        256
#define MAX_GROUPS         32

/* The kernel allocator consumes all free RAM via the buddy + slab allocators. */

/*
 * NOTE: The following have been MOVED to arch-specific headers:
 *
 *   From platform.h:  PHYS_MEMORY_BASE, PHYS_MEMORY_END, KERNEL_ENTRY,
 *                      PAGE_OFFSET, UART0_BASE, CLINT_BASE, VIRTIO_BASE,
 *                      PLIC_BASE, UART0_IRQ, PLIC_*, CLINT_*,
 *                      IRQ_S_*, CAUSE_*, CAUSE_INTR/CODE_MASK,
 *                      SIE_*, SSTATUS_*, boot_pgdir
 *
 *   From arch/mm.h:    PTE_V/R/W/X/U/G/A/D/COW, PTE_KERN, PTE_USER,
 *                      ARCH_PT_*, arch_pte_*, arch_make_satp
 *
 *   From arch/trap.h:  TRAP_CONTEXT_SIZE, TASK_CONTEXT_SIZE,
 *                      KTRAP_CONTEXT_SIZE, trap_context_t, task_context_t
 *
 *   From arch/cpu.h:   arch_mb/rmb/wmb, arch_wfi, arch_* (CSR accessors)
 *
 *   From arch/console.h: arch_uart_poll_getc
 *
 *   From firmware.h:   sbi_* / firmware_* function prototypes
 *
 * All kernel code should include "arch.h" (via "defs.h") to get these.
 */

#define SYS_getcwd         17
#define SYS_dup            23
#define SYS_dup3           24
#define SYS_fcntl          25
#define SYS_mkdirat        34
#define SYS_flock          32
#define SYS_unlinkat       35
#define SYS_linkat         37
#define SYS_inotify_init1  26
#define SYS_mknodat        33
#define SYS_umount2        39
#define SYS_utimensat      88
#define SYS_mount          40
#define SYS_statfs         43
#define SYS_fstatfs        44
#define SYS_truncate       45
#define SYS_ftruncate      46
#define SYS_fallocate      47
#define SYS_fchmod         52
#define SYS_fchmodat       53
#define SYS_fchownat       54
#define SYS_fchown         55
#define SYS_faccessat      48
#define SYS_chdir          49
#define SYS_fchdir         50
#define SYS_openat         56
#define SYS_close          57
#define SYS_pipe2          59
#define SYS_getdents64     61
#define SYS_lseek          62
#define SYS_read           63
#define SYS_write          64
#define SYS_readv          65
#define SYS_writev         66
#define SYS_pread64        67
#define SYS_pwrite64       68
#define SYS_preadv         69
#define SYS_pwritev        70
#define SYS_sendfile       71
#define SYS_select         72
#define SYS_pselect6       72
#define SYS_ppoll          73
#define SYS_vmsplice       75
#define SYS_splice         76
#define SYS_tee            77
#define SYS_epoll_create1  20
#define SYS_epoll_ctl      21
#define SYS_epoll_pwait    22
#define SYS_symlinkat      36
#define SYS_sync           81
#define SYS_fsync          82
#define SYS_fdatasync      83
#define SYS_timerfd_create 85
#define SYS_timerfd_settime 86
#define SYS_timerfd_gettime 87
#define SYS_capget         90
#define SYS_capset         91
#define SYS_ioctl          29
#define SYS_readlinkat     78
#define SYS_fstatat        79
#define SYS_fstat          80
#define SYS_exit           93
#define SYS_exit_group     94
#define SYS_waitid         95
#define SYS_set_tid_address 96
#define SYS_set_robust_list 99
#define SYS_futex          98
#define SYS_getitimer      102
#define SYS_setitimer      103
#define SYS_timer_create   107
#define SYS_timer_gettime  108
#define SYS_timer_getoverrun 109
#define SYS_timer_settime  110
#define SYS_timer_delete   111
#define SYS_nanosleep      101
#define SYS_clock_settime  112
#define SYS_clock_gettime  113
#define SYS_clock_getres   114
#define SYS_clock_nanosleep 115
#define SYS_sched_yield    124
#define SYS_sched_setparam 118
#define SYS_sched_setscheduler 119
#define SYS_sched_getscheduler 120
#define SYS_sched_getparam 121
#define SYS_sched_setaffinity 122
#define SYS_sched_getaffinity 123
#define SYS_sched_get_priority_max 125
#define SYS_sched_get_priority_min 126
#define SYS_sched_rr_get_interval 127
#define SYS_kill           129
#define SYS_reboot         142
#define SYS_tkill          130
#define SYS_tgkill         131
#define SYS_sigaction      134
#define SYS_sigprocmask    135
#define SYS_sigtimedwait   137
#define SYS_sigreturn      139
#define SYS_sigsuspend     133
#define SYS_rt_sigqueueinfo 138
#define SYS_setpriority    140
#define SYS_getpriority    141
#define SYS_setregid       143
#define SYS_setgid         144
#define SYS_setreuid       145
#define SYS_setuid         146
#define SYS_setresuid      147
#define SYS_getresuid      148
#define SYS_setresgid      149
#define SYS_getresgid      150
#define SYS_setfsuid       151
#define SYS_setfsgid       152
#define SYS_setpgid        154
#define SYS_getpgid        155
#define SYS_setsid         157
#define SYS_getgroups      158
#define SYS_uname          160
#define SYS_syslog         116
#define SYS_gettimeofday   169
#define SYS_settimeofday   170
#define SYS_times          153
#define SYS_time           235
#define SYS_socket         198
#define SYS_socketpair     199
#define SYS_bind           200
#define SYS_listen         201
#define SYS_accept         202
#define SYS_connect        203
#define SYS_getsockname    204
#define SYS_getpeername    205
#define SYS_sendto         206
#define SYS_recvfrom       207
#define SYS_setsockopt     208
#define SYS_getsockopt     209
#define SYS_shutdown       210
#define SYS_sendmsg        211
#define SYS_recvmsg        212
#define SYS_accept4        242
#define SYS_recvmmsg       243
#define SYS_getrlimit      163
#define SYS_setrlimit      164
#define SYS_getrusage      165
#define SYS_umask          166
#define SYS_prctl          167
#define SYS_getpid         172
#define SYS_getppid        173
#define SYS_getuid         174
#define SYS_geteuid        175
#define SYS_getgid         176
#define SYS_getegid        177
#define SYS_gettid         178
#define SYS_sysinfo        179
#define SYS_brk            214
#define SYS_munmap         215
#define SYS_mremap         216
#define SYS_mmap           222
#define SYS_mprotect       226
#define SYS_madvise        233
#define SYS_clone          220
#define SYS_execve         221
#define SYS_wait4          260
#define SYS_prlimit64      261
#define SYS_renameat2      276
#define SYS_getrandom      278
#define SYS_memfd_create   279
#define SYS_bpf            280
#define SYS_execveat       281
#define SYS_copy_file_range 285
#define SYS_preadv2        286
#define SYS_pwritev2       287
#define SYS_statx          291
#define SYS_close_range    436
#define SYS_shm_open       1021
#define SYS_setgroups      159
#define SYS_clock_adjtime  266
#define SYS_sendmmsg       269
#define SYS_membarrier     283
#define SYS_adjtimex       171
#define SYS_alarm          1022
#define SYS_chroot         51
#define SYS_eventfd2       19
#define SYS_fadvise64      223
#define SYS_shmget         194
#define SYS_shmctl         195
#define SYS_shmat          196
#define SYS_shmdt          197
#define SYS_setxattr       5
#define SYS_lsetxattr      6
#define SYS_getxattr       8
#define SYS_lgetxattr      9
#define SYS_listxattr      11
#define SYS_llistxattr     12
#define SYS_removexattr    14
#define SYS_lremovexattr   15
#define SYS_fremovexattr   16
#define SYS_fsetxattr      7
#define SYS_fgetxattr      10
#define SYS_flistxattr     13

#define EPERM        1
#define ENOENT       2
#define E2BIG        7
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define EBADF        9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define ENFILE       23
#define EMFILE       24
#define ENOSPC       28
#define ESPIPE       29
#define EROFS        30
#define EPIPE        32
#define ENOSYS       38
#define ENOEXEC      8
#define ENOTEMPTY    39
#define ELOOP        40
#define ENAMETOOLONG 36
#define ERANGE       34
#define ENOTTY       25
#define EOPNOTSUPP   95
#define EAFNOSUPPORT 97
#define ENOTSOCK     88
#define EMSGSIZE     90
#define EPROTOTYPE   91
#define EPROTONOSUPPORT 93
#define EADDRINUSE   98
#define EADDRNOTAVAIL 99
#define ENETUNREACH  101
#define EISCONN      106
#define ENOTCONN     107
#define EDESTADDRREQ 89
#define ECONNREFUSED 111
#define ETIMEDOUT    110
#define ENODATA      61
#define ENOLCK       37

#define FT_REGULAR    1
#define FT_DIRECTORY  2
#define FT_CHAR_DEV   3
#define FT_BLOCK_DEV  4
#define FT_PIPE       5
#define FT_SYMLINK    6

#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_ACCMODE    3
#define O_CREAT      0x40
#define O_TRUNC      0x200
#define O_APPEND     0x400
#define O_CLOEXEC    0x80000
#define O_DIRECTORY  0x10000
#define O_NONBLOCK   0x800
#define O_EXCL       0x80
#define O_PATH       0x200000

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define AT_FDCWD       (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR   0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_EMPTY_PATH  0x1000
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_EACCESS     0x200

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

/* statx mask bits (Linux ABI) */
#define STATX_TYPE         0x0001U
#define STATX_MODE         0x0002U
#define STATX_NLINK        0x0004U
#define STATX_UID          0x0008U
#define STATX_GID          0x0010U
#define STATX_ATIME        0x0020U
#define STATX_MTIME        0x0040U
#define STATX_CTIME        0x0080U
#define STATX_INO          0x0100U
#define STATX_SIZE         0x0200U
#define STATX_BLOCKS       0x0400U
#define STATX_BASIC_STATS  0x07ffU
#define STATX_BTIME        0x0800U
#define STATX_ALL          0x0fffU
#define AT_STATX_SYNC_TYPE 0x6000
#define AT_STATX_FORCE_SYNC 0x2000
#define AT_STATX_DONT_SYNC 0x4000

/* LoongArch uses faccessat2 instead of faccessat */
#define SYS_faccessat2  439
#define SYS_fchmodat2   452

#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

#define SIGHUP        1
#define SIGINT        2
#define SIGQUIT       3
#define SIGILL        4
#define SIGABRT       6
#define SIGKILL       9
#define SIGSEGV      11
#define SIGPIPE      13
#define SIGALRM      14
#define SIGTERM      15
#define SIGCHLD      17
#define SIGSTOP      19
#define SIGTSTP      20
#define SIGUSR1      10
#define SIGUSR2      12

/* Memory protection flags */
#define PROT_READ      1
#define PROT_WRITE     2
#define PROT_EXEC      4

/* Memory mapping flags */
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_FIXED      0x10
#define MAP_ANONYMOUS  0x20
#define MAP_POPULATE   0x8000
#define MAP_STACK      0x20000
#define MAP_HUGETLB    0x40000
#define MAP_FIXED_NOREPLACE 0x100000

/* mremap flags */
#define MREMAP_MAYMOVE    1
#define MREMAP_FIXED      2
#define MREMAP_DONTUNMAP  4

/* madvise advice values */
#define MADV_NORMAL      0
#define MADV_RANDOM      1
#define MADV_SEQUENTIAL  2
#define MADV_WILLNEED    3
#define MADV_DONTNEED    4
#define MADV_FREE        8
#define MADV_REMOVE      9
#define MADV_DONTFORK    10
#define MADV_DOFORK      11
#define MADV_MERGEABLE   12
#define MADV_UNMERGEABLE 13
#define MADV_HUGEPAGE    14
#define MADV_NOHUGEPAGE  15
#define MADV_DONTDUMP    16
#define MADV_DODUMP      17
#define MADV_WIPEONFORK  18
#define MADV_KEEPONFORK  19
#define MADV_COLD        20
#define MADV_PAGEOUT     21
#define MADV_POPULATE_READ 22
#define MADV_POPULATE_WRITE 23

/* prctl operations used by common libc/LTP probes */
#define PR_SET_NAME          15
#define PR_CAPBSET_READ      23
#define PR_CAPBSET_DROP      24
#define PR_SET_THP_DISABLE   41
#define PR_GET_THP_DISABLE   42

/* Resource limits */
#define RLIMIT_STACK   3
#define RLIMIT_NOFILE  7

/* Poll events */
#define POLLIN         0x001
#define POLLOUT        0x004
#define POLLERR        0x008
#define POLLHUP        0x010
#define POLLNVAL       0x020

/* Futex operations */
#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_REQUEUE      3
#define FUTEX_CMP_REQUEUE  4
#define FUTEX_WAIT_BITSET  9
#define FUTEX_WAKE_BITSET  10
#define FUTEX_CMD_MASK     0x7F
#define FUTEX_CLOCK_REALTIME 0x100
#define FUTEX_BITSET_MATCH_ANY 0xffffffffU

/* Filesystem magic */
#define EXT4_SUPER_MAGIC  0x4006

/* Process mapping */
#define MMAP_BASE_ADDR    0x60000000UL
#define USER_STACK_TOP    0x3FFFF000UL
#define USER_DYN_BASE     0x10000UL
#define USER_TLS_BASE     0x3E000000UL
#define INTERP_BASE_ADDR  0x40000000UL

/* Terminal ioctl */
#define TCGETS        0x5401
#define TCSETS        0x5402
#define TCSETSW       0x5403
#define TCSETSF       0x5404
#define TIOCGWINSZ    0x5413

/* Misc */
#define PIPE_BUF_SIZE 4096
#define FIRST_USER_FD 3

/* fcntl commands */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define F_GETLK   5
#define F_SETLK   6
#define F_SETLKW  7
#define F_SETOWN  8
#define F_GETOWN  9
#define F_SETSIG  10
#define F_GETSIG  11
#define F_SETOWN_EX 15
#define F_GETOWN_EX 16
#define F_GETOWNER_UIDS 17
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#define F_DUPFD_CLOEXEC 1030
#define F_SETLEASE 1024
#define F_GETLEASE 1025
#define F_NOTIFY 1026
#define F_CANCELLK 1029
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_GET_RW_HINT 1035
#define F_SET_RW_HINT 1036
#define F_GET_FILE_RW_HINT 1037
#define F_SET_FILE_RW_HINT 1038
#define FD_CLOEXEC 1

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define F_OWNER_TID  0
#define F_OWNER_PID  1
#define F_OWNER_PGRP 2
#define F_SEAL_SEAL         0x0001
#define F_SEAL_SHRINK       0x0002
#define F_SEAL_GROW         0x0004
#define F_SEAL_WRITE        0x0008
#define F_SEAL_FUTURE_WRITE 0x0010

/* Dirent types */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10

#endif /* _CONSTS_H */
