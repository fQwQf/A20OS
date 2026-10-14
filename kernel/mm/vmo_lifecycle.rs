#![no_std]

//! Rust-owned VMO reference and canonical-slot lookup entrypoints.
//!
//! The reference-count atomics and VMO lock remain narrow C primitives. The
//! Rust entrypoints own null handling, the final-reference decision, destroy
//! dispatch, and the locked slot lookup bounds check.

use core::ffi::c_void;
#[cfg(test)]
use core::ptr;

const PFN_NONE: u32 = u32::MAX;

unsafe extern "C" {
    fn a20_vmo_lifecycle_ref_inc(vmo: *mut c_void);
    fn a20_vmo_lifecycle_ref_dec_last(vmo: *mut c_void) -> i32;
    fn a20_vmo_destroy_rust(vmo: *mut c_void);
    fn a20_vmo_lifecycle_lock(vmo: *mut c_void, flags: *mut u64) -> i32;
    fn a20_vmo_lifecycle_unlock(vmo: *mut c_void, flags: u64);
    fn a20_vmo_lifecycle_page_count_locked(vmo: *mut c_void) -> u32;
    fn a20_vmo_lifecycle_slot_locked(vmo: *mut c_void, index: u32) -> u32;
}

/// Increment a live VMO reference using the same relaxed ordering as the C
/// refcount helper. The caller must already hold a reference or equivalent
/// synchronization that keeps the object alive.
#[no_mangle]
pub unsafe extern "C" fn a20_vmo_ref_rust(vmo: *mut c_void) {
    if !vmo.is_null() {
        // SAFETY: the public contract requires a live VMO for non-null input.
        unsafe { a20_vmo_lifecycle_ref_inc(vmo) };
    }
}

/// Drop a reference. The acquire-release decrement chooses exactly one final
/// owner; only that owner enters the existing Rust destruction transaction.
#[no_mangle]
pub unsafe extern "C" fn a20_vmo_release_rust(vmo: *mut c_void) {
    if vmo.is_null() {
        return;
    }
    // SAFETY: the caller transfers one owned reference to this function.
    if unsafe { a20_vmo_lifecycle_ref_dec_last(vmo) } != 0 {
        return;
    }
    // SAFETY: the decrement reported this as the unique final reference.
    unsafe { a20_vmo_destroy_rust(vmo) };
}

/// Return the materialized canonical PFN for a page without faulting it in.
#[no_mangle]
pub unsafe extern "C" fn a20_vmo_peek_page_rust(vmo: *mut c_void, index: u32) -> u32 {
    if vmo.is_null() {
        return PFN_NONE;
    }
    let mut flags = 0u64;
    // SAFETY: the primitive only acquires the VMO lock and records irq flags.
    if unsafe { a20_vmo_lifecycle_lock(vmo, &mut flags) } != 0 {
        return PFN_NONE;
    }
    // SAFETY: both reads occur while the VMO lock excludes resize/publication.
    let page_count = unsafe { a20_vmo_lifecycle_page_count_locked(vmo) };
    let pfn = if index < page_count {
        unsafe { a20_vmo_lifecycle_slot_locked(vmo, index) }
    } else {
        PFN_NONE
    };
    // SAFETY: lock acquisition succeeded and this is the sole unlock.
    unsafe { a20_vmo_lifecycle_unlock(vmo, flags) };
    pfn
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::sync::atomic::{AtomicU32, AtomicUsize, Ordering};

    static REFS: AtomicU32 = AtomicU32::new(1);
    static DESTROYS: AtomicUsize = AtomicUsize::new(0);
    static LOCKED: AtomicUsize = AtomicUsize::new(0);
    static COUNT: AtomicU32 = AtomicU32::new(0);
    static SLOT: AtomicU32 = AtomicU32::new(PFN_NONE);

    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_ref_inc(_: *mut c_void) {
        REFS.fetch_add(1, Ordering::Relaxed);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_ref_dec_last(_: *mut c_void) -> i32 {
        (REFS.fetch_sub(1, Ordering::AcqRel) != 1) as i32
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_destroy_rust(_: *mut c_void) {
        DESTROYS.fetch_add(1, Ordering::Relaxed);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_lock(_: *mut c_void, flags: *mut u64) -> i32 {
        assert_eq!(LOCKED.swap(1, Ordering::SeqCst), 0);
        unsafe { flags.write(0); }
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_unlock(_: *mut c_void, _: u64) {
        assert_eq!(LOCKED.swap(0, Ordering::SeqCst), 1);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_page_count_locked(_: *mut c_void) -> u32 {
        assert_eq!(LOCKED.load(Ordering::SeqCst), 1);
        COUNT.load(Ordering::Relaxed)
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_slot_locked(_: *mut c_void, index: u32) -> u32 {
        assert_eq!(LOCKED.load(Ordering::SeqCst), 1);
        assert_eq!(index, 0);
        SLOT.load(Ordering::Relaxed)
    }

    fn reset() {
        REFS.store(1, Ordering::Relaxed);
        DESTROYS.store(0, Ordering::Relaxed);
        LOCKED.store(0, Ordering::Relaxed);
        COUNT.store(1, Ordering::Relaxed);
        SLOT.store(17, Ordering::Relaxed);
    }
    fn obj() -> *mut c_void { 1usize as *mut c_void }

    #[test]
    fn ref_and_release_only_destroy_on_the_final_drop() {
        reset();
        unsafe { a20_vmo_ref_rust(obj()); }
        assert_eq!(REFS.load(Ordering::Relaxed), 2);
        unsafe { a20_vmo_release_rust(obj()); }
        assert_eq!(REFS.load(Ordering::Relaxed), 1);
        assert_eq!(DESTROYS.load(Ordering::Relaxed), 0);
        unsafe { a20_vmo_release_rust(obj()); }
        assert_eq!(REFS.load(Ordering::Relaxed), 0);
        assert_eq!(DESTROYS.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn null_ref_and_release_are_noops() {
        reset();
        unsafe { a20_vmo_ref_rust(ptr::null_mut()); a20_vmo_release_rust(ptr::null_mut()); }
        assert_eq!(REFS.load(Ordering::Relaxed), 1);
        assert_eq!(DESTROYS.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn peek_returns_canonical_page_without_materializing() {
        reset();
        assert_eq!(unsafe { a20_vmo_peek_page_rust(obj(), 0) }, 17);
        assert_eq!(unsafe { a20_vmo_peek_page_rust(obj(), 1) }, PFN_NONE);
        assert_eq!(LOCKED.load(Ordering::SeqCst), 0);
        SLOT.store(PFN_NONE, Ordering::Relaxed);
        assert_eq!(unsafe { a20_vmo_peek_page_rust(obj(), 0) }, PFN_NONE);
    }
}
