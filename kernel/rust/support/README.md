# kernel/rust/support

Shared support code for all Rust kernel modules.

- `panic_handler.rs`: provides the kernel-wide `#[panic_handler]`.
  Each individual module crate is built as an `rlib` and linked together with
  this crate so only one panic handler exists in the final kernel image.
