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

To build with all current Rust modules:

```bash
make RUST_ENABLED=1 RUST_MODULE_XATTR=1 RUST_MODULE_TIMEKEEPING=1 \
     RUST_MODULE_PAGECACHE=1 RUST_MODULE_BLOCKCACHE=1 RUST_MODULE_SYNC=1 \
     RUST_MODULE_SLAB=1 \
     ARCH=riscv64 kernel-only
```

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

## Phase 7 candidates

- `kernel/fs/vfs/stat_perm.c` — permission/time metadata (needs stable access
  to `proc_cred_t`; consider adding a C helper or using bindgen).
- `kernel/core/random.c` — self-contained RNG, but depends on entropy helpers
  and irqsave locks.
- `kernel/fs/vfs/vnode.c` — vnode reference counting and lifecycle; moderate
  blast radius but clear concurrency boundaries.
