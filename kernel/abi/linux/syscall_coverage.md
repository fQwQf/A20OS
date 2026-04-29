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
