# kernel/rust/xattr

Rust implementation of the A20OS extended attribute (xattr) table.

This module replaces `kernel/fs/xattr.c` when `RUST_MODULE_XATTR=1` is set
at build time.  The public C ABI is identical to the original:

- `xattr_check_namespace`
- `xattr_set_vnode`
- `xattr_get_vnode`
- `xattr_list_vnode`
- `xattr_remove_vnode`
- `xattr_cleanup_vnode`

## Design notes

- `#![no_std]`, no allocation.
- The global table is protected by a C-compatible spinlock because xattr can
  be called from syscall context on multiple CPUs.
- FFI bindings in `ffi.rs` are hand-written for the small set of C services
  used here (`strlen`, `strncpy`, `proc_current`, `proc_has_cap`, `spin_lock`,
  `spin_unlock`).
- Panic handling is provided by the shared `kernel/rust/support/panic_handler.rs`
  crate.
