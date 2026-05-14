# A20OS Module Boundaries

This document records the intended long-term boundaries between kernel
subsystems. It is a maintenance contract: new code should prefer these
interfaces instead of reaching into another subsystem's private state.

## Dependency Direction

The desired dependency direction is:

```text
arch -> core
mm -> core, arch
proc -> core, arch, mm
fs -> core, mm, proc credentials/context
ipc -> core, proc, fs file objects where needed
net -> core, proc, fs file objects where needed
drv -> core, arch
abi/linux -> core APIs, mm APIs, proc APIs, fs APIs, ipc APIs, net APIs
```

`abi/linux` is a compatibility layer. It should copy arguments, translate
Linux structures and flags, and call internal APIs. It should not own global
kernel objects or implement core algorithms.

## Core

`kernel/core` owns basic kernel infrastructure:

- logging, panic, printf, string helpers
- random source
- generic trap dispatch glue
- timekeeping over architecture timer ticks
- future debug helpers such as `might_sleep()` and lightweight lock checking

Architecture-specific timer registers and trap entry code stay in
`kernel/arch/*`.

## MM

`kernel/mm` owns physical pages, slab/object caches, page tables, VMA state,
ELF loading, usercopy, COW, and future page cache integration.

Rules:

- User memory access goes through usercopy helpers.
- VMA and page-table mutations should eventually be protected by an `mm` lock.
- COW and demand-fault behavior should remain outside ABI-specific code.

## Proc

`kernel/proc` owns task lifetime, PID allocation, scheduler queues, fork/exec,
wait/reparent, signals, credentials, and limits.

Rules:

- External code should use task/proc APIs instead of directly editing task
  state.
- `current_task` is currently a single-CPU current pointer. Long term it should
  become per-CPU state.
- PID lookup, run queues, and task lifetime should be separated as `proc.c`
  is split.

## FS and VFS

VFS belongs under `kernel/fs`, not as a top-level `kernel/vfs`.

Long-term layout:

```text
kernel/fs/vfs/
  path.c
  mount.c
  vnode.c
  file.c
  fdtable.c
  stat.c
  poll.c
```

Rules:

- Path-based operations should go through VFS path APIs.
- Open-file operations should use file APIs.
- Vnode lifetime should use `vnode_get()` and `vnode_put()` once that API is
  introduced.
- `*_get()` should acquire a reference; `lookup/find` should not unless
  explicitly documented.

## IPC

`kernel/ipc` owns cross-process notification, synchronization, and shared
objects such as `eventfd`, `timerfd`, and SysV shared memory.

Timer hardware and timekeeping do not belong here; `timerfd` belongs here
because it is an fd-backed wait object.

## Net

`kernel/net` owns sockets, socket file bridges, and lwIP integration.

Rules:

- lwIP internals stay inside `kernel/net`.
- ABI and VFS code should not directly depend on lwIP data structures.
- Socket lookup should eventually return referenced socket objects with a
  matching put operation.

## Drivers

`kernel/drv` owns block, net, UART, and future device drivers. Drivers expose
block/net/char interfaces upward and should not depend on pathname-level VFS
logic.

## ABI

`kernel/abi/linux` is the Linux-compatible ABI subset. It is not a native A20OS
ABI and is not a promise of complete Linux kernel compatibility.

`kernel/abi/native` is reserved for the future A20OS native ABI design. It
should remain capability/handle-oriented and should not inherit Linux syscall
history unless there is a strong reason.
