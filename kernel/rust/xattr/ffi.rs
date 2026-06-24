//! FFI bindings for the small set of C services used by the Rust xattr module.
//!
//! In phase 1 these are hand-written to avoid a bindgen dependency.  They are
//! intentionally minimal: only the functions and constants that xattr needs.

use core::ffi::{c_char, c_int};

// Error numbers from the C ABI (Linux-compatible).
pub const ENOENT: c_int = 2;
pub const EINVAL: c_int = 22;
pub const ERANGE: c_int = 34;
pub const EEXIST: c_int = 17;
pub const ENODATA: c_int = 61;
pub const ENOSPC: c_int = 28;
pub const EPERM: c_int = 1;

pub const CAP_SYS_ADMIN: c_int = 21;

/// Opaque C `task_t`.
#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

/// Mirror of `spinlock_t` from `kernel/include/core/lock.h`.
/// In non-debug builds it is a single `volatile int`.
#[repr(C)]
pub struct spinlock_t {
    locked: c_int,
}

impl spinlock_t {
    pub const fn new() -> Self {
        Self { locked: 0 }
    }
}

extern "C" {
    pub fn strlen(s: *const c_char) -> usize;
    pub fn strncpy(dst: *mut c_char, src: *const c_char, n: usize) -> *mut c_char;

    pub fn proc_current() -> *mut task_t;
    pub fn proc_has_cap(task: *mut task_t, cap: c_int) -> c_int;

    pub fn spin_lock(lock: *mut spinlock_t);
    pub fn spin_unlock(lock: *mut spinlock_t);
}
