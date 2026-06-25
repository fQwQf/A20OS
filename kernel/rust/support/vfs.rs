//! C-compatible VFS type layouts shared between Rust kernel modules and the C kernel.
//!
//! These definitions must be kept in exact sync with `kernel/include/fs/vfs.h`
//! and the structures it depends on (`spinlock_t`, `wait_queue_t`, `mutex_t`).
//! The C build verifies offsets with `_Static_assert` in `vfs_types_check.c`.

use core::ffi::{c_char, c_int, c_long, c_ulong, c_void};
use core::sync::atomic::{AtomicI32, Ordering};

/// Mirrors `refcount_t` from `kernel/include/core/refcount.h`.
#[repr(C)]
pub struct refcount_t {
    pub value: AtomicI32,
}

impl refcount_t {
    #[inline(always)]
    pub unsafe fn set(&mut self, v: c_int) {
        self.value.store(v, Ordering::Relaxed);
    }

    #[inline(always)]
    pub unsafe fn read(&self) -> c_int {
        self.value.load(Ordering::Relaxed)
    }

    #[inline(always)]
    pub unsafe fn inc(&self) {
        self.value.fetch_add(1, Ordering::Relaxed);
    }

    #[inline(always)]
    pub unsafe fn inc_not_zero(&self) -> bool {
        let mut old = self.value.load(Ordering::Relaxed);
        while old != 0 {
            match self.value.compare_exchange_weak(
                old,
                old + 1,
                Ordering::Relaxed,
                Ordering::Relaxed,
            ) {
                Ok(_) => return true,
                Err(v) => old = v,
            }
        }
        false
    }

    #[inline(always)]
    pub unsafe fn dec_and_test(&self) -> bool {
        self.value.fetch_sub(1, Ordering::AcqRel) == 1
    }
}

/// Mirrors `spinlock_t` from `kernel/include/core/lock.h` (debug fields excluded).
#[repr(C)]
pub struct spinlock_t {
    pub locked: i32,
}

/// Mirrors `wait_queue_entry_t` from `kernel/include/core/sync.h`.
#[repr(C)]
pub struct wait_queue_entry_t {
    pub next: *mut wait_queue_entry_t,
    pub prev: *mut wait_queue_entry_t,
    pub task: *mut c_void,
}

/// Mirrors `wait_queue_t` from `kernel/include/core/sync.h`.
#[repr(C)]
pub struct wait_queue_t {
    pub lock: spinlock_t,
    pub head: *mut wait_queue_entry_t,
}

/// Mirrors `mutex_t` from `kernel/include/core/sync.h`.
#[repr(C)]
pub struct mutex_t {
    pub lock: spinlock_t,
    pub locked: c_int,
    pub owner: *mut c_void,
    pub waiters: wait_queue_t,
}

/// Mirrors `MAX_PATH_LEN` from `kernel/include/core/consts.h`.
pub const MAX_PATH_LEN: usize = 512;

/// Opaque vnode handle; only used through pointers.
#[repr(C)]
pub struct vnode_t {
    _opaque: [u8; 0],
}

/// Mirrors `vfile_ops_t` from `kernel/include/fs/vfs.h`.
#[repr(C)]
pub struct vfile_ops_t {
    pub read: Option<extern "C" fn(*mut vfile_t, *mut c_char, usize) -> c_int>,
    pub write: Option<extern "C" fn(*mut vfile_t, *const c_char, usize) -> c_int>,
    pub lseek: Option<extern "C" fn(*mut vfile_t, c_long, c_int) -> c_long>,
    pub readdir: Option<extern "C" fn(*mut vfile_t, *mut c_void, usize) -> c_int>,
    pub ioctl: Option<extern "C" fn(*mut vfile_t, c_ulong, *mut c_void) -> c_int>,
    pub close: Option<extern "C" fn(*mut vfile_t) -> c_int>,
}

/// Mirrors `vfile_t` from `kernel/include/fs/vfs.h`.
#[repr(C)]
pub struct vfile_t {
    pub vnode: *mut vnode_t,
    pub flags: c_int,
    pub offset: usize,
    pub offset_lock: mutex_t,
    pub ref_count: refcount_t,
    pub owner_type: c_int,
    pub owner_pid: c_int,
    pub owner_signal: c_int,
    pub seals: c_int,
    pub lease: c_int,
    pub rw_hint: u64,
    pub path: [c_char; MAX_PATH_LEN],
    pub ops: *mut vfile_ops_t,
    pub priv_data: *mut c_void,
}

impl vfile_t {
    #[inline(always)]
    pub unsafe fn ref_init(&mut self, refs: c_int) {
        self.ref_count.set(refs);
    }

    #[inline(always)]
    pub unsafe fn ref_read(&self) -> c_int {
        self.ref_count.read()
    }

    #[inline(always)]
    pub unsafe fn get(&self) {
        self.ref_count.inc();
    }

    #[inline(always)]
    pub unsafe fn put_ref_only(&self) -> bool {
        self.ref_count.dec_and_test()
    }
}
