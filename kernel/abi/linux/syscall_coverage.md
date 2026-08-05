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

| Area | Level | Notes |
| --- | --- | --- |
| basic fd I/O | partial | read/write/pread/pwrite/iovec paths exist; concurrent close/lifetime rules need tightening. |
| path and metadata | partial | openat/stat/chmod/chown/link/symlink/xattr coverage exists; path resolution and permissions need cleanup. |
| process lifecycle | partial | fork/clone/exec/wait/exit work for current userland; SMP/thread edge semantics remain limited. |
| signals | partial | common delivery paths exist; Linux edge behavior is not complete. |
| memory management | partial | brk/mmap/munmap/mprotect/mremap and COW exist; file mmap/page cache semantics need work. |
| scheduler | stub | policy/priority/affinity APIs are compatibility approximations. |
| futex | partial | basic operations exist; advanced futex operations are incomplete. |
| poll/epoll/select | partial | fd readiness works for common objects; wait infrastructure should move to formal wait queues. |
| eventfd/timerfd | partial | fd-backed wait objects exist; full Linux timer semantics are simplified. |
| sockets | partial | AF_INET/AF_UNIX/AF_ALG subset exists via lwIP/socket layer; many protocol details are simplified. |
| bpf | stub | minimal map/prog/socket-filter shim, not real eBPF. |
| namespaces | stub | compatibility return paths, no full namespace model. |
| capabilities | stub | small capability model, not Linux security semantics. |
| file advice/copy helpers | partial | implemented for common paths, many flags are approximations. |
| SysV/POSIX shm and memfd | partial | useful shared-memory objects exist; full Linux accounting/security is incomplete. |

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
| `bpf` | bpf | `partial` | `smoke-abi-linux` | map/prog shim only; no full verifier or eBPF runtime |
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
| `fanotify_init` | path/fs | `stub` | `smoke-vfs-stress` | explicit -ENOSYS compatibility placeholder |
| `fanotify_mark` | path/fs | `stub` | `smoke-vfs-stress` | explicit -ENOSYS compatibility placeholder |
| `signalfd4` | signals | `partial` | `smoke-signalfd-stress` | implemented subset; create/mask-update/read/poll/epoll; Linux edge semantics remain documented gaps |
| `acct` | system | `stub` | `smoke-abi-linux` | explicit -ENOSYS compatibility placeholder |
| `add_key` | keyring | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `request_key` | keyring | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `keyctl` | keyring | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `io_setup` | aio | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `io_destroy` | aio | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `io_submit` | aio | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `io_getevents` | aio | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `io_cancel` | aio | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `init_module` | modules | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `delete_module` | modules | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `finit_module` | modules | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `userfaultfd` | memory | `stub` | `smoke-mm-stress` | explicit -ENOSYS compatibility placeholder |
| `perf_event_open` | perf | `stub` | `stub-review` | explicit -ENOSYS compatibility placeholder |
| `arch_prctl` | arch | `stub` | `stub-review` | architecture-specific implementation; unsupported architectures return -ENOSYS |
<!-- LINUX_SYSCALL_COVERAGE_END -->

## Stub Decision Record

The following explicit `-ENOSYS` placeholders in `syscall_table.def` have been reviewed. Each entry documents the decision, rationale, and whether it is within the claimed compatibility scope.

| Syscall | Decision | Rationale | In Scope |
| --- | --- | --- | --- |
| `fanotify_init` | **keep stub** | Filesystem monitoring; not required by base userland (musl, sbase, mksh). | No |
| `fanotify_mark` | **keep stub** | Companion to `fanotify_init`; same rationale. | No |
| `acct` | **keep stub** | Process accounting; no userland dependency and no accounting subsystem. | No |
| `add_key` | **keep stub** | Keyring management; security subsystem does not implement a keyring. | No |
| `request_key` | **keep stub** | Keyring management; same rationale as `add_key`. | No |
| `keyctl` | **keep stub** | Keyring management; same rationale as `add_key`. | No |
| `io_setup` | **keep stub** | Linux AIO; `pread`/`pwrite`/`readv`/`writev` cover all current I/O needs. | No |
| `io_destroy` | **keep stub** | Linux AIO; companion to `io_setup`. | No |
| `io_submit` | **keep stub** | Linux AIO; companion to `io_setup`. | No |
| `io_getevents` | **keep stub** | Linux AIO; companion to `io_setup`. | No |
| `io_cancel` | **keep stub** | Linux AIO; companion to `io_setup`. | No |
| `init_module` | **keep stub** | Kernel module loading; A20OS uses a statically linked kernel design. | No |
| `delete_module` | **keep stub** | Kernel module unloading; companion to `init_module`. | No |
| `finit_module` | **keep stub** | Kernel module loading via fd; companion to `init_module`. | No |
| `userfaultfd` | **keep stub** | Userspace page-fault handling; MM subsystem does not support this model. | No |
| `perf_event_open` | **keep stub** | Performance monitoring counters; no PMC driver or perf subsystem. | No |
| `arch_prctl` | **keep stub** | x86_64-specific arch prctl; kept in the generic table for cross-arch uniformity but always returns `-ENOSYS` on non-x86_64. | No |

**Scope Note:** None of the above stubs are within the claimed Linux ABI compatibility scope. They are kept in the syscall table to provide predictable `-ENOSYS` behavior rather than missing-table crashes, which improves userland robustness. If future userland requires any of these, the decision should be revisited with a dedicated implementation plan and tests.
