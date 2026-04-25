#ifndef _CONSTS_H
#define _CONSTS_H

#include "core/types.h"

/* ---------- Generic page size (same on all supported archs) ---------- */
#define PAGE_SIZE          4096UL
#define PAGE_SIZE_BITS     12
#define PAGE_OFFSET_MASK   ((1UL << PAGE_SIZE_BITS) - 1)

/* ---------- Kernel / user stack sizes ---------- */
#define KERNEL_STACK_SIZE        (64 * 1024)
#define USER_STACK_INITIAL_PAGES 16
#define USER_STACK_MAX_SIZE      (8 * 1024 * 1024UL)

/* ---------- Limits ---------- */
#define MAX_PROCS          64
#define MAX_FILES          64
#define MAX_PATH_LEN       512
#define MAX_NAME_LEN       256
#define MAX_ARGS           256
#define MAX_CMD_LEN        4096
#define MAX_HISTORY        256

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
#define SYS_unlinkat       35
#define SYS_linkat         37
#define SYS_umount2        39
#define SYS_utimensat      88
#define SYS_mount          40
#define SYS_statfs         43
#define SYS_fstatfs        44
#define SYS_truncate       45
#define SYS_ftruncate      46
#define SYS_faccessat      48
#define SYS_chdir          49
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
#define SYS_sendfile       71
#define SYS_select         72
#define SYS_ppoll          73
#define SYS_epoll_create1  20
#define SYS_symlinkat      36
#define SYS_sync           81
#define SYS_fsync          82
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
#define SYS_nanosleep      101
#define SYS_clock_settime  112
#define SYS_clock_gettime  113
#define SYS_clock_getres   114
#define SYS_sched_yield    124
#define SYS_kill           129
#define SYS_reboot         142
#define SYS_tgkill         131
#define SYS_sigaction      134
#define SYS_sigprocmask    135
#define SYS_sigtimedwait   137
#define SYS_sigreturn      139
#define SYS_sigsuspend     133
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
#define SYS_statx          291
#define SYS_shm_open       1021
#define SYS_setgroups      159

#define EPERM        1
#define ENOENT       2
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
#define ENAMETOOLONG 63
#define ERANGE       34
#define ENOTTY       25
#define EOPNOTSUPP   95
#define EAFNOSUPPORT 97
#define ENOTSOCK     88
#define EADDRINUSE   98
#define ENETUNREACH  101

#define FT_REGULAR    1
#define FT_DIRECTORY  2
#define FT_CHAR_DEV   3
#define FT_BLOCK_DEV  4
#define FT_PIPE       5
#define FT_SYMLINK    6

#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_CREAT      0x40
#define O_TRUNC      0x200
#define O_APPEND     0x400
#define O_DIRECTORY  0x10000

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define AT_FDCWD       (-100)
#define AT_REMOVEDIR   0x200
#define AT_EMPTY_PATH  0x1000
#define AT_SYMLINK_NOFOLLOW 0x100

/* statx mask bits (Linux ABI) */
#define STATX_BASIC_STATS  0x07ffU

/* LoongArch uses faccessat2 instead of faccessat */
#define SYS_faccessat2  439

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

/* Resource limits */
#define RLIMIT_STACK   3
#define RLIMIT_NOFILE  7

/* Poll events */
#define POLLIN         0x001
#define POLLOUT        0x004
#define POLLERR        0x008

/* Futex operations */
#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_WAIT_BITSET  9
#define FUTEX_WAKE_BITSET  10
#define FUTEX_CMD_MASK     0x7F

/* Prctl operations */
#define PR_SET_NAME    15

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

/* Dirent types */
#define DT_UNKNOWN  0
#define DT_FIFO     1
#define DT_CHR      2
#define DT_DIR      4
#define DT_BLK      6
#define DT_REG      8
#define DT_LNK     10

#endif /* _CONSTS_H */
