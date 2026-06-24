//! Root crate that bundles all enabled Rust kernel modules into a single
//! `staticlib`.  This staticlib is then linked with the C kernel objects using
//! the normal cross-compiler linker.
//!
//! Putting all modules behind one staticlib lets `rustc` resolve the panic
//! handler, `libcore`, and `compiler_builtins` exactly once, avoiding the
//! duplicate-symbol problems that come from manually linking multiple rlibs
//! with `gcc`.

#![no_std]

extern crate a20rust_support;

#[cfg(rust_module_page_cache)]
extern crate page_cache;

#[cfg(rust_module_block_cache)]
extern crate block_cache;

#[cfg(rust_module_xattr)]
extern crate xattr;

#[cfg(rust_module_timekeeping)]
extern crate timekeeping;

#[cfg(rust_module_sync)]
extern crate sync;

#[cfg(rust_module_slab)]
extern crate slab;
