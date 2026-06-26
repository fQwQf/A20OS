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

#[cfg(rust_module_dcache)]
extern crate dcache;

#[cfg(rust_module_xattr)]
extern crate xattr;

#[cfg(rust_module_timekeeping)]
extern crate timekeeping;

#[cfg(rust_module_sync)]
extern crate sync;

#[cfg(rust_module_slab)]
extern crate slab;

#[cfg(rust_module_stat_perm)]
extern crate stat_perm;

#[cfg(rust_module_proc_list)]
extern crate proc_list;

#[cfg(rust_module_random)]
extern crate random;

#[cfg(rust_module_eventfd)]
extern crate eventfd;

#[cfg(rust_module_timerfd)]
extern crate timerfd;

#[cfg(rust_module_locks)]
extern crate locks;

#[cfg(rust_module_fdtable)]
extern crate fdtable;

#[cfg(rust_module_file)]
extern crate file;

#[cfg(rust_module_pipe)]
extern crate pipe;

#[cfg(rust_module_signal)]
extern crate signal;

#[cfg(rust_module_futex)]
extern crate futex;

#[cfg(rust_module_sched)]
extern crate sched;

#[cfg(rust_module_wait)]
extern crate wait;

#[cfg(rust_module_proc_core)]
extern crate proc_core;

#[cfg(rust_module_socket_registry)]
extern crate socket_registry;

#[cfg(rust_module_a20_event)]
extern crate a20_event;

#[cfg(rust_module_sysv_sem)]
extern crate sysv_sem;

#[cfg(rust_module_sysv_shm)]
extern crate sysv_shm;
