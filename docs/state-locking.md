# Kernel Global State and Locking Rules

This document records current global kernel state and the lock that is expected
to protect it. Some rules describe the current single-CPU implementation rather
than the final SMP design; those entries are marked explicitly.

## Naming Rules

- `get` should acquire a reference unless the function documents otherwise.
- `put` should release a reference.
- `lookup` and `find` should not acquire references unless the function name or
  comment says so.
- A function ending in `_locked` requires its caller to hold the subsystem lock
  documented next to that object.

## Process State

| State | Owner | Current Protection | Notes |
| --- | --- | --- | --- |
| `proc_table` | `kernel/proc/proc.c` | `proc_lock` for allocation, state transitions, PID/run queue membership | Some readers still scan under single-CPU assumptions. |
| `current_task` | `kernel/proc/proc.c` | single-CPU invariant | Must become per-CPU before SMP. |
| PID hash | `kernel/proc/proc.c` | `proc_lock` | Should move to `proc/pid.c`. |
| run queue | `kernel/proc/sched.c` | `proc_lock`, caller-held | Already split from `proc.c`; still global, not per-CPU. |
| wake/alarm scan deadlines | `kernel/proc/proc.c` | atomic relaxed updates plus scheduler scan | Needs a clearer timer queue long term. |

## VFS State

| State | Owner | Current Protection | Notes |
| --- | --- | --- | --- |
| mount table | `kernel/fs/vfs.c` | mostly init/single-CPU assumptions | Needs a mount lock before namespace work. |
| dcache | `kernel/fs/vfs.c` | `g_dcache_lock` | Entries hold vnode references and must release on invalidation. |
| global open file table | `kernel/fs/file.c` | file-table lock inside file layer | `vfs_get_file()` lifetime semantics still need tightening. |
| vnode refcount | VFS/filesystems | ad hoc integer updates | Should move to `vnode_get()`/`vnode_put()` and `refcount_t`. |
| vfile refcount | file layer/VFS | ad hoc integer updates | Should move to `file_get()`/`file_put()`. |

## MM State

| State | Owner | Current Protection | Notes |
| --- | --- | --- | --- |
| physical frame allocator | `kernel/mm/frame.c` | frame allocator lock | Public API should hide allocator internals. |
| slab/object caches | `kernel/mm/slab.c`, `kernel/mm/objcache.c` | cache-local locks | Keep cache users from bypassing cache APIs. |
| `mm_struct` refcount | `kernel/mm/vm.c` and proc paths | ad hoc integer updates | Needs `refcount_t` and documented VMA/page-table lock. |
| VMA list/page table | `kernel/mm/vm.c`, `kernel/mm/mm.c` | current-process assumptions in some paths | Needs an `mm` lock before SMP/threaded user processes. |

## IPC State

| State | Owner | Current Protection | Notes |
| --- | --- | --- | --- |
| eventfd objects | `kernel/ipc/eventfd.c` | object-local state plus file lifetime | Should use wait queues eventually. |
| timerfd objects | `kernel/ipc/timerfd.c` | object-local state plus timer polling | Time source remains `core`/`arch`, not IPC. |
| SysV shm table | `kernel/ipc/sysv_shm.c` | subsystem lock if present in implementation | Needs consistent refcount/permission rules. |

## Network State

| State | Owner | Current Protection | Notes |
| --- | --- | --- | --- |
| socket table | `kernel/net/socket.c` | `g_net_lock` | Should move to `socket_registry.c`. |
| socket rx/accept queues | `kernel/net/socket.c` | `g_net_lock` | Should move to `socket_queue.c` and later wait queues. |
| lwIP stack | `kernel/net/lwip_stack.c` | net polling/single-thread assumptions | ABI/VFS must not depend on lwIP internals. |

## Block Cache State

| State | Owner | Current Protection | Notes |
| --- | --- | --- | --- |
| block cache entries | `kernel/fs/block_cache.c` | block cache lock | Eviction should return failure or wait instead of panicking when all entries are pinned. |

## Lock Order Draft

The current code does not yet enforce lock ordering. Until lockdep exists,
prefer this order:

```text
proc/tasklist
  -> files/fdtable
    -> VFS mount/dcache/vnode
      -> filesystem-private locks
        -> block cache
net socket lock should not be held across VFS or block I/O calls.
```

Never sleep or call into disk/network completion paths while holding a
spinlock. Once `might_sleep()` exists, every blocking path should use it in
debug builds.
