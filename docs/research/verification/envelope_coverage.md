# 信封咽喉完备性覆盖矩阵（自动生成，勿手改）

由 `tools/gen_envelope_coverage.py` 从 `kernel/abi/linux/syscall_table.def` 机械生成——每登记一个新 syscall，`make check-envelope-coverage` 即失败直至其被显式分类。

- 登记入口总数：**366**
- ACQUIRE：14
- TRANSFER：3
- USE：15
- FAILCLOSED：1
- PLANNED：35
- NA：298
- **资源权威相关且已调解：33**；已知未调解面（PLANNED）：35（全部挂 W2 行项）；无权威参与（NA）：298

分类语义见 docs/research/05 §2.5；类目定义见生成脚本头部。PLANNED 行项清零是论文投稿前条件（审稿人第一攻击点）。

| nr | syscall | 分类 | 说明 |
|------|---------|------|------|
| 0 | `io_setup` | NA |  |
| 1 | `io_destroy` | NA |  |
| 2 | `io_submit` | NA |  |
| 3 | `io_cancel` | NA |  |
| 4 | `io_getevents` | NA |  |
| 5 | `setxattr` | NA |  |
| 6 | `lsetxattr` | NA |  |
| 7 | `fsetxattr` | NA |  |
| 8 | `getxattr` | NA |  |
| 9 | `lgetxattr` | NA |  |
| 10 | `fgetxattr` | NA |  |
| 11 | `listxattr` | NA |  |
| 12 | `llistxattr` | NA |  |
| 13 | `flistxattr` | NA |  |
| 14 | `removexattr` | NA |  |
| 15 | `lremovexattr` | NA |  |
| 16 | `fremovexattr` | NA |  |
| 17 | `getcwd` | NA |  |
| 18 | `lookup_dcookie` | NA |  |
| 19 | `eventfd2` | ACQUIRE | A3 EVENT_QUEUE |
| 20 | `epoll_create1` | PLANNED | W2 EVENT_QUEUE |
| 21 | `epoll_ctl` | NA |  |
| 22 | `epoll_pwait` | NA |  |
| 23 | `dup` | NA |  |
| 24 | `dup3` | NA |  |
| 25 | `fcntl` | NA |  |
| 26 | `inotify_init1` | PLANNED | W2 EVENT_QUEUE |
| 27 | `inotify_add_watch` | NA |  |
| 28 | `inotify_rm_watch` | NA |  |
| 29 | `ioctl` | NA |  |
| 30 | `ioprio_set` | NA |  |
| 31 | `ioprio_get` | NA |  |
| 32 | `flock` | NA |  |
| 33 | `mknodat` | NA |  |
| 34 | `mkdirat` | NA |  |
| 35 | `unlinkat` | NA |  |
| 36 | `symlinkat` | NA |  |
| 37 | `linkat` | NA |  |
| 38 | `renameat` | NA |  |
| 39 | `umount2` | PLANNED | W2 |
| 40 | `mount` | PLANNED | W2: 特权挂载对信封拒绝 |
| 41 | `pivot_root` | NA |  |
| 42 | `nfsservctl` | NA |  |
| 43 | `statfs` | NA |  |
| 44 | `fstatfs` | NA |  |
| 45 | `truncate` | NA |  |
| 46 | `ftruncate` | NA |  |
| 47 | `fallocate` | NA |  |
| 48 | `faccessat` | NA |  |
| 49 | `chdir` | NA |  |
| 50 | `fchdir` | NA |  |
| 51 | `chroot` | NA |  |
| 52 | `fchmod` | NA |  |
| 53 | `fchmodat` | NA |  |
| 54 | `fchownat` | NA |  |
| 55 | `fchown` | NA |  |
| 56 | `openat` | ACQUIRE | A1 权利推导 + A8 重开权利交集 |
| 57 | `close` | NA |  |
| 58 | `vhangup` | NA |  |
| 59 | `pipe2` | ACQUIRE | A3 两端各一影子 |
| 60 | `quotactl` | NA |  |
| 61 | `getdents64` | NA |  |
| 62 | `lseek` | NA |  |
| 63 | `read` | USE | 方向位 R |
| 64 | `write` | USE | 方向位 W |
| 65 | `readv` | USE | 方向位 R |
| 66 | `writev` | USE | 方向位 W |
| 67 | `pread64` | USE | 方向位 R |
| 68 | `pwrite64` | USE | 方向位 W |
| 69 | `preadv` | NA |  |
| 70 | `pwritev` | NA |  |
| 71 | `sendfile` | NA |  |
| 72 | `select` | NA |  |
| 73 | `ppoll` | NA |  |
| 74 | `signalfd4` | ACQUIRE | A3 EVENT_QUEUE（仅创建分支） |
| 75 | `vmsplice` | NA |  |
| 76 | `splice` | NA |  |
| 77 | `tee` | NA |  |
| 78 | `readlinkat` | NA |  |
| 79 | `fstatat` | NA |  |
| 80 | `fstat` | NA |  |
| 81 | `sync` | NA |  |
| 82 | `fsync` | NA |  |
| 83 | `fdatasync` | NA |  |
| 84 | `sync_file_range` | NA |  |
| 85 | `timerfd_create` | ACQUIRE | A3 TIMER |
| 86 | `timerfd_settime` | NA |  |
| 87 | `timerfd_gettime` | NA |  |
| 88 | `utimensat` | NA |  |
| 89 | `acct` | NA |  |
| 90 | `capget` | NA |  |
| 91 | `capset` | NA |  |
| 92 | `personality` | NA |  |
| 93 | `exit` | NA |  |
| 94 | `exit_group` | NA |  |
| 95 | `waitid` | NA |  |
| 96 | `set_tid_address` | NA |  |
| 97 | `unshare` | NA |  |
| 98 | `futex` | NA |  |
| 99 | `set_robust_list` | NA |  |
| 100 | `get_robust_list` | NA |  |
| 101 | `nanosleep` | NA |  |
| 102 | `getitimer` | NA |  |
| 103 | `setitimer` | NA |  |
| 104 | `kexec_load` | NA |  |
| 105 | `init_module` | NA |  |
| 106 | `delete_module` | NA |  |
| 107 | `timer_create` | NA |  |
| 108 | `timer_gettime` | NA |  |
| 109 | `timer_getoverrun` | NA |  |
| 110 | `timer_settime` | NA |  |
| 111 | `timer_delete` | NA |  |
| 112 | `clock_settime` | NA |  |
| 113 | `clock_gettime` | NA |  |
| 114 | `clock_getres` | NA |  |
| 115 | `clock_nanosleep` | NA |  |
| 116 | `syslog` | NA |  |
| 117 | `ptrace` | PLANNED | W2: 跨进程内省对信封拒绝 |
| 118 | `sched_setparam` | NA |  |
| 119 | `sched_setscheduler` | NA |  |
| 120 | `sched_getscheduler` | NA |  |
| 121 | `sched_getparam` | NA |  |
| 122 | `sched_setaffinity` | NA |  |
| 123 | `sched_getaffinity` | NA |  |
| 124 | `sched_yield` | NA |  |
| 125 | `sched_get_priority_max` | NA |  |
| 126 | `sched_get_priority_min` | NA |  |
| 127 | `sched_rr_get_interval` | NA |  |
| 128 | `restart_syscall` | NA |  |
| 129 | `kill` | NA |  |
| 130 | `tkill` | NA |  |
| 131 | `tgkill` | NA |  |
| 132 | `sigaltstack` | NA |  |
| 133 | `sigsuspend` | NA |  |
| 134 | `sigaction` | NA |  |
| 135 | `sigprocmask` | NA |  |
| 136 | `rt_sigpending` | NA |  |
| 137 | `sigtimedwait` | NA |  |
| 138 | `rt_sigqueueinfo` | NA |  |
| 139 | `sigreturn` | NA |  |
| 140 | `setpriority` | NA |  |
| 141 | `getpriority` | NA |  |
| 142 | `reboot` | NA |  |
| 143 | `setregid` | NA |  |
| 144 | `setgid` | NA |  |
| 145 | `setreuid` | NA |  |
| 146 | `setuid` | NA |  |
| 147 | `setresuid` | NA |  |
| 148 | `getresuid` | NA |  |
| 149 | `setresgid` | NA |  |
| 150 | `getresgid` | NA |  |
| 151 | `setfsuid` | NA |  |
| 152 | `setfsgid` | NA |  |
| 153 | `times` | NA |  |
| 154 | `setpgid` | NA |  |
| 155 | `getpgid` | NA |  |
| 156 | `getsid` | NA |  |
| 157 | `setsid` | NA |  |
| 158 | `getgroups` | NA |  |
| 159 | `setgroups` | NA |  |
| 160 | `uname` | NA |  |
| 161 | `sethostname` | NA |  |
| 162 | `setdomainname` | NA |  |
| 163 | `getrlimit` | NA |  |
| 164 | `setrlimit` | NA |  |
| 165 | `getrusage` | NA |  |
| 166 | `umask` | NA |  |
| 167 | `prctl` | NA |  |
| 168 | `getcpu` | NA |  |
| 169 | `gettimeofday` | NA |  |
| 170 | `settimeofday` | NA |  |
| 171 | `adjtimex` | NA |  |
| 172 | `getpid` | NA |  |
| 173 | `getppid` | NA |  |
| 174 | `getuid` | NA |  |
| 175 | `geteuid` | NA |  |
| 176 | `getgid` | NA |  |
| 177 | `getegid` | NA |  |
| 178 | `gettid` | NA |  |
| 179 | `sysinfo` | NA |  |
| 180 | `mq_open` | PLANNED | W2 |
| 181 | `mq_unlink` | NA |  |
| 182 | `mq_timedsend` | PLANNED | W2 |
| 183 | `mq_timedreceive` | PLANNED | W2 |
| 184 | `mq_notify` | NA |  |
| 185 | `mq_getsetattr` | NA |  |
| 186 | `msgget` | PLANNED | W2 |
| 187 | `msgctl` | PLANNED | W2 |
| 188 | `msgrcv` | PLANNED | W2 |
| 189 | `msgsnd` | PLANNED | W2 |
| 190 | `semget` | PLANNED | W2 |
| 191 | `semctl` | PLANNED | W2 |
| 192 | `semtimedop` | PLANNED | W2 |
| 193 | `semop` | PLANNED | W2 |
| 194 | `shmget` | PLANNED | W2: 段创建类检查 |
| 195 | `shmctl` | PLANNED | W2 审计 |
| 196 | `shmat` | ACQUIRE | A5 MEMORY 类检查（足迹计费 W2） |
| 197 | `shmdt` | NA |  |
| 198 | `socket` | ACQUIRE | A2 网络 rights + 类上限 |
| 199 | `socketpair` | ACQUIRE | A2 双端 SOCKET 获取，各自安装影子 |
| 200 | `bind` | USE | 同 connect |
| 201 | `listen` | USE | 同 connect |
| 202 | `accept` | ACQUIRE | 同 accept4（薄委托） |
| 203 | `connect` | USE | socket use 计次 |
| 204 | `getsockname` | NA |  |
| 205 | `getpeername` | NA |  |
| 206 | `sendto` | USE | 方向位 W + len 字节计费 |
| 207 | `recvfrom` | USE | 方向位 R + len 字节计费 |
| 208 | `setsockopt` | PLANNED | W2 审计 |
| 209 | `getsockopt` | NA |  |
| 210 | `shutdown` | PLANNED | W2 |
| 211 | `sendmsg` | TRANSFER | A6 发送侧 propagation 门控 + 数据面 total 字节 W 计费（均已落地） |
| 212 | `recvmsg` | TRANSFER | A6 接收侧逐 fd 安装裁决 + 数据面 total 字节 R 计费 |
| 213 | `readahead` | NA |  |
| 214 | `brk` | NA |  |
| 215 | `munmap` | NA |  |
| 216 | `mremap` | NA |  |
| 217 | `add_key` | NA |  |
| 218 | `request_key` | NA |  |
| 219 | `keyctl` | NA |  |
| 220 | `clone` | NA |  |
| 221 | `execve` | NA |  |
| 222 | `mmap` | PLANNED | W2: A4 file-backed Map right + 时间 |
| 223 | `fadvise64` | NA |  |
| 224 | `swapon` | PLANNED | W2 |
| 225 | `swapoff` | PLANNED | W2 |
| 226 | `mprotect` | NA |  |
| 227 | `msync` | NA |  |
| 228 | `mlock` | NA |  |
| 229 | `munlock` | NA |  |
| 230 | `mlockall` | NA |  |
| 231 | `munlockall` | NA |  |
| 232 | `mincore` | NA |  |
| 233 | `madvise` | NA |  |
| 234 | `remap_file_pages` | NA |  |
| 235 | `mbind` | NA |  |
| 236 | `get_mempolicy` | NA |  |
| 237 | `set_mempolicy` | NA |  |
| 238 | `migrate_pages` | NA |  |
| 239 | `move_pages` | NA |  |
| 240 | `rt_tgsigqueueinfo` | NA |  |
| 241 | `perf_event_open` | PLANNED | W2 |
| 242 | `accept4` | ACQUIRE | A2 派生 socket 全新获取裁决 |
| 243 | `recvmmsg` | USE | 经 recvmsg 汇聚计费 |
| 258 | `riscv_hwprobe` | NA |  |
| 259 | `riscv_flush_icache` | NA |  |
| 260 | `wait4` | NA |  |
| 261 | `prlimit64` | NA |  |
| 262 | `fanotify_init` | PLANNED | W2 |
| 263 | `fanotify_mark` | NA |  |
| 264 | `name_to_handle_at` | NA |  |
| 265 | `open_by_handle_at` | NA |  |
| 266 | `clock_adjtime` | NA |  |
| 267 | `syncfs` | NA |  |
| 268 | `setns` | NA |  |
| 269 | `sendmmsg` | USE | 经 sendmsg_from_msghdr 汇聚计费 |
| 270 | `process_vm_readv` | PLANNED | W2 同 ptrace |
| 271 | `process_vm_writev` | PLANNED | W2 同 ptrace |
| 272 | `kcmp` | NA |  |
| 273 | `finit_module` | NA |  |
| 274 | `sched_setattr` | NA |  |
| 275 | `sched_getattr` | NA |  |
| 276 | `renameat2` | NA |  |
| 277 | `seccomp` | NA |  |
| 278 | `getrandom` | NA |  |
| 279 | `memfd_create` | ACQUIRE | A3 匿名 FILE |
| 280 | `bpf` | PLANNED | W2: 建议 ENV 直接拒绝 |
| 281 | `execveat` | NA |  |
| 282 | `userfaultfd` | PLANNED | W2 |
| 283 | `membarrier` | NA |  |
| 284 | `mlock2` | NA |  |
| 285 | `copy_file_range` | NA |  |
| 286 | `preadv2` | NA |  |
| 287 | `pwritev2` | NA |  |
| 288 | `pkey_mprotect` | NA |  |
| 289 | `pkey_alloc` | NA |  |
| 290 | `pkey_free` | NA |  |
| 291 | `statx` | NA |  |
| 292 | `io_pgetevents` | NA |  |
| 293 | `rseq` | NA |  |
| 294 | `kexec_file_load` | NA |  |
| 403 | `clock_gettime64` | NA |  |
| 404 | `clock_settime64` | NA |  |
| 406 | `clock_getres_time64` | NA |  |
| 407 | `clock_nanosleep_time64` | NA |  |
| 408 | `timer_gettime64` | NA |  |
| 409 | `timer_settime64` | NA |  |
| 410 | `timerfd_gettime64` | NA |  |
| 411 | `timerfd_settime64` | NA |  |
| 412 | `utimensat_time64` | NA |  |
| 413 | `pselect6_time64` | NA |  |
| 414 | `ppoll_time64` | NA |  |
| 416 | `io_pgetevents_time64` | NA |  |
| 417 | `recvmmsg_time64` | USE | 同 recvmmsg |
| 418 | `mq_timedsend_time64` | PLANNED | W2 |
| 419 | `mq_timedreceive_time64` | PLANNED | W2 |
| 420 | `semtimedop_time64` | PLANNED | W2 |
| 421 | `rt_sigtimedwait_time64` | NA |  |
| 422 | `futex_time64` | NA |  |
| 423 | `sched_rr_get_interval_time64` | NA |  |
| 424 | `pidfd_send_signal` | NA |  |
| 425 | `io_uring_setup` | ACQUIRE | A9 ring fd 按 EVENT_QUEUE 获取；SQPOLL 拒绝 |
| 426 | `io_uring_enter` | USE | A10 SQE 执行点 READ/WRITE/FSYNC 调解 |
| 427 | `io_uring_register` | FAILCLOSED | A9 REGISTER_FILES 分支对信封任务 -EPERM；EVENTFD 完成通知在执行点调解 |
| 428 | `open_tree` | NA |  |
| 429 | `move_mount` | NA |  |
| 430 | `fsopen` | NA |  |
| 431 | `fsconfig` | NA |  |
| 432 | `fsmount` | NA |  |
| 433 | `fspick` | NA |  |
| 434 | `pidfd_open` | NA |  |
| 435 | `clone3` | NA |  |
| 436 | `close_range` | NA |  |
| 437 | `openat2` | NA |  |
| 438 | `pidfd_getfd` | TRANSFER | A7 全新获取裁决（安装前） |
| 439 | `faccessat2` | NA |  |
| 440 | `process_madvise` | NA |  |
| 441 | `epoll_pwait2` | NA |  |
| 442 | `mount_setattr` | NA |  |
| 443 | `quotactl_fd` | NA |  |
| 444 | `landlock_create_ruleset` | NA |  |
| 445 | `landlock_add_rule` | NA |  |
| 446 | `landlock_restrict_self` | NA |  |
| 447 | `memfd_secret` | NA |  |
| 448 | `process_mrelease` | NA |  |
| 449 | `futex_waitv` | NA |  |
| 450 | `set_mempolicy_home_node` | NA |  |
| 451 | `cachestat` | NA |  |
| 452 | `fchmodat2` | NA |  |
| 453 | `map_shadow_stack` | NA |  |
| 454 | `futex_wake` | NA |  |
| 455 | `futex_wait` | NA |  |
| 456 | `futex_requeue` | NA |  |
| 457 | `statmount` | PLANNED | W2 mount-api 族 |
| 458 | `listmount` | PLANNED | W2 mount-api 族 |
| 459 | `lsm_get_self_attr` | NA |  |
| 460 | `lsm_set_self_attr` | NA |  |
| 461 | `lsm_list_modules` | NA |  |
| 462 | `mseal` | NA |  |
| 463 | `setxattrat` | NA |  |
| 464 | `getxattrat` | NA |  |
| 465 | `listxattrat` | NA |  |
| 466 | `removexattrat` | NA |  |
| 467 | `open_tree_attr` | PLANNED | W2 mount-api 族 |
| 468 | `file_getattr` | NA |  |
| 469 | `file_setattr` | NA |  |
| 470 | `listns` | NA |  |
| 900 | `a20_channel_pair` | ACQUIRE | Linux bridge 双端 CHANNEL_ENDPOINT 获取 |
| 901 | `a20_registry_client` | ACQUIRE | Linux bridge registry CHANNEL_ENDPOINT 获取 |
| 902 | `a20_envelope_create` | NA |  |
| 903 | `a20_envelope_enter` | NA |  |
| 904 | `a20_envelope_revoke` | NA |  |
| 905 | `a20_envelope_stats` | NA |  |
| 906 | `a20_envelope_audit` | NA |  |
| 1000 | `arch_prctl` | NA |  |
| 1001 | `set_thread_area` | NA |  |
| 1002 | `poll` | NA |  |
| 1003 | `time` | NA |  |
| 1004 | `pause` | NA |  |
| 1005 | `utime` | NA |  |
| 1006 | `utimes` | NA |  |
| 1007 | `get_thread_area` | NA |  |
| 1020 | `mkswap` | NA |  |
| 1021 | `shm_open` | NA |  |
| 1022 | `alarm` | NA |  |
| 1023 | `clock_gettime32` | NA |  |
