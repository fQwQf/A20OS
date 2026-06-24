//! Safe irqsave spinlock wrapper around the C `spinlock_t` ABI.
//!
//! This wrapper turns the manual `spin_lock_irqsave` / `spin_unlock_irqrestore`
//! pattern into a Rust RAII guard.  The protected data can only be accessed
//! through the guard, making it impossible to touch the shared state without
//! holding the lock and without interrupts disabled.
//!
//! # Safety notes
//! - The wrapper is `Sync` only when `T: Send`, matching the standard library
//!   `Mutex` contract.
//! - The underlying C lock is the same `spinlock_t` used by the rest of the
//!   kernel, so lock-order contracts with C code are preserved.
//! - This wrapper intentionally uses the *non-debug* layout of `spinlock_t`
//!   (`locked: i32`).  Builds with `CONFIG_DEBUG_LOCKS=1` must not enable Rust
//!   modules unless the layout below is updated to match the extra debug fields.

use core::cell::UnsafeCell;
use core::ops::{Deref, DerefMut};

/// C-compatible spinlock state.  Must match `struct spinlock` in
/// `kernel/include/core/lock.h` for non-debug builds.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct spinlock_t {
    pub locked: i32,
}

/// Initializer compatible with `SPINLOCK_INIT`.
pub const SPINLOCK_INIT: spinlock_t = spinlock_t { locked: 0 };

extern "C" {
    fn a20_irqsave_lock(lock: *mut spinlock_t) -> u64;
    fn a20_irqsave_unlock(lock: *mut spinlock_t, flags: u64);
}

/// A spinlock that disables interrupts while held.
pub struct IrqSaveSpinLock<T> {
    lock: spinlock_t,
    data: UnsafeCell<T>,
}

// Safe because the data is only accessed while the lock is held.
unsafe impl<T: Send> Sync for IrqSaveSpinLock<T> {}

impl<T> IrqSaveSpinLock<T> {
    /// Create a new lock protecting `data`.
    pub const fn new(data: T) -> Self {
        Self {
            lock: SPINLOCK_INIT,
            data: UnsafeCell::new(data),
        }
    }

    /// Acquire the lock, disable interrupts, and return a guard.
    pub fn lock(&self) -> IrqSaveGuard<'_, T> {
        let flags = unsafe { a20_irqsave_lock(self.lock_ptr()) };
        IrqSaveGuard { lock: self, flags }
    }

    fn lock_ptr(&self) -> *mut spinlock_t {
        &self.lock as *const _ as *mut _
    }
}

/// RAII guard for `IrqSaveSpinLock`.  Releases the lock and restores interrupts
/// when dropped.
pub struct IrqSaveGuard<'a, T> {
    lock: &'a IrqSaveSpinLock<T>,
    flags: u64,
}

impl<'a, T> Deref for IrqSaveGuard<'a, T> {
    type Target = T;
    fn deref(&self) -> &T {
        // Safety: we hold the lock, so exclusive access is guaranteed.
        unsafe { &*self.lock.data.get() }
    }
}

impl<'a, T> DerefMut for IrqSaveGuard<'a, T> {
    fn deref_mut(&mut self) -> &mut T {
        // Safety: we hold the lock, so exclusive access is guaranteed.
        unsafe { &mut *self.lock.data.get() }
    }
}

impl<'a, T> Drop for IrqSaveGuard<'a, T> {
    fn drop(&mut self) {
        unsafe { a20_irqsave_unlock(self.lock.lock_ptr(), self.flags) };
    }
}

/// Lock a raw `spinlock_t` pointer that is owned/allocated by C code.
///
/// # Safety
/// `lock` must be a valid, initialized `spinlock_t` and must remain valid
/// until the returned guard is dropped.  Caller must obey the kernel lock
/// ordering rules.
pub unsafe fn raw_irqsave_lock(lock: *mut spinlock_t) -> RawIrqSaveGuard {
    let flags = a20_irqsave_lock(lock);
    RawIrqSaveGuard { lock, flags }
}

/// RAII guard returned by `raw_irqsave_lock`.  Releases the lock and restores
/// interrupts when dropped.
pub struct RawIrqSaveGuard {
    lock: *mut spinlock_t,
    flags: u64,
}

impl Drop for RawIrqSaveGuard {
    fn drop(&mut self) {
        unsafe { a20_irqsave_unlock(self.lock, self.flags) };
    }
}
