/*
 * x86_64 Linux syscall numbers -> kernel internal (riscv64-ABI) mapping.
 *
 * x86_64 uses a completely different syscall number space from the riscv64 /
 * generic ABI used internally by A20OS.  This header provides a translation
 * table so that user programs compiled for x86_64 Linux work transparently.
 *
 * Generated from:
 *   /usr/include/x86_64-linux-gnu/asm/unistd_64.h  (Linux 5.x)
 *   cross-referenced with kernel/include/abi/linux/syscall_nr.h
 *
 * Format: { x86_nr, kernel_nr }
 * x86_nr   = native x86_64 Linux syscall number
 * kernel_nr = SYS_xxx value from syscall_nr.h (generic/riscv64 ABI)
 *
 * Entries with kernel_nr == (uint16_t)-1 are NOT currently supported.
 */

#ifndef _ARCH_X86_64_SYSCALL_NR_H
#define _ARCH_X86_64_SYSCALL_NR_H

#include "abi/linux/syscall_nr.h"

/* x86_64 native syscall numbers */
#define X86_SYS_read            0
#define X86_SYS_write           1
#define X86_SYS_open            2   /* -> openat */
#define X86_SYS_close           3
#define X86_SYS_stat            4   /* -> fstatat */
#define X86_SYS_fstat           5
#define X86_SYS_lstat           6   /* -> fstatat */
#define X86_SYS_poll            7   /* -> ppoll */
#define X86_SYS_lseek           8
#define X86_SYS_mmap            9
#define X86_SYS_mprotect        10
#define X86_SYS_munmap          11
#define X86_SYS_brk             12
#define X86_SYS_rt_sigaction    13
#define X86_SYS_rt_sigprocmask  14
#define X86_SYS_rt_sigreturn    15
#define X86_SYS_ioctl           16
#define X86_SYS_pread64         17
#define X86_SYS_pwrite64        18
#define X86_SYS_readv           19
#define X86_SYS_writev          20
#define X86_SYS_access          21  /* -> faccessat */
#define X86_SYS_pipe            22  /* -> pipe2 */
#define X86_SYS_select          23
#define X86_SYS_sched_yield     24
#define X86_SYS_mremap          25
#define X86_SYS_msync           26
#define X86_SYS_mincore         27
#define X86_SYS_madvise         28
#define X86_SYS_shmget          29
#define X86_SYS_shmat           30
#define X86_SYS_shmctl          31
#define X86_SYS_dup             32
#define X86_SYS_dup2            33  /* -> dup3 */
#define X86_SYS_pause           34
#define X86_SYS_nanosleep       35
#define X86_SYS_getitimer       36
#define X86_SYS_alarm           37
#define X86_SYS_setitimer       38
#define X86_SYS_getpid          39
#define X86_SYS_sendfile        40
#define X86_SYS_socket          41
#define X86_SYS_connect         42
#define X86_SYS_accept          43
#define X86_SYS_sendto          44
#define X86_SYS_recvfrom        45
#define X86_SYS_sendmsg         46
#define X86_SYS_recvmsg         47
#define X86_SYS_shutdown        48
#define X86_SYS_bind            49
#define X86_SYS_listen          50
#define X86_SYS_getsockname     51
#define X86_SYS_getpeername     52
#define X86_SYS_socketpair      53
#define X86_SYS_setsockopt      54
#define X86_SYS_getsockopt      55
#define X86_SYS_clone           56
#define X86_SYS_fork            57
#define X86_SYS_vfork           58
#define X86_SYS_execve          59
#define X86_SYS_exit            60
#define X86_SYS_wait4           61
#define X86_SYS_kill            62
#define X86_SYS_uname           63
#define X86_SYS_semget          64
#define X86_SYS_semop           65
#define X86_SYS_semctl          66
#define X86_SYS_shmdt           67
#define X86_SYS_msgget          68
#define X86_SYS_msgsnd          69
#define X86_SYS_msgrcv          70
#define X86_SYS_msgctl          71
#define X86_SYS_fcntl           72
#define X86_SYS_flock           73
#define X86_SYS_fsync           74
#define X86_SYS_fdatasync       75
#define X86_SYS_truncate        76
#define X86_SYS_ftruncate       77
#define X86_SYS_getdents        78  /* -> getdents64 */
#define X86_SYS_getcwd          79
#define X86_SYS_chdir           80
#define X86_SYS_fchdir          81
#define X86_SYS_rename          82  /* -> renameat2 */
#define X86_SYS_mkdir           83  /* -> mkdirat */
#define X86_SYS_rmdir           84  /* -> unlinkat */
#define X86_SYS_creat           85  /* -> openat */
#define X86_SYS_link            86  /* -> linkat */
#define X86_SYS_unlink          87  /* -> unlinkat */
#define X86_SYS_symlink         88  /* -> symlinkat */
#define X86_SYS_readlink        89  /* -> readlinkat */
#define X86_SYS_chmod           90  /* -> fchmodat */
#define X86_SYS_fchmod          91
#define X86_SYS_chown           92  /* -> fchownat */
#define X86_SYS_fchown          93
#define X86_SYS_lchown          94  /* -> fchownat */
#define X86_SYS_umask           95
#define X86_SYS_gettimeofday    96
#define X86_SYS_getrlimit       97
#define X86_SYS_getrusage       98
#define X86_SYS_sysinfo         99
#define X86_SYS_times           100
#define X86_SYS_ptrace          101
#define X86_SYS_getuid          102
#define X86_SYS_syslog          103
#define X86_SYS_getgid          104
#define X86_SYS_setuid          105
#define X86_SYS_setgid          106
#define X86_SYS_geteuid        107
#define X86_SYS_getegid         108
#define X86_SYS_setpgid         109
#define X86_SYS_getppid         110
#define X86_SYS_getpgrp         111
#define X86_SYS_setsid          112
#define X86_SYS_setreuid        113
#define X86_SYS_setregid        114
#define X86_SYS_getgroups       115
#define X86_SYS_setgroups       116
#define X86_SYS_setresuid       117
#define X86_SYS_getresuid       118
#define X86_SYS_setresgid       119
#define X86_SYS_getresgid       120
#define X86_SYS_getpgid         121
#define X86_SYS_setfsuid        122
#define X86_SYS_setfsgid        123
#define X86_SYS_getsid          124
#define X86_SYS_capget          125
#define X86_SYS_capset          126
#define X86_SYS_rt_sigpending   127
#define X86_SYS_rt_sigtimedwait 128
#define X86_SYS_rt_sigqueueinfo 129
#define X86_SYS_rt_sigsuspend   130
#define X86_SYS_sigaltstack     131
#define X86_SYS_utime           132
#define X86_SYS_mknod           133  /* -> mknodat */
#define X86_SYS_personality     135
#define X86_SYS_statfs          137
#define X86_SYS_fstatfs         138
#define X86_SYS_sysfs          139
#define X86_SYS_getpriority     140
#define X86_SYS_setpriority     141
#define X86_SYS_sched_setparam  142
#define X86_SYS_sched_getparam  143
#define X86_SYS_sched_setscheduler 144
#define X86_SYS_sched_getscheduler 145
#define X86_SYS_sched_get_priority_max 146
#define X86_SYS_sched_get_priority_min 147
#define X86_SYS_sched_rr_get_interval 148
#define X86_SYS_mlock           149
#define X86_SYS_munlock         150
#define X86_SYS_mlockall        151
#define X86_SYS_munlockall      152
#define X86_SYS_vhangup         153
#define X86_SYS_pivot_root      155
#define X86_SYS_prctl           157
#define X86_SYS_arch_prctl      158
#define X86_SYS_adjtimex        159
#define X86_SYS_setrlimit       160
#define X86_SYS_chroot          161
#define X86_SYS_sync            162
#define X86_SYS_acct            163
#define X86_SYS_settimeofday    164
#define X86_SYS_mount           165
#define X86_SYS_umount2         166
#define X86_SYS_swapon          167
#define X86_SYS_swapoff         168
#define X86_SYS_reboot          169
#define X86_SYS_sethostname     170
#define X86_SYS_setdomainname   171
#define X86_SYS_time            201
#define X86_SYS_futex           202
#define X86_SYS_sched_setaffinity 203
#define X86_SYS_sched_getaffinity 204
#define X86_SYS_io_setup        206
#define X86_SYS_io_destroy      207
#define X86_SYS_io_getevents    208
#define X86_SYS_io_submit       209
#define X86_SYS_io_cancel       210
#define X86_SYS_set_tid_address 218
#define X86_SYS_restart_syscall 219
#define X86_SYS_semtimedop      220
#define X86_SYS_timer_create    222
#define X86_SYS_timer_settime   223
#define X86_SYS_timer_gettime   224
#define X86_SYS_timer_getoverrun 225
#define X86_SYS_timer_delete    226
#define X86_SYS_clock_settime   227
#define X86_SYS_clock_gettime   228
#define X86_SYS_clock_getres    229
#define X86_SYS_clock_nanosleep 230
#define X86_SYS_exit_group      231
#define X86_SYS_waitid          247
#define X86_SYS_set_robust_list 273
#define X86_SYS_get_robust_list 274
#define X86_SYS_epoll_create    213
#define X86_SYS_epoll_ctl       232
#define X86_SYS_epoll_wait      232
#define X86_SYS_epoll_pwait     281
#define X86_SYS_epoll_create1   291
#define X86_SYS_dup3            292
#define X86_SYS_pipe2           293
#define X86_SYS_inotify_init    253
#define X86_SYS_inotify_init1   294
#define X86_SYS_inotify_add_watch 254
#define X86_SYS_inotify_rm_watch 255
#define X86_SYS_openat          257
#define X86_SYS_mkdirat         258
#define X86_SYS_mknodat         259
#define X86_SYS_fchownat        260
#define X86_SYS_futimesat       261
#define X86_SYS_fstatat         262 /* newfstatat */
#define X86_SYS_unlinkat        263
#define X86_SYS_renameat        264
#define X86_SYS_linkat          265
#define X86_SYS_symlinkat       266
#define X86_SYS_readlinkat      267
#define X86_SYS_fchmodat        268
#define X86_SYS_faccessat       269
#define X86_SYS_pselect6        270
#define X86_SYS_ppoll           271
#define X86_SYS_unshare         272
#define X86_SYS_splice          275
#define X86_SYS_tee             276
#define X86_SYS_sync_file_range 277
#define X86_SYS_vmsplice        278
#define X86_SYS_utimensat       280
#define X86_SYS_epoll_pwait2    (X86_SYSCALL_TABLE_SIZE-1)
#define X86_SYS_signalfd        282
#define X86_SYS_timerfd_create  283
#define X86_SYS_eventfd         284
#define X86_SYS_fallocate       285
#define X86_SYS_timerfd_settime 286
#define X86_SYS_timerfd_gettime 287
#define X86_SYS_accept4         288
#define X86_SYS_signalfd4       289
#define X86_SYS_eventfd2        290
#define X86_SYS_preadv          295
#define X86_SYS_pwritev         296
#define X86_SYS_perf_event_open 298
#define X86_SYS_recvmmsg        299
#define X86_SYS_prlimit64       302
#define X86_SYS_clock_adjtime   305
#define X86_SYS_syncfs          306
#define X86_SYS_sendmmsg        307
#define X86_SYS_setns           308
#define X86_SYS_getcpu          309
#define X86_SYS_getdents64      217
#define X86_SYS_getrandom       318
#define X86_SYS_memfd_create    319
#define X86_SYS_bpf             321
#define X86_SYS_execveat        322
#define X86_SYS_userfaultfd     323
#define X86_SYS_membarrier      324
#define X86_SYS_mlock2          325
#define X86_SYS_copy_file_range 326
#define X86_SYS_preadv2         327
#define X86_SYS_pwritev2        328
#define X86_SYS_statx           332
#define X86_SYS_pidfd_send_signal 424
#define X86_SYS_clone3          435
#define X86_SYS_close_range     436
#define X86_SYS_openat2         437
#define X86_SYS_faccessat2      439
#define X86_SYS_renameat2       316
#define X86_SYS_add_key         248
#define X86_SYS_request_key     249
#define X86_SYS_keyctl          250
#define X86_SYS_fanotify_init   300
#define X86_SYS_fanotify_mark   301
#define X86_SYS_setxattr        188
#define X86_SYS_lsetxattr       189
#define X86_SYS_fsetxattr       190
#define X86_SYS_getxattr        191
#define X86_SYS_lgetxattr       192
#define X86_SYS_fgetxattr       193
#define X86_SYS_listxattr       194
#define X86_SYS_llistxattr      195
#define X86_SYS_flistxattr      196
#define X86_SYS_removexattr     197
#define X86_SYS_lremovexattr    198
#define X86_SYS_fremovexattr    199
#define X86_SYS_sched_setattr   314
#define X86_SYS_sched_getattr   315
#define X86_SYS_memfd_secret    447
#define X86_SYS_fchmodat2       452

#define X86_SYSCALL_TABLE_SIZE  453

/* Map x86_64 syscall nr -> kernel internal syscall nr */
static inline uint32_t x86_syscall_to_kernel_nr(uint32_t x86_nr)
{
    /* Indexed lookup for the densely-used range 0..332. */
    static const uint16_t x86_to_kernel[X86_SYSCALL_TABLE_SIZE] = {
        /* 0  */ SYS_read,
        /* 1  */ SYS_write,
        /* 2  */ SYS_openat,        /* open -> openat */
        /* 3  */ SYS_close,
        /* 4  */ SYS_fstatat,       /* stat -> fstatat */
        /* 5  */ SYS_fstat,
        /* 6  */ SYS_fstatat,       /* lstat -> fstatat */
        /* 7  */ SYS_ppoll,         /* poll -> ppoll */
        /* 8  */ SYS_lseek,
        /* 9  */ SYS_mmap,
        /* 10 */ SYS_mprotect,
        /* 11 */ SYS_munmap,
        /* 12 */ SYS_brk,
        /* 13 */ SYS_sigaction,     /* rt_sigaction */
        /* 14 */ SYS_sigprocmask,   /* rt_sigprocmask */
        /* 15 */ SYS_sigreturn,     /* rt_sigreturn */
        /* 16 */ SYS_ioctl,
        /* 17 */ SYS_pread64,
        /* 18 */ SYS_pwrite64,
        /* 19 */ SYS_readv,
        /* 20 */ SYS_writev,
        /* 21 */ SYS_faccessat,     /* access -> faccessat */
        /* 22 */ SYS_pipe2,         /* pipe -> pipe2 */
        /* 23 */ SYS_select,
        /* 24 */ SYS_sched_yield,
        /* 25 */ SYS_mremap,
        /* 26 */ SYS_msync,
        /* 27 */ SYS_mincore,
        /* 28 */ SYS_madvise,
        /* 29 */ SYS_shmget,
        /* 30 */ SYS_shmat,
        /* 31 */ SYS_shmctl,
        /* 32 */ SYS_dup,
        /* 33 */ SYS_dup3,          /* dup2 -> dup3 */
        /* 34 */ (uint16_t)-1,      /* pause - not impl */
        /* 35 */ SYS_nanosleep,
        /* 36 */ SYS_getitimer,
        /* 37 */ SYS_alarm,
        /* 38 */ SYS_setitimer,
        /* 39 */ SYS_getpid,
        /* 40 */ SYS_sendfile,
        /* 41 */ SYS_socket,
        /* 42 */ SYS_connect,
        /* 43 */ SYS_accept,
        /* 44 */ SYS_sendto,
        /* 45 */ SYS_recvfrom,
        /* 46 */ SYS_sendmsg,
        /* 47 */ SYS_recvmsg,
        /* 48 */ SYS_shutdown,
        /* 49 */ SYS_bind,
        /* 50 */ SYS_listen,
        /* 51 */ SYS_getsockname,
        /* 52 */ SYS_getpeername,
        /* 53 */ SYS_socketpair,
        /* 54 */ SYS_setsockopt,
        /* 55 */ SYS_getsockopt,
        /* 56 */ SYS_clone,
        /* 57 */ SYS_clone,         /* fork -> clone */
        /* 58 */ SYS_clone,         /* vfork -> clone */
        /* 59 */ SYS_execve,
        /* 60 */ SYS_exit,
        /* 61 */ SYS_wait4,
        /* 62 */ SYS_kill,
        /* 63 */ SYS_uname,
        /* 64 */ SYS_semget,
        /* 65 */ SYS_semop,
        /* 66 */ SYS_semctl,
        /* 67 */ SYS_shmdt,
        /* 68 */ (uint16_t)-1,      /* msgget */
        /* 69 */ (uint16_t)-1,      /* msgsnd */
        /* 70 */ (uint16_t)-1,      /* msgrcv */
        /* 71 */ (uint16_t)-1,      /* msgctl */
        /* 72 */ SYS_fcntl,
        /* 73 */ SYS_flock,
        /* 74 */ SYS_fsync,
        /* 75 */ SYS_fdatasync,
        /* 76 */ SYS_truncate,
        /* 77 */ SYS_ftruncate,
        /* 78 */ SYS_getdents64,    /* getdents -> getdents64 */
        /* 79 */ SYS_getcwd,
        /* 80 */ SYS_chdir,
        /* 81 */ SYS_fchdir,
        /* 82 */ SYS_renameat2,     /* rename -> renameat2 */
        /* 83 */ SYS_mkdirat,       /* mkdir -> mkdirat */
        /* 84 */ SYS_unlinkat,      /* rmdir -> unlinkat */
        /* 85 */ SYS_openat,        /* creat -> openat */
        /* 86 */ SYS_linkat,        /* link -> linkat */
        /* 87 */ SYS_unlinkat,      /* unlink -> unlinkat */
        /* 88 */ SYS_symlinkat,     /* symlink -> symlinkat */
        /* 89 */ SYS_readlinkat,    /* readlink -> readlinkat */
        /* 90 */ SYS_fchmodat,      /* chmod -> fchmodat */
        /* 91 */ SYS_fchmod,
        /* 92 */ SYS_fchownat,      /* chown -> fchownat */
        /* 93 */ SYS_fchown,
        /* 94 */ SYS_fchownat,      /* lchown -> fchownat */
        /* 95 */ SYS_umask,
        /* 96 */ SYS_gettimeofday,
        /* 97 */ SYS_getrlimit,
        /* 98 */ SYS_getrusage,
        /* 99 */ SYS_sysinfo,
        /* 100 */ SYS_times,
        /* 101 */ (uint16_t)-1,     /* ptrace */
        /* 102 */ SYS_getuid,
        /* 103 */ SYS_syslog,
        /* 104 */ SYS_getgid,
        /* 105 */ SYS_setuid,
        /* 106 */ SYS_setgid,
        /* 107 */ SYS_geteuid,
        /* 108 */ SYS_getegid,
        /* 109 */ SYS_setpgid,
        /* 110 */ SYS_getppid,
        /* 111 */ SYS_getpgid,      /* getpgrp -> getpgid */
        /* 112 */ SYS_setsid,
        /* 113 */ SYS_setreuid,
        /* 114 */ SYS_setregid,
        /* 115 */ SYS_getgroups,
        /* 116 */ SYS_setgroups,
        /* 117 */ SYS_setresuid,
        /* 118 */ SYS_getresuid,
        /* 119 */ SYS_setresgid,
        /* 120 */ SYS_getresgid,
        /* 121 */ SYS_getpgid,
        /* 122 */ SYS_setfsuid,
        /* 123 */ SYS_setfsgid,
        /* 124 */ SYS_getsid,
        /* 125 */ SYS_capget,
        /* 126 */ SYS_capset,
        /* 127 */ SYS_rt_sigpending,
        /* 128 */ SYS_sigtimedwait,
        /* 129 */ SYS_rt_sigqueueinfo,
        /* 130 */ SYS_sigsuspend,
        /* 131 */ SYS_sigaltstack,
        /* 132 */ (uint16_t)-1,     /* utime */
        /* 133 */ SYS_mknodat,      /* mknod -> mknodat */
        /* 134 */ (uint16_t)-1,     /* uselib */
        /* 135 */ SYS_personality,
        /* 136 */ (uint16_t)-1,     /* ustat */
        /* 137 */ SYS_statfs,
        /* 138 */ SYS_fstatfs,
        /* 139 */ (uint16_t)-1,     /* sysfs */
        /* 140 */ SYS_getpriority,
        /* 141 */ SYS_setpriority,
        /* 142 */ SYS_sched_setparam,
        /* 143 */ SYS_sched_getparam,
        /* 144 */ SYS_sched_setscheduler,
        /* 145 */ SYS_sched_getscheduler,
        /* 146 */ SYS_sched_get_priority_max,
        /* 147 */ SYS_sched_get_priority_min,
        /* 148 */ SYS_sched_rr_get_interval,
        /* 149 */ SYS_mlock,
        /* 150 */ SYS_munlock,
        /* 151 */ SYS_mlockall,
        /* 152 */ SYS_munlockall,
        /* 153 */ SYS_vhangup,
        /* 154 */ (uint16_t)-1,     /* modify_ldt */
        /* 155 */ SYS_pivot_root,
        /* 156 */ (uint16_t)-1,     /* _sysctl */
        /* 157 */ SYS_prctl,
        /* 158 */ SYS_arch_prctl,
        /* 159 */ SYS_adjtimex,
        /* 160 */ SYS_setrlimit,
        /* 161 */ SYS_chroot,
        /* 162 */ SYS_sync,
        /* 163 */ SYS_acct,
        /* 164 */ SYS_settimeofday,
        /* 165 */ SYS_mount,
        /* 166 */ SYS_umount2,
        /* 167 */ (uint16_t)-1,     /* swapon */
        /* 168 */ (uint16_t)-1,     /* swapoff */
        /* 169 */ SYS_reboot,
        /* 170 */ SYS_sethostname,
        /* 171 */ SYS_setdomainname,
        /* 172 */ (uint16_t)-1,     /* iopl */
        /* 173 */ (uint16_t)-1,     /* ioperm */
        /* 174 */ (uint16_t)-1,     /* create_module */
        /* 175 */ SYS_init_module,
        /* 176 */ SYS_delete_module,
        /* 177 */ (uint16_t)-1,     /* get_kernel_syms */
        /* 178 */ (uint16_t)-1,     /* query_module */
        /* 179 */ (uint16_t)-1,     /* quotactl */
        /* 180 */ (uint16_t)-1,     /* nfsservctl */
        /* 181 */ (uint16_t)-1,
        /* 182 */ (uint16_t)-1,
        /* 183 */ (uint16_t)-1,
        /* 184 */ (uint16_t)-1,
        /* 185 */ (uint16_t)-1,
        /* 186 */ (uint16_t)-1,
        /* 187 */ (uint16_t)-1,
        /* 188 */ SYS_setxattr,
        /* 189 */ SYS_lsetxattr,
        /* 190 */ SYS_fsetxattr,
        /* 191 */ SYS_getxattr,
        /* 192 */ SYS_lgetxattr,
        /* 193 */ SYS_fgetxattr,
        /* 194 */ SYS_listxattr,
        /* 195 */ SYS_llistxattr,
        /* 196 */ SYS_flistxattr,
        /* 197 */ SYS_removexattr,
        /* 198 */ SYS_lremovexattr,
        /* 199 */ SYS_fremovexattr,
        /* 200 */ (uint16_t)-1,     /* tkill */
        /* 201 */ SYS_time,
        /* 202 */ SYS_futex,
        /* 203 */ SYS_sched_setaffinity,
        /* 204 */ SYS_sched_getaffinity,
        /* 205 */ (uint16_t)-1,     /* set_thread_area */
        /* 206 */ SYS_io_setup,
        /* 207 */ SYS_io_destroy,
        /* 208 */ SYS_io_getevents,
        /* 209 */ SYS_io_submit,
        /* 210 */ SYS_io_cancel,
        /* 211 */ (uint16_t)-1,     /* get_thread_area */
        /* 212 */ (uint16_t)-1,     /* lookup_dcookie */
        /* 213 */ SYS_epoll_create1, /* epoll_create -> epoll_create1 */
        /* 214 */ (uint16_t)-1,
        /* 215 */ (uint16_t)-1,
        /* 216 */ (uint16_t)-1,
        /* 217 */ SYS_getdents64,
        /* 218 */ SYS_set_tid_address,
        /* 219 */ (uint16_t)-1,     /* restart_syscall */
        /* 220 */ SYS_semtimedop,
        /* 221 */ (uint16_t)-1,     /* fadvise64 */
        /* 222 */ SYS_timer_create,
        /* 223 */ SYS_timer_settime,
        /* 224 */ SYS_timer_gettime,
        /* 225 */ SYS_timer_getoverrun,
        /* 226 */ SYS_timer_delete,
        /* 227 */ SYS_clock_settime,
        /* 228 */ SYS_clock_gettime,
        /* 229 */ SYS_clock_getres,
        /* 230 */ SYS_clock_nanosleep,
        /* 231 */ SYS_exit_group,
        /* 232 */ SYS_epoll_ctl,
        /* 233 */ SYS_tgkill,
        /* 234 */ (uint16_t)-1,     /* utimes */
        /* 235 */ (uint16_t)-1,
        /* 236 */ (uint16_t)-1,
        /* 237 */ (uint16_t)-1,
        /* 238 */ (uint16_t)-1,
        /* 239 */ (uint16_t)-1,
        /* 240 */ (uint16_t)-1,
        /* 241 */ SYS_perf_event_open,
        /* 242 */ (uint16_t)-1,
        /* 243 */ (uint16_t)-1,
        /* 244 */ (uint16_t)-1,
        /* 245 */ (uint16_t)-1,
        /* 246 */ (uint16_t)-1,
        /* 247 */ SYS_waitid,
        /* 248 */ SYS_add_key,
        /* 249 */ SYS_request_key,
        /* 250 */ SYS_keyctl,
        /* 251 */ (uint16_t)-1,     /* ioprio_set */
        /* 252 */ (uint16_t)-1,     /* ioprio_get */
        /* 253 */ SYS_inotify_init1,
        /* 254 */ SYS_inotify_add_watch,
        /* 255 */ SYS_inotify_rm_watch,
        /* 256 */ (uint16_t)-1,     /* migrate_pages */
        /* 257 */ SYS_openat,
        /* 258 */ SYS_mkdirat,
        /* 259 */ SYS_mknodat,
        /* 260 */ SYS_fchownat,
        /* 261 */ SYS_utimensat,    /* futimesat -> utimensat */
        /* 262 */ SYS_fstatat,      /* newfstatat */
        /* 263 */ SYS_unlinkat,
        /* 264 */ SYS_renameat2,    /* renameat -> renameat2 */
        /* 265 */ SYS_linkat,
        /* 266 */ SYS_symlinkat,
        /* 267 */ SYS_readlinkat,
        /* 268 */ SYS_fchmodat,
        /* 269 */ SYS_faccessat,
        /* 270 */ SYS_pselect6,
        /* 271 */ SYS_ppoll,
        /* 272 */ SYS_unshare,
        /* 273 */ SYS_set_robust_list,
        /* 274 */ SYS_get_robust_list,
        /* 275 */ SYS_splice,
        /* 276 */ SYS_tee,
        /* 277 */ SYS_sync_file_range,
        /* 278 */ SYS_vmsplice,
        /* 279 */ (uint16_t)-1,     /* move_pages */
        /* 280 */ SYS_utimensat,
        /* 281 */ SYS_epoll_pwait,
        /* 282 */ SYS_signalfd4,    /* signalfd -> signalfd4 */
        /* 283 */ SYS_timerfd_create,
        /* 284 */ (uint16_t)-1,     /* eventfd -> eventfd2 */
        /* 285 */ SYS_fallocate,
        /* 286 */ SYS_timerfd_settime,
        /* 287 */ SYS_timerfd_gettime,
        /* 288 */ SYS_accept4,
        /* 289 */ SYS_signalfd4,
        /* 290 */ SYS_eventfd2,
        /* 291 */ SYS_epoll_create1,
        /* 292 */ SYS_dup3,
        /* 293 */ SYS_pipe2,
        /* 294 */ SYS_inotify_init1,
        /* 295 */ SYS_preadv,
        /* 296 */ SYS_pwritev,
        /* 297 */ (uint16_t)-1,     /* rt_tgsigqueueinfo */
        /* 298 */ SYS_perf_event_open,
        /* 299 */ SYS_recvmmsg,
        /* 300 */ SYS_fanotify_init,
        /* 301 */ SYS_fanotify_mark,
        /* 302 */ SYS_prlimit64,
        /* 303 */ (uint16_t)-1,     /* name_to_handle_at */
        /* 304 */ (uint16_t)-1,     /* open_by_handle_at */
        /* 305 */ SYS_clock_adjtime,
        /* 306 */ SYS_syncfs,
        /* 307 */ SYS_sendmmsg,
        /* 308 */ SYS_setns,
        /* 309 */ (uint16_t)-1,     /* getcpu */
        /* 310 */ (uint16_t)-1,     /* process_vm_readv */
        /* 311 */ (uint16_t)-1,     /* process_vm_writev */
        /* 312 */ (uint16_t)-1,     /* kcmp */
        /* 313 */ SYS_finit_module,
        /* 314 */ SYS_sched_setattr,
        /* 315 */ SYS_sched_getattr,
        /* 316 */ SYS_renameat2,
        /* 317 */ SYS_membarrier,
        /* 318 */ SYS_getrandom,
        /* 319 */ SYS_memfd_create,
        /* 320 */ (uint16_t)-1,     /* kexec_file_load */
        /* 321 */ SYS_bpf,
        /* 322 */ SYS_execveat,
        /* 323 */ SYS_userfaultfd,
        /* 324 */ SYS_membarrier,
        /* 325 */ (uint16_t)-1,     /* mlock2 */
        /* 326 */ SYS_copy_file_range,
        /* 327 */ SYS_preadv2,
        /* 328 */ SYS_pwritev2,
        /* 329 */ (uint16_t)-1,     /* pkey_mprotect */
        /* 330 */ (uint16_t)-1,     /* pkey_alloc */
        /* 331 */ (uint16_t)-1,     /* pkey_free */
        /* 332 */ SYS_statx,
        /* 333 */ (uint16_t)-1,
        /* 334 */ (uint16_t)-1,
        /* 335 */ (uint16_t)-1,
        /* 336 */ (uint16_t)-1,
        /* 337 */ (uint16_t)-1,
        /* 338 */ (uint16_t)-1,
        /* 339 */ (uint16_t)-1,
        /* 340 */ (uint16_t)-1,
        /* 341 */ (uint16_t)-1,
        /* 342 */ (uint16_t)-1,
        /* 343 */ (uint16_t)-1,
        /* 344 */ (uint16_t)-1,
        /* 345 */ (uint16_t)-1,
        /* 346 */ (uint16_t)-1,
        /* 347 */ (uint16_t)-1,
        /* 348 */ (uint16_t)-1,
        /* 349 */ (uint16_t)-1,
        /* 350 */ (uint16_t)-1,
        /* 351 */ (uint16_t)-1,
        /* 352 */ (uint16_t)-1,
        /* 353 */ (uint16_t)-1,
        /* 354 */ (uint16_t)-1,
        /* 355 */ (uint16_t)-1,
        /* 356 */ (uint16_t)-1,
        /* 357 */ (uint16_t)-1,
        /* 358 */ (uint16_t)-1,
        /* 359 */ (uint16_t)-1,
        /* 360 */ (uint16_t)-1,
        /* 361 */ (uint16_t)-1,
        /* 362 */ (uint16_t)-1,
        /* 363 */ (uint16_t)-1,
        /* 364 */ (uint16_t)-1,
        /* 365 */ (uint16_t)-1,
        /* 366 */ (uint16_t)-1,
        /* 367 */ (uint16_t)-1,
        /* 368 */ (uint16_t)-1,
        /* 369 */ (uint16_t)-1,
        /* 370 */ (uint16_t)-1,
        /* 371 */ (uint16_t)-1,
        /* 372 */ (uint16_t)-1,
        /* 373 */ (uint16_t)-1,
        /* 374 */ (uint16_t)-1,
        /* 375 */ (uint16_t)-1,
        /* 376 */ (uint16_t)-1,
        /* 377 */ (uint16_t)-1,
        /* 378 */ (uint16_t)-1,
        /* 379 */ (uint16_t)-1,
        /* 380 */ (uint16_t)-1,
        /* 381 */ (uint16_t)-1,
        /* 382 */ (uint16_t)-1,
        /* 383 */ (uint16_t)-1,
        /* 384 */ (uint16_t)-1,
        /* 385 */ (uint16_t)-1,
        /* 386 */ (uint16_t)-1,
        /* 387 */ (uint16_t)-1,
        /* 388 */ (uint16_t)-1,
        /* 389 */ (uint16_t)-1,
        /* 390 */ (uint16_t)-1,
        /* 391 */ (uint16_t)-1,
        /* 392 */ (uint16_t)-1,
        /* 393 */ (uint16_t)-1,
        /* 394 */ (uint16_t)-1,
        /* 395 */ (uint16_t)-1,
        /* 396 */ (uint16_t)-1,
        /* 397 */ (uint16_t)-1,
        /* 398 */ (uint16_t)-1,
        /* 399 */ (uint16_t)-1,
        /* 400 */ (uint16_t)-1,
        /* 401 */ (uint16_t)-1,
        /* 402 */ (uint16_t)-1,
        /* 403 */ (uint16_t)-1,
        /* 404 */ (uint16_t)-1,
        /* 405 */ (uint16_t)-1,
        /* 406 */ (uint16_t)-1,
        /* 407 */ (uint16_t)-1,
        /* 408 */ (uint16_t)-1,
        /* 409 */ (uint16_t)-1,
        /* 410 */ (uint16_t)-1,
        /* 411 */ (uint16_t)-1,
        /* 412 */ (uint16_t)-1,
        /* 413 */ (uint16_t)-1,
        /* 414 */ (uint16_t)-1,
        /* 415 */ (uint16_t)-1,
        /* 416 */ (uint16_t)-1,
        /* 417 */ (uint16_t)-1,
        /* 418 */ (uint16_t)-1,
        /* 419 */ (uint16_t)-1,
        /* 420 */ (uint16_t)-1,
        /* 421 */ (uint16_t)-1,
        /* 422 */ (uint16_t)-1,
        /* 423 */ (uint16_t)-1,
        /* 424 */ SYS_pidfd_send_signal,
        /* 425 */ (uint16_t)-1,
        /* 426 */ (uint16_t)-1,
        /* 427 */ (uint16_t)-1,
        /* 428 */ (uint16_t)-1,
        /* 429 */ (uint16_t)-1,
        /* 430 */ (uint16_t)-1,
        /* 431 */ (uint16_t)-1,
        /* 432 */ (uint16_t)-1,
        /* 433 */ (uint16_t)-1,
        /* 434 */ (uint16_t)-1,
        /* 435 */ SYS_clone3,
        /* 436 */ SYS_close_range,
        /* 437 */ SYS_openat2,
        /* 438 */ (uint16_t)-1,     /* pidfd_getfd */
        /* 439 */ SYS_faccessat2,
        /* 440 */ (uint16_t)-1,
        /* 441 */ (uint16_t)-1,
        /* 442 */ (uint16_t)-1,
        /* 443 */ (uint16_t)-1,
        /* 444 */ (uint16_t)-1,
        /* 445 */ (uint16_t)-1,
        /* 446 */ (uint16_t)-1,
        /* 447 */ (uint16_t)-1,     /* memfd_secret */
        /* 448 */ (uint16_t)-1,
        /* 449 */ (uint16_t)-1,
        /* 450 */ (uint16_t)-1,
        /* 451 */ (uint16_t)-1,
        /* 452 */ SYS_fchmodat,     /* fchmodat2 -> fchmodat (approx) */
    };

    if (x86_nr >= X86_SYSCALL_TABLE_SIZE)
        return (uint32_t)-1;
    return x86_to_kernel[x86_nr];
}

/*
 * x86_64-specific syscall argument rewriting.
 *
 * Some syscalls have different argument layouts or behaviour on x86_64 vs
 * the generic (riscv64) ABI.  This function is called AFTER the syscall
 * number has been translated; it may patch up the args in-place.
 *
 * Returns 1 if the args were modified, 0 otherwise.
 */
static inline int x86_syscall_rewrite_args(uint32_t x86_nr,
                                           linux_syscall_args_t *args)
{
    switch (x86_nr) {
        case X86_SYS_fork:
            args->arg[0] = 17; /* SIGCHLD */
            args->arg[1] = 0;  /* stack */
            args->arg[2] = 0;  /* ptid */
            args->arg[3] = 0;  /* ctid (r10) */
            args->arg[4] = 0;  /* tls (r8) */
            return 1;
        case X86_SYS_vfork:
            args->arg[0] = 0x4111; /* CLONE_VFORK | CLONE_VM | SIGCHLD */
            args->arg[1] = 0;
            args->arg[2] = 0;
            args->arg[3] = 0;
            args->arg[4] = 0;
            return 1;
        case X86_SYS_open:
        case X86_SYS_creat:
        case X86_SYS_stat:
        case X86_SYS_lstat:
        case X86_SYS_access:
        case X86_SYS_chmod:
        case X86_SYS_chown:
        case X86_SYS_lchown:
        case X86_SYS_mkdir:
        case X86_SYS_rmdir:
        case X86_SYS_unlink:
        case X86_SYS_readlink:
            /* Shift args to the right by 1, inject AT_FDCWD */
            args->arg[4] = args->arg[3];
            args->arg[3] = args->arg[2];
            args->arg[2] = args->arg[1];
            args->arg[1] = args->arg[0];
            args->arg[0] = -100; /* AT_FDCWD */
            
            if (x86_nr == X86_SYS_creat) {
                args->arg[3] = args->arg[2];
                args->arg[2] = 0x40 | 0x200 | 1; /* O_CREAT | O_TRUNC | O_WRONLY */
            } else if (x86_nr == X86_SYS_stat) {
                args->arg[3] = 0;
            } else if (x86_nr == X86_SYS_lstat) {
                args->arg[3] = 0x100; /* AT_SYMLINK_NOFOLLOW */
            } else if (x86_nr == X86_SYS_rmdir) {
                args->arg[2] = 0x200; /* AT_REMOVEDIR */
            } else if (x86_nr == X86_SYS_lchown) {
                args->arg[4] = 0x100; /* AT_SYMLINK_NOFOLLOW */
            } else if (x86_nr == X86_SYS_chown) {
                args->arg[4] = 0;
            } else if (x86_nr == X86_SYS_chmod) {
                args->arg[3] = 0;
            }
            return 1;
        case X86_SYS_link:
        case X86_SYS_rename:
            args->arg[4] = 0;
            args->arg[3] = args->arg[1];
            args->arg[2] = -100; /* AT_FDCWD */
            args->arg[1] = args->arg[0];
            args->arg[0] = -100; /* AT_FDCWD */
            return 1;
        case X86_SYS_symlink:
            args->arg[2] = args->arg[1];
            args->arg[1] = -100; /* AT_FDCWD */
            return 1;
        case X86_SYS_pipe:
            args->arg[1] = 0;
            return 1;
        case X86_SYS_dup2:
            args->arg[2] = 0;
            return 1;
    }
    return 0;
}

#endif /* _ARCH_X86_64_SYSCALL_NR_H */
