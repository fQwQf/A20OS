# Linux Syscall Coverage

This file tracks the intended compatibility level of the Linux syscall subset.
It is deliberately conservative: a syscall should not be marked `full` merely
because it has an entry in `syscall_table.def`.

## Coverage Legend

- `full`: intended complete behavior for supported objects and flags.
- `partial`: useful implementation with known semantic gaps.
- `stub`: compatibility placeholder or simplified success/failure behavior.
- `missing`: entry is absent or important operations return `-ENOSYS`.

## Current Summary

`syscall_table.def` currently registers 258 dispatch entries, including two
A20OS extensions. Registration is dispatch coverage, not a claim of semantic
Linux completeness. Exactly 16 entries are direct, fixed `-ENOSYS`
placeholders; registered non-placeholder calls may still support only a subset
of Linux commands, flags, object types, or concurrency semantics.

| Area | Level | Notes |
| --- | --- | --- |
| basic fd I/O | partial | read/write/pread/pwrite/iovec paths exist; concurrent close/lifetime rules need tightening. |
| path and metadata | partial | openat/stat/chmod/chown/link/symlink/xattr coverage exists; path resolution and permissions need cleanup. |
| process lifecycle | partial | fork/clone/exec/wait/exit work for current userland; SMP/thread edge semantics remain limited. |
| signals | partial | common delivery paths exist; Linux edge behavior is not complete. |
| memory management | partial | brk/mmap/munmap/mprotect/mremap and COW exist; file mmap/page cache semantics need work. |
| scheduler | partial | APIs map onto the per-CPU EEVDF/SMP scheduler, but Linux policy/priority/affinity, RT, deadline, cgroup, and topology semantics remain bounded. |
| futex | partial | basic operations exist; advanced futex operations are incomplete. |
| poll/epoll/select | partial | fd readiness works for common objects; wait infrastructure should move to formal wait queues. |
| eventfd/timerfd | partial | fd-backed wait objects exist; full Linux timer semantics are simplified. |
| sockets | partial | AF_INET/AF_UNIX/AF_ALG subset exists via lwIP/socket layer; many protocol details are simplified. |
| bpf | partial | KEP-backed program load/attach/detach only; no BPF maps, and attach targets are A20OS extension-point ids rather than Linux attach objects. |
| namespaces | stub | compatibility return paths, no full namespace model. |
| capabilities | stub | small capability model, not Linux security semantics. |
| file advice/copy helpers | partial | implemented for common paths, many flags are approximations. |
| SysV/POSIX shm and memfd | partial | useful shared-memory objects exist; full Linux accounting/security is incomplete. |
| keyring | partial | kernel keyring subsystem (kernel/ipc/keyring.c): add/request/keyctl with session and per-uid user keyrings. |
| fanotify | partial | FAN_CLASS_NOTIF + FID on the shared notify backend; content/pre-content classes not supported. |
| process accounting | partial | Linux v3 acct records written on process exit. |
| Linux AIO | partial | aio contexts with synchronous VFS execution of pread/pwrite/fsync/fdatasync. |
| pidfd | partial | pidfd_open/pidfd_getfd/pidfd_send_signal with capability checks. |
| driver modules | partial | init/finit/delete_module drive the A20OS drvmod ET_REL loader (privileged). |

## Next Steps

1. Generate a table from `syscall_table.def` and annotate each syscall.
2. Mark every deliberate fixed-success implementation as `stub`.
3. Add a smoke test for each syscall group before upgrading its level.

## Generated Syscall Table

<!-- LINUX_SYSCALL_COVERAGE_BEGIN -->
| Syscall | Area | Level | Smoke Gate | Notes |
| --- | --- | --- | --- | --- |
| `read` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `write` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `writev` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `readv` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `preadv` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `pwritev` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `preadv2` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `pwritev2` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `openat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `close` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `lseek` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `dup` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `dup3` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fcntl` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `flock` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `pipe2` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `ioctl` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `pread64` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `pwrite64` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sync` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fsync` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fdatasync` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `ftruncate` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `truncate` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fallocate` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fadvise64` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `copy_file_range` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `splice` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `vmsplice` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `tee` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `close_range` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sendfile` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `select` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `poll` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `ppoll` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `epoll_create1` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `epoll_ctl` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `epoll_pwait` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `epoll_pwait2` | poll | `partial` | `smoke-syscall-ext` | epoll_pwait with a timespec64 timeout; Linux edge semantics remain documented gaps |
| `eventfd2` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `timerfd_create` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timerfd_settime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timerfd_gettime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `inotify_init1` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `socket` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `socketpair` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `bind` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `listen` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `accept` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `accept4` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `connect` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getsockname` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getpeername` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `sendto` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `recvfrom` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setsockopt` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getsockopt` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `shutdown` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `sendmsg` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `recvmsg` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `sendmmsg` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `recvmmsg` | sockets | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `mkdirat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `unlinkat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `renameat2` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `chdir` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fchdir` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `getcwd` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fstat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fstatat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `readlinkat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `faccessat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `faccessat2` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fchmod` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fchmodat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fchmodat2` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fchown` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fchownat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `setxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `lsetxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fsetxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `getxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `lgetxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fgetxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `listxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `llistxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `flistxattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `removexattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `lremovexattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fremovexattr` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `getdents64` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `linkat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `symlinkat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `statx` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `statfs` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fstatfs` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `mount` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `umount2` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `swapon` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `swapoff` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `mkswap` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `utimensat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `chroot` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `mknodat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `set_thread_area` | arch | `partial` | `smoke-abi-linux` | implemented subset; architecture-specific semantics remain bounded |
| `exit` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `exit_group` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `waitid` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `getpid` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `getppid` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `gettid` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `set_tid_address` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `set_robust_list` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `get_robust_list` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `capget` | capabilities | `partial` | `smoke-abi-linux` | small kernel capability model, not full Linux security semantics |
| `capset` | capabilities | `partial` | `smoke-abi-linux` | small kernel capability model, not full Linux security semantics |
| `getuid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `geteuid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getgid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getegid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setuid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setgid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setreuid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setregid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setresuid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getresuid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setresgid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getresgid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setfsuid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setfsgid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getpgid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setpgid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setsid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `clone` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `execve` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `execveat` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `wait4` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `ptrace` | process/debug | `partial` | `smoke-ptrace` | kernel-internal debug interface (proc_debug_*); TRACEME/ATTACH/DETACH/CONT/SYSCALL/SINGLESTEP(x86_64), GETREGS/SETREGS, PEEK/POKE, GETSIGINFO, SETOPTIONS, GETREGSET; PTRACE_SEIZE/TRACECLONE/interrupt-style ops not implemented |
| `sched_yield` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_get_priority_max` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_get_priority_min` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_getaffinity` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_setaffinity` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_getparam` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_setparam` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_getscheduler` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_setscheduler` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_rr_get_interval` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `getpriority` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `setpriority` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `reboot` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `prctl` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `prlimit64` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getrlimit` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setrlimit` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getrusage` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `kill` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `tkill` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `tgkill` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sigaction` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sigprocmask` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sigtimedwait` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `rt_sigqueueinfo` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `pidfd_send_signal` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `pidfd_open` | process | `partial` | `smoke-syscall-ext` | creates a pidfd for a live pid; requires CAP_SYS_PTRACE or same-user |
| `pidfd_getfd` | process | `partial` | `smoke-syscall-ext` | duplicates a target pidfd's fd into the caller; requires CAP_SYS_PTRACE or same-user |
| `sigreturn` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sigsuspend` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sigaltstack` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `brk` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `mmap` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `munmap` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `mprotect` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `msync` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `madvise` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `mremap` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `shm_open` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `memfd_create` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `shmget` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `shmat` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `shmdt` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `shmctl` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `semget` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `semctl` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `semtimedop` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `semop` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `bpf` | bpf | `partial` | `smoke-abi-linux` | KEP-backed BPF_PROG_LOAD/ATTACH/DETACH only; no BPF maps; target_fd is an A20OS extension-point id |
| `clock_settime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_gettime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_gettime32` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_getres` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `nanosleep` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_nanosleep` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `getitimer` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `setitimer` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_create` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_delete` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_gettime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_getoverrun` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_settime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `adjtimex` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_adjtime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `gettimeofday` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `settimeofday` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `times` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `time` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `alarm` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `uname` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `sysinfo` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getgroups` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setgroups` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `umask` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `syslog` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getrandom` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `futex` | futex | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `membarrier` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getcpu` | scheduler | `partial` | `smoke-proc-stress` | reports the current logical CPU and a single NUMA node; cache argument is ignored |
| `sync_file_range` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `getsid` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `rt_sigpending` | signals | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sethostname` | namespaces | `partial` | `smoke-abi-linux` | compatibility paths only; no full namespace model |
| `setdomainname` | namespaces | `partial` | `smoke-abi-linux` | compatibility paths only; no full namespace model |
| `mlock` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `munlock` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `mlockall` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `munlockall` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `mincore` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `personality` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `vhangup` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `unshare` | namespaces | `partial` | `smoke-abi-linux` | compatibility paths only; no full namespace model |
| `setns` | namespaces | `partial` | `smoke-abi-linux` | compatibility paths only; no full namespace model |
| `pivot_root` | namespaces | `partial` | `smoke-abi-linux` | compatibility paths only; no full namespace model |
| `get_mempolicy` | memory | `partial` | `smoke-mm-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sched_setattr` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `sched_getattr` | scheduler | `partial` | `smoke-proc-stress` | policy/priority/affinity compatibility is bounded by current scheduler state |
| `clone3` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `openat2` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `inotify_init` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `inotify_add_watch` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `inotify_rm_watch` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `syncfs` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `fanotify_init` | path/fs | `partial` | `smoke-syscall-ext` | FAN_CLASS_NOTIF + FID reporting on the shared notify backend; content/pre-content classes and per-event fds not supported |
| `fanotify_mark` | path/fs | `partial` | `smoke-syscall-ext` | ADD/REMOVE/DONT_FOLLOW/ONLYDIR marks on the shared notify backend; full Linux mark flags not supported |
| `signalfd4` | signals | `partial` | `smoke-signalfd-stress` | implemented subset; create/mask-update/read/poll/epoll; Linux edge semantics remain documented gaps |
| `acct` | system | `partial` | `smoke-syscall-ext` | writes Linux v3 acct records on process exit; disabled by acct(NULL) |
| `add_key` | keyring | `partial` | `smoke-syscall-ext` | kernel keyring subsystem (kernel/ipc/keyring.c); add/update/link/search/read; no key type instantiators |
| `request_key` | keyring | `partial` | `smoke-syscall-ext` | searches session then user keyrings; callout_info ignored |
| `keyctl` | keyring | `partial` | `smoke-syscall-ext` | GET_KEYRING_ID/JOIN/CHOWN/SETPERM/SET_TIMEOUT/LINK/UNLINK/SEARCH/DESCRIBE/READ; remaining cmds -EOPNOTSUPP |
| `io_setup` | aio | `partial` | `smoke-syscall-ext` | kernel AIO contexts (kernel/fs/aio.c); synchronous pread/pwrite/fsync/fdatasync execution with completion queue |
| `io_destroy` | aio | `partial` | `smoke-syscall-ext` | context teardown; reaped automatically on mm destroy |
| `io_submit` | aio | `partial` | `smoke-syscall-ext` | PREAD/PWRITE/FSYNC/FDSYNC executed synchronously through VFS; POLL/NOOP and other opcodes -EINVAL |
| `io_getevents` | aio | `partial` | `smoke-syscall-ext` | waits for min_nr completions with timeout; copies io_event records |
| `io_pgetevents` | aio | `partial` | `smoke-syscall-ext` | io_getevents with sigmask accepted (mask applied as in pwait) |
| `io_cancel` | aio | `partial` | `smoke-syscall-ext` | reports in-flight iocb results; synchronous execution cannot abort a running op |
| `init_module` | modules | `partial` | `smoke-syscall-ext` | stages a module image and loads it through the A20OS drvmod ET_REL loader; requires CAP_SYS_MODULE |
| `delete_module` | modules | `partial` | `smoke-syscall-ext` | unloads a drvmod module by name; pinned (driver-registered) modules return -EBUSY |
| `finit_module` | modules | `partial` | `smoke-syscall-ext` | loads a drvmod module from an already-open fd; requires CAP_SYS_MODULE |
| `userfaultfd` | memory | `stub` | `smoke-mm-stress` | explicit -ENOSYS compatibility placeholder |
| `perf_event_open` | perf | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `arch_prctl` | arch | `stub` | `stub-review` | architecture-specific implementation; unsupported architectures return -ENOSYS |
| `a20_channel_pair` | a20-ipc | `full` | `smoke-a20-channel` | A20OS extension: create a channel pair as fds (read/write per message); Linux ABI bridge to the unified channel IPC |
| `a20_registry_client` | a20-ipc | `full` | `smoke-a20-channel` | A20OS extension: open the well-known service-registry client endpoint as an fd |
<!-- LINUX_SYSCALL_COVERAGE_END -->

## Stub Decision Record

The following 16 explicit, fixed `-ENOSYS` placeholders in `syscall_table.def` have been reviewed. Each entry documents the decision, rationale, and whether it is within the claimed compatibility scope.

| Syscall | Decision | Rationale | In Scope |
| --- | --- | --- | --- |
| `fanotify_init` | **implemented** | Now wired to the shared notify backend with FAN_CLASS_NOTIF + FID reporting. | Yes |
| `fanotify_mark` | **implemented** | ADD/REMOVE/DONT_FOLLOW/ONLYDIR marks on the shared notify backend. | Yes |
| `acct` | **implemented** | Writes Linux v3 accounting records on process exit. | Yes |
| `add_key` | **implemented** | Kernel keyring subsystem in `kernel/ipc/keyring.c`. | Yes |
| `request_key` | **implemented** | Session/user keyring search. | Yes |
| `keyctl` | **implemented** | Core keyctl commands; remaining cmds `-EOPNOTSUPP`. | Yes |
| `io_setup` | **implemented** | Kernel AIO contexts in `kernel/fs/aio.c`. | Yes |
| `io_destroy` | **implemented** | Context teardown. | Yes |
| `io_submit` | **implemented** | PREAD/PWRITE/FSYNC/FDSYNC through the VFS. | Yes |
| `io_getevents` | **implemented** | Completion wait/copy. | Yes |
| `io_cancel` | **implemented** | In-flight iocb result reporting. | Yes |
| `init_module` | **implemented** | Maps to the A20OS drvmod loader (privileged). | Yes |
| `delete_module` | **implemented** | drvmod unload by name (privileged). | Yes |
| `finit_module` | **implemented** | drvmod load from an fd (privileged). | Yes |
| `userfaultfd` | **keep stub** | Userspace page-fault handling; MM subsystem does not support this model. | No |
| `perf_event_open` | **keep stub** | Performance monitoring counters; no PMC driver or perf subsystem. | No |

`arch_prctl` is not one of these 16 direct placeholders. The x86_64 implementation supports `ARCH_SET_FS` and `ARCH_GET_FS`, while unsupported operations and the generic non-x86_64 fallback may return `-ENOSYS`; the generated table therefore keeps its conservative `stub` classification.

**Scope Note:** The remaining `-ENOSYS` placeholders (`userfaultfd`, `perf_event_open`) are outside the claimed Linux ABI compatibility scope. They are kept in the syscall table to provide predictable `-ENOSYS` behavior rather than missing-table crashes, which improves userland robustness. The newly implemented syscalls above are `partial`: they cover the common ABI surface with documented gaps in Linux edge semantics.
