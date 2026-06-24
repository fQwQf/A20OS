# Rust Migration Plan (RIIR)

This document describes how Rust is being introduced into the A20OS kernel
after the competition.

## Strategy

- **Incremental**, module-by-module rewrite.
- Each Rust module is built as an `rlib` and linked into the kernel ELF.
- The existing C implementation remains in place; a Makefile toggle selects
  the C or Rust version for each module.
- Public C headers are authoritative; Rust modules expose `extern "C"`
  functions with identical signatures.

## Toolchain

Pinned by `rust-toolchain.toml`:

- channel: `nightly-2025-01-18`
- components: `rust-src`, `rustfmt`, `clippy`
- targets:
  - `riscv64gc-unknown-none-elf`
  - `loongarch64-unknown-none`
  - `aarch64-unknown-none`
  - `x86_64-unknown-none`

## Build toggles

| Variable | Default | Meaning |
|----------|---------|---------|
| `RUST_ENABLED` | `0` | Master switch for Rust modules |
| `RUST_MODULE_XATTR` | `0` | Use Rust xattr implementation |

To build with the Rust xattr module:

```bash
make RUST_ENABLED=1 RUST_MODULE_XATTR=1 ARCH=riscv64 kernel-only
```

## Adding a new Rust module

1. Create `kernel/rust/<module>/lib.rs` and `ffi.rs` if needed.
2. Add `RUST_MODULE_<NAME>` flag to the Makefile.
3. Conditionally filter the C source from `KERNEL_SRC` and add the Rust rlib
   to `RUST_LIBS`.
4. Implement `extern "C"` wrappers preserving the existing header contract.
5. Add regression tests comparing C and Rust builds.

## Phase 1

Rewrite `kernel/fs/xattr.c` in Rust.  This module is leaf-like, allocation-free,
and has a small public API, making it ideal for proving the build/FFI pipeline.

## Phase 2 candidates

- `kernel/fs/vfs/stat_perm.c` — permission/time metadata, addresses the recent
  VFS time-metadata exhaustion bug class.
- `kernel/core/random.c` — self-contained stateful service.
- `kernel/core/timekeeping.c` — small state with arithmetic invariants.

## Non-goals

- Full kernel rewrite in the near term.
- Replacing the slab allocator, scheduler, or VFS core as an early module.
- Introducing `std` or `alloc` in phase 1.
