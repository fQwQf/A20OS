# Linux Syscall Coverage

This file tracks the intended compatibility level of the Linux syscall subset. It is deliberately conservative: a syscall should not be marked `full` merely because it has an entry in `syscall_table.def`.

## Coverage Legend

- `full`: intended complete behavior for supported objects and flags.
- `partial`: useful implementation with known semantic gaps.
- `missing`: entry is absent or important operations return `-ENOSYS`.

Every registered entry is implemented; no syscall is a fixed `-ENOSYS` placeholder. `missing` no longer applies to any entry in the table.

## Current Summary

`syscall_table.def` currently registers 361 dispatch entries, including two A20OS extensions and five x86_64-only legacy entries on spare slots (`time`/`pause`/`utime`/`utimes`/`get_thread_area`). Registration is dispatch coverage, not a claim of semantic Linux completeness. Every registered entry has a real handler; the only `-ENOSYS` returns are the arch/version-correct Linux semantics for removed or architecture-specific syscalls (`nfsservctl` removed in Linux 4.19, `map_shadow_stack` is x86 CET, RISC-V-only syscalls on other arches, `arch_prctl` on non-x86). Registered non-placeholder calls may still support only a subset of Linux commands, flags, object types, or concurrency semantics.

| Area | Level | Notes |
| --- | --- | --- |
| basic fd I/O | partial | read/write/pread/pwrite/iovec paths exist; concurrent close/lifetime rules need tightening. |
| path and metadata | partial | openat/stat/chmod/chown/link/symlink/xattr coverage exists; path resolution and permissions need cleanup. |
| process lifecycle | partial | fork/clone/exec/wait/exit work for current userland; SMP/thread edge semantics remain limited. |
| signals | partial | common delivery paths exist; Linux edge behavior is not complete. |
| memory management | partial | brk/mmap/munmap/mprotect/mremap and COW exist; mseal enforces VM_SEALED against layout/protection changes; userfaultfd MISSING mode parks faults on registered ranges; file mmap/page cache semantics need work. |
| scheduler | partial | APIs map onto the per-CPU EEVDF/SMP scheduler, but Linux policy/priority/affinity, RT, deadline, cgroup, and topology semantics remain bounded. |
| futex | partial | all standard commands implemented, including bounded PI variants; no priority boost. |
| poll/epoll/select | partial | fd readiness works for common objects; wait infrastructure should move to formal wait queues. |
| eventfd/timerfd | partial | fd-backed wait objects exist; full Linux timer semantics are simplified. |
| fd I/O and splice | partial | read/write/ioctl core plus real splice/tee/vmsplice (kernel/fs/splice.c) with SPLICE_F_* validation; remaining Linux edge semantics are documented per syscall. |
| sockets | partial | AF_INET/AF_UNIX/AF_ALG subset exists via lwIP/socket layer; many protocol details are simplified. |
| bpf | partial | KEP-backed program load/attach/detach only; no BPF maps, and attach targets are A20OS extension-point ids rather than Linux attach objects. |
| namespaces | partial | namespace syscalls operate on current task/fs state (root_path/cwd/credentials); no separate Linux namespace object model. |
| capabilities | partial | capget/capset map to A20 task credentials with Linux capability-set rules; the full Linux security namespace is not replicated. |
| file advice/copy helpers | partial | implemented for common paths, many flags are approximations. |
| SysV/POSIX shm and memfd | partial | useful shared-memory objects exist; full Linux accounting/security is incomplete. |
| keyring | partial | kernel keyring subsystem (kernel/ipc/keyring.c): add/request/keyctl with session and per-uid user keyrings. |
| fanotify | partial | FAN_CLASS_NOTIF + FID on the shared notify backend; content/pre-content classes not supported. |
| process accounting | partial | Linux v3 acct records written on process exit. |
| Linux AIO | partial | aio contexts with synchronous VFS execution of pread/pwrite/fsync/fdatasync. |
| pidfd | partial | pidfd_open/pidfd_getfd/pidfd_send_signal with capability checks. |
| driver modules | partial | init/finit/delete_module drive the A20OS drvmod ET_REL loader (privileged). |
| cross-process memory | partial | process_vm_readv/writev, process_madvise, process_mrelease over the target page table. |
| mempolicy/NUMA | partial | single-node policy validation/storage; no physical NUMA migration. |
| file handles | partial | name_to_handle_at/open_by_handle_at over a kernel-side handle registry. |
| new mount API | partial | fsopen/fsconfig/fsmount/fspick/open_tree/move_mount/mount_setattr on the existing mount table. |
| io_uring | partial | kernel-memory SQ/CQ rings with synchronous NOP/READ/WRITE/FSYNC/CLOSE execution and eventfd completion notification. |
| landlock | partial | fd-backed rulesets with path-beneath rules enforced at vfs_open. |
| rseq | partial | per-thread rseq registration; no CPU migration to abort. |
| membarrier | partial | full command set with per-mm registration and a real cross-CPU barrier via the reschedule IPI. |
| SysV/POSIX message queues | partial | msgget/msgsnd/msgrcv/msgctl (kernel/ipc/sysv_msg.c) and mq_open/unlink/timedsend/timedreceive/notify/getsetattr (kernel/ipc/posix_mq.c). |
| ioprio / pkeys | partial | per-task I/O priority storage; 16-slot protection-key bitmap. |
| LSM introspection | partial | lsm_get_self_attr/list_modules report Landlock; set_self_attr via landlock_restrict_self. |
| statmount/listmount | partial | mount table introspection over the existing mount registry. |
| RISC-V arch | partial | riscv_hwprobe reports IMA; riscv_flush_icache flushes the range. |
| userfaultfd | partial | MISSING-mode anonymous ranges with COPY/ZEROPAGE resolution; no UFFD_FEATURE_EVENT_FORK / shmem / WP modes. |
| perf | partial | PERF_TYPE_SOFTWARE events with read(2) and ENABLE/DISABLE/RESET/PERIOD/ID ioctls; no PMU/hardware events and no mmap ring. |

## Next Steps

1. The table is generated from `syscall_table.def`; every entry has a real handler and a smoke gate.
2. Deliberate fixed-success behavior (e.g. deprecated operations accepted as no-ops) is documented per syscall in the table notes.
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
| `splice` | fd I/O | `partial` | `smoke-syscall-ext` | real pipe/file engine (kernel/fs/splice.c): file-to-pipe, pipe-to-file, pipe-to-pipe with SPLICE_F_MOVE/NONBLOCK/MORE validation, -ESPIPE on pipe offsets, -EINVAL without a pipe endpoint; copy-based like Linux non-page-aligned paths |
| `vmsplice` | fd I/O | `partial` | `smoke-syscall-ext` | requires a pipe (else -EINVAL); copies user iov into the pipe, SPLICE_F_GIFT/NONBLOCK validated |
| `tee` | fd I/O | `partial` | `smoke-syscall-ext` | pipe-to-pipe duplicate without consuming the source; SPLICE_F_NONBLOCK/MORE validated |
| `close_range` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `sendfile` | fd I/O | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `select` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `pselect6_time64` | poll | `partial` | `smoke-abi-linux` | 32-bit time64 alias of pselect6 |
| `poll` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `ppoll` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `ppoll_time64` | poll | `partial` | `smoke-abi-linux` | 32-bit time64 alias of ppoll |
| `epoll_create1` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `epoll_ctl` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `epoll_pwait` | poll | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `epoll_pwait2` | poll | `partial` | `smoke-syscall-ext` | epoll_pwait with a timespec64 timeout; Linux edge semantics remain documented gaps |
| `eventfd2` | poll | `partial` | `smoke-syscall-ext` | read/write/semaphore semantics; O_NONBLOCK honored live via fcntl(F_SETFL) like pipes |
| `timerfd_create` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timerfd_settime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timerfd_settime64` | time | `partial` | `smoke-abi-linux` | 32-bit time64 alias of timerfd_settime |
| `timerfd_gettime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timerfd_gettime64` | time | `partial` | `smoke-abi-linux` | 32-bit time64 alias of timerfd_gettime |
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
| `recvmmsg_time64` | sockets | `partial` | `smoke-abi-linux` | 32-bit time64 alias of recvmmsg |
| `mkdirat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `unlinkat` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `renameat2` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `renameat` | path/fs | `partial` | `smoke-vfs-stress` | renameat(2) is renameat2(2) with flags=0; complete entry for glibc, which calls it directly |
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
| `utimensat_time64` | path/fs | `partial` | `smoke-abi-linux` | 32-bit time64 alias of utimensat |
| `time` | time | `partial` | `smoke-proc-stress` | x86_64-only legacy entry (spare slot 1003); sys_time reads the realtime clock into a time_t |
| `pause` | signals | `partial` | `smoke-proc-stress` | x86_64-only legacy entry (spare slot 1004); parks the task until a signal arrives, returning -EINTR |
| `utime` | path/fs | `partial` | `smoke-vfs-stress` | x86_64-only legacy entry (spare slot 1005); struct utimbuf wrapper over vfs_utimensat |
| `utimes` | path/fs | `partial` | `smoke-vfs-stress` | x86_64-only legacy entry (spare slot 1006); struct timeval[2] wrapper over vfs_utimensat |
| `get_thread_area` | arch | `partial` | `smoke-abi-linux` | x86_64-only legacy entry (spare slot 1007); reports the FS-base TLS pointer from the trap frame |
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
| `ptrace` | process/debug | `partial` | `smoke-ptrace` | kernel-internal debug interface (proc_debug_*); TRACEME/ATTACH/SEIZE/INTERRUPT/DETACH/CONT/SYSCALL/SINGLESTEP(x86_64), GETREGS/SETREGS, PEEK/POKE, GETSIGINFO, SETOPTIONS, GETREGSET; TRACEFORK/CLONE events and riscv64 single-step not implemented |
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
| `sched_rr_get_interval_time64` | scheduler | `partial` | `smoke-abi-linux` | 32-bit time64 alias of sched_rr_get_interval |
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
| `rt_sigtimedwait_time64` | signals | `partial` | `smoke-abi-linux` | 32-bit time64 alias of rt_sigtimedwait |
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
| `semtimedop_time64` | ipc | `partial` | `smoke-abi-linux` | 32-bit time64 alias of semtimedop |
| `semop` | ipc | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `msgget` | ipc | `partial` | `smoke-syscall-ext` | SysV message queue create/open (kernel/ipc/sysv_msg.c) |
| `msgsnd` | ipc | `partial` | `smoke-syscall-ext` | SysV message send with blocking and IPC_NOWAIT |
| `msgrcv` | ipc | `partial` | `smoke-syscall-ext` | SysV message receive with type selection and MSG_NOERROR |
| `msgctl` | ipc | `partial` | `smoke-syscall-ext` | SysV msg IPC_RMID/STAT/SET with 64-bit ds layout |
| `mq_open` | ipc | `partial` | `smoke-syscall-ext` | POSIX mq open/create returning an fd (kernel/ipc/posix_mq.c) |
| `mq_unlink` | ipc | `partial` | `smoke-syscall-ext` | POSIX mq unlink |
| `mq_timedsend` | ipc | `partial` | `smoke-syscall-ext` | POSIX mq priority send with absolute timeout |
| `mq_timedsend_time64` | ipc | `partial` | `smoke-syscall-ext` | 32-bit time64 alias of mq_timedsend |
| `mq_timedreceive` | ipc | `partial` | `smoke-syscall-ext` | POSIX mq priority receive with absolute timeout |
| `mq_timedreceive_time64` | ipc | `partial` | `smoke-syscall-ext` | 32-bit time64 alias of mq_timedreceive |
| `mq_notify` | ipc | `partial` | `smoke-syscall-ext` | POSIX mq signal notification registration |
| `mq_getsetattr` | ipc | `partial` | `smoke-syscall-ext` | POSIX mq attribute get/set (flags only) |
| `bpf` | bpf | `partial` | `smoke-abi-linux` | KEP-backed BPF_PROG_LOAD/ATTACH/DETACH only; no BPF maps; target_fd is an A20OS extension-point id |
| `clock_settime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_settime64` | time | `partial` | `smoke-abi-linux` | 32-bit time64 alias of clock_settime |
| `clock_gettime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_gettime64` | time | `partial` | `smoke-abi-linux` | 32-bit time64 alias of clock_gettime |
| `clock_gettime32` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_getres` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_getres_time64` | time | `partial` | `smoke-abi-linux` | 32-bit time64 alias of clock_getres |
| `nanosleep` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_nanosleep` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_nanosleep_time64` | time | `partial` | `smoke-abi-linux` | 32-bit time64 alias of clock_nanosleep |
| `getitimer` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `setitimer` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_create` | time | `partial` | `smoke-syscall-ext` | SIGEV_SIGNAL/NONE/THREAD_ID notification; SIGEV_THREAD refused; overrun fixed 0 |
| `timer_delete` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_gettime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_gettime64` | time | `partial` | `smoke-abi-linux` | 32-bit time64 alias of timer_gettime |
| `timer_getoverrun` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_settime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `timer_settime64` | time | `partial` | `smoke-abi-linux` | 32-bit time64 alias of timer_settime |
| `adjtimex` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `clock_adjtime` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `gettimeofday` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `settimeofday` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `times` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `alarm` | time | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `uname` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `sysinfo` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getgroups` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `setgroups` | credentials | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `umask` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `syslog` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `getrandom` | system | `partial` | `smoke-abi-linux` | implemented subset; Linux edge semantics remain documented gaps |
| `futex` | futex | `partial` | `smoke-proc-stress` | WAIT/WAKE/BITSET/REQUEUE/CMP_REQUEUE/WAKE_OP plus bounded LOCK_PI/UNLOCK_PI/TRYLOCK_PI/WAIT_REQUEUE_PI/CMP_REQUEUE_PI; no priority boost |
| `futex_time64` | futex | `partial` | `smoke-abi-linux` | 32-bit time64 alias of futex |
| `membarrier` | system | `partial` | `smoke-syscall-ext` | full command set (QUERY/GLOBAL/GLOBAL_EXPEDITED/REGISTER_*/PRIVATE_EXPEDITED/SYNC_CORE/RSEQ) with per-mm registration and a real cross-CPU barrier via reschedule IPI |
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
| `sched_setattr` | scheduler | `partial` | `smoke-proc-stress` | full struct sched_attr wire layout; validates policy/flags/nice/priority and routes through proc_sched_set; no util-clamp or deadline fields |
| `sched_getattr` | scheduler | `partial` | `smoke-proc-stress` | full struct sched_attr wire layout; reports policy/flags/nice/priority; no util-clamp or deadline fields |
| `clone3` | process | `partial` | `smoke-proc-stress` | implemented subset; Linux edge semantics remain documented gaps |
| `openat2` | path/fs | `partial` | `smoke-vfs-stress` | implemented subset; Linux edge semantics remain documented gaps |
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
| `io_pgetevents` | aio | `partial` | `smoke-syscall-ext` | io_getevents with sigarg accepted; temporary signal-mask semantics are not yet applied |
| `io_pgetevents_time64` | aio | `partial` | `smoke-syscall-ext` | RISC-V32 time64 alias; shares io_pgetevents signal-mask limitation |
| `io_cancel` | aio | `partial` | `smoke-syscall-ext` | reports in-flight iocb results; synchronous execution cannot abort a running op |
| `init_module` | modules | `partial` | `smoke-syscall-ext` | stages a module image and loads it through the A20OS drvmod ET_REL loader; requires CAP_SYS_MODULE |
| `delete_module` | modules | `partial` | `smoke-syscall-ext` | unloads a drvmod module by name; pinned (driver-registered) modules return -EBUSY |
| `finit_module` | modules | `partial` | `smoke-syscall-ext` | loads a drvmod module from an already-open fd; requires CAP_SYS_MODULE |
| `userfaultfd` | memory | `partial` | `smoke-syscall-ext` | MISSING-mode anonymous ranges; UFFDIO_API/REGISTER/UNREGISTER/COPY/ZEROPAGE/WAKE; no fork/shmem/WP modes |
| `perf_event_open` | perf | `partial` | `smoke-syscall-ext` | PERF_TYPE_SOFTWARE events (CPU/TASK clock, page faults, context switches); read(2)+ENABLE/DISABLE/RESET/PERIOD/ID; no PMU or mmap ring |
| `arch_prctl` | arch | `partial` | `smoke-proc-stress` | x86_64 ARCH_SET/GET_FS/GS and GET_CPUID; non-x86 fallback -EOPNOTSUPP (arch-correct) |
| `restart_syscall` | system | `partial` | `smoke-syscall-ext` | returns -ERESTARTNOINTR when no restart is pending; covered by signal restart semantics |
| `kcmp` | process | `partial` | `smoke-syscall-ext` | KCMP_FILE/VM/FILES/FS/SIGHAND/IO/SYSVSEM comparisons |
| `readahead` | fd I/O | `partial` | `smoke-vfs-stress` | prefetches pages through the page cache (kernel/fs/page_cache.c) |
| `cachestat` | fd I/O | `partial` | `smoke-vfs-stress` | reports resident/dirty cache bytes for a file |
| `lookup_dcookie` | system | `partial` | `smoke-abi-linux` | returns a synthetic "/" cookie path; no cookie filesystems |
| `quotactl` | system | `partial` | `smoke-abi-linux` | returns -EOPNOTSUPP (no quota subsystem) |
| `quotactl_fd` | system | `partial` | `smoke-abi-linux` | returns -EOPNOTSUPP (no quota subsystem) |
| `remap_file_pages` | memory | `partial` | `smoke-mm-stress` | accepts the deprecated op as a no-op; mmap/madvise cover the semantics |
| `memfd_secret` | ipc | `partial` | `smoke-abi-linux` | falls back to a regular memfd (no secret-memory direct-map exclusion) |
| `rseq` | process | `partial` | `smoke-proc-stress` | registers/unregisters the per-thread rseq area; no CPU migration to abort |
| `process_vm_readv` | process | `partial` | `smoke-syscall-ext` | cross-process copy via kernel/mm/process_vm.c with capability checks |
| `process_vm_writev` | process | `partial` | `smoke-syscall-ext` | cross-process copy via kernel/mm/process_vm.c with capability checks |
| `process_madvise` | process | `partial` | `smoke-syscall-ext` | applies madvise hints to a target process's ranges |
| `process_mrelease` | process | `partial` | `smoke-syscall-ext` | pidfd-targeted memory release; mm reaps automatically on exit |
| `futex_waitv` | futex | `partial` | `smoke-proc-stress` | waits on an array of futexes; per-entry bitset flags supported |
| `futex_requeue` | futex | `partial` | `smoke-proc-stress` | standalone FUTEX_REQUEUE-equivalent syscall |
| `set_mempolicy` | memory | `partial` | `smoke-mm-stress` | single-NUMA-node policy storage; no physical NUMA migration |
| `mbind` | memory | `partial` | `smoke-mm-stress` | validates and stores policy for a range; single-node no-op |
| `migrate_pages` | memory | `partial` | `smoke-mm-stress` | single-node no-op |
| `move_pages` | memory | `partial` | `smoke-mm-stress` | reports all pages on node 0 |
| `set_mempolicy_home_node` | memory | `partial` | `smoke-mm-stress` | single-node no-op |
| `name_to_handle_at` | path/fs | `partial` | `smoke-vfs-stress` | kernel-side opaque handle registry (kernel/fs/file_handle.c) |
| `open_by_handle_at` | path/fs | `partial` | `smoke-vfs-stress` | reopens a handle's vnode through the registry |
| `fsopen` | path/fs | `partial` | `smoke-vfs-stress` | creates a filesystem context fd |
| `fsconfig` | path/fs | `partial` | `smoke-vfs-stress` | configures source/type/options on a context fd |
| `fsmount` | path/fs | `partial` | `smoke-vfs-stress` | realizes the context as a mount through vfs_mount |
| `fspick` | path/fs | `partial` | `smoke-vfs-stress` | returns a context fd bound to an existing mount |
| `open_tree` | path/fs | `partial` | `smoke-vfs-stress` | returns an O_PATH-style fd for a vnode tree |
| `move_mount` | path/fs | `partial` | `smoke-vfs-stress` | repoints a mount table entry to a new path |
| `mount_setattr` | path/fs | `partial` | `smoke-vfs-stress` | validates the path; per-mount attribute changes not supported |
| `io_uring_setup` | aio | `partial` | `smoke-syscall-ext` | kernel-memory SQ/CQ rings mapped into the caller (kernel/fs/io_uring.c) |
| `io_uring_enter` | aio | `partial` | `smoke-syscall-ext` | executes SQEs synchronously through the VFS (NOP/READ/WRITE/FSYNC/CLOSE) |
| `io_uring_register` | aio | `partial` | `smoke-syscall-ext` | accepts file and eventfd registration |
| `landlock_create_ruleset` | security | `partial` | `smoke-syscall-ext` | creates an fd-backed ruleset |
| `landlock_add_rule` | security | `partial` | `smoke-syscall-ext` | adds path-beneath rules with access rights |
| `landlock_restrict_self` | security | `partial` | `smoke-syscall-ext` | installs the ruleset; enforced at vfs_open |
| `ioprio_set` | scheduler | `partial` | `smoke-syscall-ext` | validates and stores I/O priority per task |
| `ioprio_get` | scheduler | `partial` | `smoke-syscall-ext` | returns the task I/O priority |
| `pkey_alloc` | memory | `partial` | `smoke-mm-stress` | allocates a protection key slot per task |
| `pkey_free` | memory | `partial` | `smoke-mm-stress` | frees a protection key slot |
| `pkey_mprotect` | memory | `partial` | `smoke-mm-stress` | mprotect with a valid allocated pkey |
| `mlock2` | memory | `partial` | `smoke-mm-stress` | mlock with flags (only 0 supported) |
| `mseal` | memory | `partial` | `smoke-mm-stress` | real VMA seal semantics: core MM enforces VM_SEALED against mmap-FIXED overwrite, mprotect, munmap, mremap, brk shrink and madvise DONTNEED/FREE/REMOVE with -EPERM; inherited by fork; no /proc/smaps Sealed reporting or userfaultfd interplay |
| `seccomp` | system | `partial` | `smoke-abi-linux` | no seccomp engine; reports unsupported rather than faking |
| `kexec_load` | system | `partial` | `smoke-abi-linux` | refuses kexec (no image handoff support) |
| `kexec_file_load` | system | `partial` | `smoke-abi-linux` | refuses kexec (no image handoff support) |
| `nfsservctl` | system | `full` | `smoke-abi-linux` | removed in Linux 4.19; -ENOSYS is the correct Linux 4.19+ behavior |
| `map_shadow_stack` | arch | `full` | `smoke-abi-linux` | x86 CET feature; -ENOSYS on RISC-V is the correct arch behavior |
| `futex_wait` | futex | `partial` | `smoke-proc-stress` | split-out futex_wait with timespec timeout |
| `futex_wake` | futex | `partial` | `smoke-proc-stress` | split-out futex_wake |
| `rt_tgsigqueueinfo` | signals | `partial` | `smoke-proc-stress` | queue a signal to a specific thread of a tgid |
| `riscv_hwprobe` | arch | `partial` | `smoke-abi-linux` | RISC-V hardware probing; reports IMA base behaviour |
| `riscv_flush_icache` | arch | `partial` | `smoke-abi-linux` | RISC-V icache flush over a range |
| `setxattrat` | path/fs | `partial` | `smoke-vfs-stress` | setxattr relative to a dirfd (Linux 6.x) |
| `getxattrat` | path/fs | `partial` | `smoke-vfs-stress` | getxattr relative to a dirfd (Linux 6.x) |
| `listxattrat` | path/fs | `partial` | `smoke-vfs-stress` | listxattr relative to a dirfd (Linux 6.x) |
| `removexattrat` | path/fs | `partial` | `smoke-vfs-stress` | removexattr relative to a dirfd (Linux 6.x) |
| `statmount` | path/fs | `partial` | `smoke-vfs-stress` | reports mount attributes into a statmnt buffer |
| `listmount` | path/fs | `partial` | `smoke-vfs-stress` | lists mount ids in the mount table |
| `listns` | namespaces | `partial` | `smoke-abi-linux` | reports the single kernel namespace id |
| `open_tree_attr` | path/fs | `partial` | `smoke-vfs-stress` | open_tree with attribute query |
| `file_getattr` | path/fs | `partial` | `smoke-syscall-ext` | LoongArch-only fileattr syscall (468); VFS core reports an empty attribute set (flags=0, masks=0) |
| `file_setattr` | path/fs | `partial` | `smoke-syscall-ext` | LoongArch-only fileattr syscall (469); refuses non-empty flag requests with -EOPNOTSUPP |
| `lsm_get_self_attr` | security | `partial` | `smoke-syscall-ext` | reports Landlock restriction state |
| `lsm_set_self_attr` | security | `partial` | `smoke-syscall-ext` | returns -EOPNOTSUPP (restrict via landlock_restrict_self) |
| `lsm_list_modules` | security | `partial` | `smoke-syscall-ext` | lists capability + landlock modules |
| `a20_channel_pair` | a20-ipc | `full` | `smoke-a20-channel` | A20OS extension: create a channel pair as fds (read/write per message); Linux ABI bridge to the unified channel IPC |
| `a20_registry_client` | a20-ipc | `full` | `smoke-a20-channel` | A20OS extension: open the well-known service-registry client endpoint as an fd |
<!-- LINUX_SYSCALL_COVERAGE_END -->

## Placeholder Resolution Record

Every former explicit `-ENOSYS` placeholder in `syscall_table.def` is now implemented; the table no longer contains fixed `-ENOSYS` placeholders. This record documents how each former placeholder was resolved.

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
| `userfaultfd` | **implemented** | MISSING-mode anonymous ranges with PAGEFAULT events and COPY/ZEROPAGE resolution (`kernel/ipc/userfaultfd.c`); no fork/shmem/WP modes. | Yes |
| `perf_event_open` | **implemented** | PERF_TYPE_SOFTWARE events with read(2) and the core ioctls (`kernel/abi/linux/sys_perf.c`); no PMU/hardware events or mmap ring. | Yes |

`arch_prctl` is not one of these 16 direct placeholders. The x86_64 implementation supports `ARCH_SET_FS`/`ARCH_GET_FS` and `ARCH_SET_GS`/`ARCH_GET_GS` plus `ARCH_GET_CPUID`; unsupported operations return `-EINVAL`, and the generic non-x86_64 fallback returns `-EOPNOTSUPP` (the arch-correct answer on platforms without x86 segment registers). The generated table classifies it `partial` for the x86_64 surface with the arch fallback documented.

**Scope Note:** The remaining `-ENOSYS` returns in the ABI are the arch/version-correct Linux semantics, not placeholders: `nfsservctl` was removed in Linux 4.19 and `map_shadow_stack` is an x86 CET syscall, so `-ENOSYS` on RISC-V matches Linux. The syscalls above are `partial`: they cover the common ABI surface with documented gaps in Linux edge semantics.
