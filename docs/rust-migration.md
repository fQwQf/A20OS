# Rust Migration Plan (RIIR)

This document describes how Rust is being introduced into the A20OS kernel
after the competition.

## Strategy

- **Incremental**, module-by-module rewrite.
- Each Rust module is built as an `rlib`.
- A root crate `kernel/rust/rust_kernel.rs` is built as a single `staticlib`
  that bundles all enabled module rlibs plus the shared support crate.  This
  staticlib is linked with the C kernel objects using the normal cross-compiler
  linker.  The single-staticlib approach lets `rustc` resolve the panic handler,
  `libcore`, and `compiler_builtins` exactly once, avoiding duplicate-symbol
  errors that occur when multiple rlibs are passed directly to `gcc`.
- The existing C implementation remains in place; a Makefile toggle selects
  the C or Rust version for each module.
- Public C headers are authoritative; Rust modules expose `extern "C"`
  functions with identical signatures.

## Toolchain

Pinned by `rust-toolchain.toml`:

- channel: `nightly-2025-01-18`
- components: `rust-src`, `rustfmt`, `clippy`
- targets:
  - `riscv64imac-unknown-none-elf`
  - `loongarch64-unknown-none`
  - `aarch64-unknown-none`
  - `x86_64-unknown-none`

The RISC-V target uses `imac` (soft-float `lp64` ABI) to match the C kernel's
`-mabi=lp64`.

## Build toggles

| Variable | Default | Meaning |
|----------|---------|---------|
| `RUST_ENABLED` | `0` | Master switch for Rust modules |
| `RUST_MODULE_XATTR` | `0` | Use Rust xattr implementation |
| `RUST_MODULE_TIMEKEEPING` | `0` | Use Rust timekeeping implementation |
| `RUST_MODULE_PAGECACHE` | `0` | Use Rust page cache implementation |
| `RUST_MODULE_BLOCKCACHE` | `0` | Use Rust block cache implementation |
| `RUST_MODULE_SYNC` | `0` | Use Rust waitqueue/mutex/completion implementation |
| `RUST_MODULE_SLAB` | `0` | Use Rust slab allocator implementation |
| `RUST_MODULE_STATPERM` | `0` | Use Rust stat/permission/time metadata implementation |
| `RUST_MODULE_PROC_LIST` | `0` | Use Rust all-task list implementation |
| `RUST_MODULE_RANDOM` | `0` | Use Rust kernel RNG implementation |
| `RUST_MODULE_EVENTFD` | `0` | Use Rust eventfd implementation |
| `RUST_MODULE_TIMERFD` | `0` | Use Rust timerfd implementation |
| `RUST_MODULE_LOCKS` | `0` | Use Rust POSIX/BSD file locking implementation |
| `RUST_MODULE_FDTABLE` | `0` | Use Rust fdtable implementation |
| `RUST_MODULE_FILE` | `0` | Use Rust global VFS file table implementation |
| `RUST_MODULE_PIPE` | `0` | Use Rust pipe implementation |
| `RUST_MODULE_SIGNAL` | `1` | Use Rust signal implementation |
| `RUST_MODULE_FUTEX` | `1` | Use Rust futex implementation |
| `RUST_MODULE_SCHED` | `1` | Use Rust scheduler implementation |

To build with all current Rust modules:

```bash
make RUST_ENABLED=1 RUST_MODULE_XATTR=1 RUST_MODULE_TIMEKEEPING=1 \
     RUST_MODULE_PAGECACHE=1 RUST_MODULE_BLOCKCACHE=1 RUST_MODULE_SYNC=1 \
     RUST_MODULE_SLAB=1 RUST_MODULE_STATPERM=1 RUST_MODULE_PROC_LIST=1 \
     RUST_MODULE_RANDOM=1 RUST_MODULE_EVENTFD=1 RUST_MODULE_TIMERFD=1 \
     RUST_MODULE_LOCKS=1 RUST_MODULE_FDTABLE=1 RUST_MODULE_FILE=1 \
     RUST_MODULE_PIPE=1 RUST_MODULE_SIGNAL=1 RUST_MODULE_FUTEX=1 \
     RUST_MODULE_SCHED=1 \
     ARCH=riscv64 kernel-only
```

## Phase 18

Rewrote `kernel/proc/sched.c` in Rust.

- New module: `kernel/rust/sched/`.
- Preserves the exported scheduler ABI (`proc_sched_runq_init`,
  `proc_sched_select_cpu{,_locked}`, `proc_runq_*`, `proc_set_{wake_time,alarm_expire}`,
  `proc_next_timer_interval`, `sched_reap_zombies`, `context_switch`, `sched`,
  `proc_yield`) while keeping `kernel/proc/sched.c` in-tree as the fallback
  implementation.
- Uses `IrqSaveSpinLock<RunQueue>` per CPU for runqueue state and thin C helpers
  in `kernel/rust/support/sched_helpers.c` for opaque `task_t` field access,
  ABI-conditional timer tick callbacks, and low-level switch glue.
- Toggle: `RUST_MODULE_SCHED=1` (default on).

## Adding a new Rust module

1. Create `kernel/rust/<module>/lib.rs` and `ffi.rs` if needed.
2. Add `RUST_MODULE_<NAME>` flag to the Makefile.
3. Conditionally filter the C source from `KERNEL_SRC` and add the Rust rlib
   to `RUST_LIBS`.
4. If the module needs C helpers that are `static inline` or macros, add thin
   C wrappers in `kernel/rust/support/` and list them in `RUST_SUPPORT_COBJ`.
5. Add `--crate-name <module>` to the rlib build rule so multiple modules with
   `lib.rs` roots do not collide.
6. Implement `extern "C"` wrappers preserving the existing header contract.
7. Add regression tests comparing C and Rust builds.

## Phase 1

Rewrite `kernel/fs/xattr.c` in Rust.  This module is leaf-like, allocation-free,
and has a small public API, making it ideal for proving the build/FFI pipeline.

## Phase 2

Rewrote `kernel/core/timekeeping.c` in Rust.

- New module: `kernel/rust/timekeeping/`.
- Adds shared C helpers in `kernel/rust/support/irqsave_lock.c` and
  `kernel/rust/support/arch_info.c` for irqsave locks and timer frequency.
- Toggle: `RUST_MODULE_TIMEKEEPING=1`.
- Builds for all four architectures.

## Phase 3

Rewrote `kernel/fs/page_cache.c` in Rust.

- New module: `kernel/rust/page_cache/`.
- Replaces the manual intrusive-list / single-global-lock internals with
  index-based lists protected by a safe irqsave spinlock wrapper, plus atomic
  fields for refcount and per-page flags.
- Adds shared C helpers in `kernel/rust/support/page_cache_helpers.c` for
  frame-to-virtual mapping, PFN validity, frame refcount, and vnode writeback.
- Toggle: `RUST_MODULE_PAGECACHE=1`.
- Builds for all four architectures; `smoke-vfs-stress` passes on riscv64.

## Phase 5

Rewrote `kernel/core/sync.c` in Rust.

- New module: `kernel/rust/sync/`.
- Replaces waitqueue, mutex, and completion linked-list manipulation under
  irqsave spinlocks with RAII-protected critical sections.
- Adds shared C helpers in `kernel/rust/support/sync_helpers.c` for
  `proc_current`, `proc_make_ready`, `sched`, and task-state get/set.
- Extends `kernel/rust/support/lock.rs` with `raw_irqsave_lock` /
  `RawIrqSaveGuard` so Rust can lock C-allocated `spinlock_t` fields.
- Toggle: `RUST_MODULE_SYNC=1`.
- Builds for all four architectures; `smoke-vfs-stress`,
  `smoke-futex-stress`, and `smoke-sched-stress` pass on riscv64, both with
  sync alone and with all current Rust modules enabled.

## Verification status

- Default C-only build (`RUST_ENABLED=0`) passes `make check-kernel-build` on
  all four architectures.
- All Rust modules enabled build and link on riscv64, loongarch64, aarch64,
  and x86_64.
- `smoke-vfs-stress` passes on riscv64 with `RUST_MODULE_PAGECACHE=1` both
  alone and together with `RUST_MODULE_XATTR=1` and `RUST_MODULE_TIMEKEEPING=1`.
- `RUST_MODULE_BLOCKCACHE=1` passes `make check-kernel-build` on all four
  architectures and `smoke-vfs-stress`/`smoke-vfs-edge` on riscv64.
- `RUST_MODULE_SYNC=1` passes `make check-kernel-build` on all four
  architectures and `smoke-vfs-stress`/`smoke-futex-stress`/
  `smoke-sched-stress` on riscv64.
- `RUST_MODULE_SLAB=1` passes `make check-kernel-build` on all four
  architectures and `smoke-vfs-stress`/`smoke-futex-stress`/
  `smoke-sched-stress` on riscv64, both alone and with all other Rust
  modules enabled.
- `RUST_MODULE_STATPERM=1` passes `make check-kernel-build` on all four
  architectures and `smoke-vfs-stress`/`smoke-futex-stress`/
  `smoke-sched-stress`/`smoke-vfs-edge` on riscv64.
- `RUST_MODULE_PROC_LIST=1` passes `make check-kernel-build` on all four
  architectures and `smoke-proc-a20`/`smoke-proc-stress`/
  `smoke-sched-stress`/`smoke-futex-stress`/`smoke-vfs-stress` on riscv64.
- `RUST_MODULE_RANDOM=1` passes `make check-kernel-build` on all four
  architectures and `smoke-proc-a20`/`smoke-futex-stress`/`smoke-sched-stress`
  on riscv64 (`smoke-vfs-stress` is blocked by an unrelated user-space build
  race, not by the RNG module).
- `RUST_MODULE_EVENTFD=1` passes `make check-kernel-build` on all four
  architectures and `smoke-futex-stress` on riscv64 (`smoke-proc-a20` and
  `smoke-sched-stress` are blocked by the same unrelated user-space build
  race).
- `RUST_MODULE_TIMERFD=1` passes `make check-kernel-build` on all four
  architectures and `smoke-sched-stress` on riscv64 (`smoke-proc-a20` and
  `smoke-futex-stress` are blocked by the same unrelated user-space build
  race).
- `RUST_MODULE_LOCKS=1` passes `make check-kernel-build` on all four
  architectures and `smoke-vfs-stress`/`smoke-futex-stress` on riscv64.
- `RUST_MODULE_FDTABLE=1` passes `make check-kernel-build` on all four
  architectures and `smoke-vfs-stress`/`smoke-futex-stress`/
  `smoke-sched-stress` on riscv64.
- `RUST_MODULE_FILE=1` passes `make check-kernel-build` on all four
  architectures and `smoke-vfs-stress`/`smoke-futex-stress`/
  `smoke-sched-stress` on riscv64.

## Phase 16

Rewrote `kernel/proc/signal.c` in Rust.

- New module: `kernel/rust/signal/`.
- Preserves the public `kernel/include/proc/signal.h` ABI and keeps the shared
  `signal_state_t` layout byte-identical with C.
- Uses `kernel/rust/support/signal_helpers.c` for opaque `task_t` access and
  architecture-specific rt-sigframe construction/restoration while keeping the
  signal policy, pending-mask semantics, and syscall ABI in Rust.
- Toggle: `RUST_MODULE_SIGNAL=1`.
- Verification target: four-arch `make kernel-only` plus riscv64
  `smoke-vfs-stress` / `smoke-futex-stress` / `smoke-sched-stress`.

## Phase 17

Rewrote `kernel/abi/linux/sys_futex.c` in Rust.

- New module: `kernel/rust/futex/`.
- Preserves the public `sys_futex`, `futex_wake_user`, and
  `exit_robust_list` C ABI while keeping the original fixed-size waiter table,
  wake-generation protocol, and wake-after-unlock behavior.
- Adds `kernel/rust/support/futex_helpers.c` so Rust can translate futex
  physical keys, read opaque task/mm state, and query kernel time sources
  without exposing `task_t` / `mm_struct_t` internals.
- Toggle: `RUST_MODULE_FUTEX=1`.
- Verification target: four-arch `make kernel-only` plus riscv64
  `smoke-futex-stress` / `smoke-sched-stress`.

## Phase 15

Rewrote `kernel/fs/pipe.c` in Rust.

- New module: `kernel/rust/pipe/`.
- Preserves the public `kernel/include/fs/pipe.h` ABI and the original
  pipe-specific exported symbols/callbacks.
- Replaces manual pipe-buffer locking with `IrqSaveSpinLock<PipeState>` and uses
  `wait_queue_prepare` / `sched` / `wait_queue_finish` to preserve the
  lost-wakeup-free blocking protocol around readers/writers.
- Adds `kernel/rust/support/pipe_helpers.c` so Rust can access `vfile_t`
  fields and task pid/state without duplicating full C structure layouts.
- Toggle: `RUST_MODULE_PIPE=1`.
- Builds for all four architectures; `smoke-vfs-stress`,
  `smoke-futex-stress`, and `smoke-sched-stress` pass on riscv64.

## Phase 6

Rewrote `kernel/mm/slab.c` in Rust.

- New module: `kernel/rust/slab/`.
- Replaces the global allocator's per-cache spinlocks, manual
  partial/full/spare intrusive lists, bitmap tracking, and free-list
  manipulation with RAII irqsave locks and index-based free lists inside
  each slab page.
- Adds shared C helpers in `kernel/rust/support/slab_helpers.c` for buddy
  allocator access, PFN/virtual-address conversion, and frame metadata.
- Toggle: `RUST_MODULE_SLAB=1`.
- Builds for all four architectures; `smoke-vfs-stress`,
  `smoke-futex-stress`, and `smoke-sched-stress` pass on riscv64, both with
  slab alone and with all current Rust modules enabled.

## Phase 7

Rewrote `kernel/fs/vfs/stat_perm.c` in Rust.

- New module: `kernel/rust/stat_perm/`.
- Fixes the unprotected global `g_time_meta` array race: a single
  `IrqSaveSpinLock<[Option<TimeMeta>; VFS_TIME_META_MAX]>` now protects all
  lookup/insert/update/drop operations.
- Avoids deadlock in `vfs_set_times` by reading old times via `vfs_vnode_stat`
  *before* acquiring the time-metadata lock.
- Adds shared C helpers in `kernel/rust/support/stat_perm_helpers.c` for
  vnode `(mnt, ino)` key extraction, `vn->ops->stat` fallback, and credential
  copying, plus `a20_timekeeping_get_realtime` in
  `kernel/rust/support/time_helpers.c`.
- Toggle: `RUST_MODULE_STATPERM=1`.
- Builds for all four architectures; `smoke-vfs-stress`, `smoke-futex-stress`,
  `smoke-sched-stress`, and `smoke-vfs-edge` pass on riscv64.

## Phase 8

Rewrote the all-task list primitives in `kernel/proc/proc.c` in Rust.

- New module: `kernel/rust/proc_list/`.
- Replaces `proc_link_task_locked`, `proc_unlink_task_locked`,
  `proc_first_task_locked`, and `proc_next_task_locked` to address
  `proc_next_task_locked` list-corruption panics observed in stress runs.
- Keeps `all_next`/`all_prev` embedded in `task_t`; Rust owns the list
  head/tail and all link mutations.  Callers continue to hold `proc_lock`;
  Rust primitives do not reacquire it.
- Adds shared C helpers in `kernel/rust/support/proc_list_helpers.c` for
  task field access, PID reading, kernel-address validation, and corrupt
  list logging.
- Toggle: `RUST_MODULE_PROC_LIST=1`.
- Builds for all four architectures; `smoke-proc-a20`,
  `smoke-proc-stress`, `smoke-sched-stress`, `smoke-futex-stress`, and
  `smoke-vfs-stress` pass on riscv64.

## Phase 9

Rewrote `kernel/core/random.c` in Rust.

- New module: `kernel/rust/random/`.
- Replaces the unprotected global `rng_s`/`rng_ready`/`rng_generation` state
  and manual `spin_lock_irqsave`/`spin_unlock_irqrestore` pairs with a single
  `IrqSaveSpinLock<RngState>` so the state can only be accessed while holding
  the lock.
- Preserves the xoshiro256** algorithm, splitmix64 seeding, and the
  inline-reseed-every-64-generations behavior of the C implementation.
- Adds a shared C helper in `kernel/rust/support/random_helpers.c` for entropy
  sampling (`timer_get_ticks`, `arch_read_addr_space_token`, `frame_free_count`,
  `proc_current`, `__builtin_return_address`, and stack-address leakage), since
  these constructs cannot be expressed directly in Rust.
- Toggle: `RUST_MODULE_RANDOM=1`.
- Builds for all four architectures; `smoke-proc-a20`, `smoke-futex-stress`,
  and `smoke-sched-stress` pass on riscv64.

## Phase 10

Rewrote `kernel/ipc/eventfd.c` in Rust.

- New module: `kernel/rust/eventfd/`.
- Replaces the manual `spin_lock`/`spin_unlock` pairs around the eventfd
  counter and waitqueues with a single `IrqSaveSpinLock<EventFdState>`,
  eliminating a class of lock-release-forget or interrupt-unsafe races.
- Keeps the C waitqueue ABI (`wait_queue_sleep`/`wait_queue_wake_all`) but
  captures the waitqueue pointer while the lock guard is alive, then drops the
  guard before sleeping to preserve the original "release lock, sleep,
  reacquire" semantics.
- Adds shared C helpers in `kernel/rust/support/eventfd_helpers.c` for
  `vfile_alloc`, `vfile_free`, vfile priv access, and anonymous-fd
  installation, since these helpers need the full `vfile_t` / `vfile_ops_t`
  definitions from the C headers.
- Toggle: `RUST_MODULE_EVENTFD=1`.
- Builds for all four architectures; `smoke-futex-stress` passes on riscv64.

## Phase 11

Rewrote `kernel/ipc/timerfd.c` in Rust.

- New module: `kernel/rust/timerfd/`.
- Replaces the manual `spin_lock`/`spin_unlock` pairs around the timerfd
  state and waitqueue with a single `IrqSaveSpinLock<TimerFdState>`,
  eliminating lock-leak / stale-lock-state races.
- Preserves the original "release lock, sleep, reacquire" pattern around
  `wait_queue_sleep` by capturing the waitqueue pointer from the guard before
  dropping it.
- Adds shared C helpers in `kernel/rust/support/timerfd_helpers.c` for
  `vfile_alloc`, `vfile_free`, vfile priv access, anonymous-fd installation,
  and ops-pointer matching (needed by `timerfd_settime_file` and
  `timerfd_gettime_file` to validate that a global fd is a timerfd).
- Toggle: `RUST_MODULE_TIMERFD=1`.
- Builds for all four architectures; `smoke-sched-stress` passes on riscv64.

## Phase 12 candidates

## Phase 12

Rewrote `kernel/fs/locks.c` in Rust.

- New module: `kernel/rust/locks/`.
- Preserves the public `kernel/include/fs/locks.h` ABI and the original C
  semantics for POSIX byte-range locks and BSD `flock()` entries, including the
  256-entry global tables, range splitting/merging, blocking retry loops, and
  release paths.
- Replaces manual global lock management with a single
  `IrqSaveSpinLock<LockState>` protecting both lock tables plus the global
  waiter queue.
- Uses `wait_queue_prepare` + `sched` + `wait_queue_finish` for blocking
  waiters instead of `wait_queue_sleep`, eliminating the lost-wakeup window
  between dropping the table lock and enqueueing on the wait queue.
- Adds shared C helpers in `kernel/rust/support/locks_helpers.c` for `vfile_t`
  key/size/offset extraction and current-pid access, keeping Rust independent of
  full VFS structure layouts.
- Toggle: `RUST_MODULE_LOCKS=1`.
- Builds for all four architectures; `smoke-vfs-stress` and
  `smoke-futex-stress` pass on riscv64.

## Phase 13

Rewrote `kernel/fs/fdtable.c` in Rust.

- New module: `kernel/rust/fdtable/`.
- Preserves the public `kernel/include/fs/fdtable.h` ABI and the original C
  semantics for fd install/duplicate/close, copy-on-write (`fdtable_unshare`),
  sharing (`fdtable_share`), close-on-exec, and process exit paths.
- Replaces the per-`files_struct_t` manual `spin_lock_irqsave`/
  `spin_unlock_irqrestore` pairs with a single `IrqSaveSpinLock<FdTableInner>`,
  and uses `AtomicI32` for the reference count, removing stale-lock-state and
  refcount races.
- Keeps `task->files` as an opaque pointer managed via C helpers in
  `kernel/rust/support/fdtable_helpers.c`, so Rust does not need to replicate
  the full `task_t` layout.
- Toggle: `RUST_MODULE_FDTABLE=1`.
- Builds for all four architectures; `smoke-vfs-stress`,
  `smoke-futex-stress`, and `smoke-sched-stress` pass on riscv64.

## Phase 14

Rewrote `kernel/fs/file.c` in Rust.

- New module: `kernel/rust/file/`.
- Preserves the public `kernel/include/fs/file.h` and `kernel/include/fs/vfs.h`
  ABI for the global VFS file table (`vfs_get_file`, `vfs_get_file_ref`,
  `vfs_alloc_fd`, `vfs_dup`, `vfs_dup3`, `file_close_prepare`, etc.).
- Replaces the global `g_file_lock` manual `spin_lock_irqsave`/
  `spin_unlock_irqrestore` pairs with a single `IrqSaveSpinLock<FileTableState>`,
  eliminating stale-lock-state and mask-update races.
- Keeps `vfile_t` as an opaque C type; refcount and object-cache operations
  stay in C helpers in `kernel/rust/support/file_helpers.c` so Rust does not
  need to replicate the full `vfile_t` layout.
- Toggle: `RUST_MODULE_FILE=1`.
- Builds for all four architectures; `smoke-vfs-stress`,
  `smoke-futex-stress`, and `smoke-sched-stress` pass on riscv64.

- `kernel/proc/signal.c` — signal pending/delivery races, ~546 lines; high
  theoretical ROI but touches arch-specific signal frames.

Modules intentionally left as C unless a concrete bug cluster is proven:
`kernel/fs/ext4.c`, `kernel/fs/fat32.c`, `kernel/fs/vfs.c` core, pseudo
filesystems (`procfs/devfs/ramfs/sysfs/cgroupfs`), `arch/`, `drivers/`,
`external/lwip`, and simple sequential core code (`printf.c`, `klog.c`,
`panic.c`).
