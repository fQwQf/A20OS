//! Kernel VFS lifecycle controllers. C keeps pointer/refcount/lock and I/O
//! primitives; Rust owns shutdown phase and backend-close policy transitions.
#![no_std]

#[path = "shutdown_state.rs"]
mod shutdown_state;
#[path = "vfile_lifecycle.rs"]
mod vfile_lifecycle;
