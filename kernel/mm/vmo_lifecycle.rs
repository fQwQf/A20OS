#![no_std]

//! Rust-owned VMO references, canonical lookup and atomic RAM snapshots.
//!
//! The reference-count atomics and VMO lock remain narrow C primitives. The
//! Rust entrypoints own null handling, the final-reference decision, destroy
//! dispatch, locked range validation and complete snapshot pin acquisition.

use core::ffi::c_void;
#[cfg(test)]
use core::ptr;

const PFN_NONE: u32 = u32::MAX;
#[cfg(test)]
const PIN_MAX_PAGES: u32 = 4096;

unsafe extern "C" {
    fn a20_vmo_lifecycle_ref_inc(vmo: *mut c_void);
    fn a20_vmo_lifecycle_ref_dec_last(vmo: *mut c_void) -> i32;
    fn a20_vmo_destroy_rust(vmo: *mut c_void);
    fn a20_vmo_lifecycle_lock(vmo: *mut c_void, flags: *mut u64) -> i32;
    fn a20_vmo_lifecycle_unlock(vmo: *mut c_void, flags: u64);
    fn a20_vmo_lifecycle_page_count_locked(vmo: *mut c_void) -> u32;
    fn a20_vmo_lifecycle_slot_locked(vmo: *mut c_void, index: u32) -> u32;
    fn a20_vmo_lifecycle_type_locked(vmo: *mut c_void) -> u32;
    fn a20_vmo_lifecycle_pin_limit() -> u32;
    fn a20_vmo_lifecycle_frame_valid(pfn: u32) -> i32;
    fn a20_vmo_lifecycle_pin_frames(pages: *const u32, count: u32);
}

/// Pin one atomic snapshot without faulting or leaving a partially filled output.
#[no_mangle]
pub unsafe extern "C" fn a20_vmo_pin_pages_rust(
    vmo: *mut c_void, first: u32, count: u32, out: *mut u32,
) -> i32 {
    if vmo.is_null() || out.is_null() || count == 0 { return -22; }
    if count > unsafe { a20_vmo_lifecycle_pin_limit() } { return -7; }
    let mut flags = 0u64;
    let ret = unsafe { a20_vmo_lifecycle_lock(vmo, &mut flags) };
    if ret != 0 { return ret; }
    // All exits after this closure release the same VMO lock.
    let ret = (|| {
        // Canonical RAM; contiguous allocations are split before publication.
        let kind = unsafe { a20_vmo_lifecycle_type_locked(vmo) };
        if !matches!(kind, 0 | 2 | 3 | 4) { return -95; }
        let total = unsafe { a20_vmo_lifecycle_page_count_locked(vmo) };
        if first > total || count > total - first { return -22; }
        for i in 0..count {
            let pfn = unsafe { a20_vmo_lifecycle_slot_locked(vmo, first + i) };
            if pfn == PFN_NONE { return -11; }
            if unsafe { a20_vmo_lifecycle_frame_valid(pfn) } == 0 { return -22; }
        }
        for i in 0..count {
            let pfn = unsafe { a20_vmo_lifecycle_slot_locked(vmo, first + i) };
            // SAFETY: count output slots are caller-owned, disjoint from the VMO.
            unsafe { out.add(i as usize).write(pfn); }
        }
        // The VMO still owns these canonical frames while PFA takes the pins.
        unsafe { a20_vmo_lifecycle_pin_frames(out, count); }
        0
    })();
    unsafe { a20_vmo_lifecycle_unlock(vmo, flags); }
    ret
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
    static KIND: AtomicU32 = AtomicU32::new(0);
    static HOLE: AtomicU32 = AtomicU32::new(PFN_NONE);
    static PINS: AtomicUsize = AtomicUsize::new(0);

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
        if index == HOLE.load(Ordering::Relaxed) { PFN_NONE }
        else { SLOT.load(Ordering::Relaxed).saturating_add(index) }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_type_locked(_: *mut c_void) -> u32 {
        assert_eq!(LOCKED.load(Ordering::SeqCst), 1);
        KIND.load(Ordering::Relaxed)
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_pin_limit() -> u32 { PIN_MAX_PAGES }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_frame_valid(pfn: u32) -> i32 {
        (pfn < 4096) as i32
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_lifecycle_pin_frames(pages: *const u32, count: u32) {
        assert_eq!(LOCKED.load(Ordering::SeqCst), 1);
        for i in 0..count { assert!(unsafe { pages.add(i as usize).read() } < 4096); }
        PINS.fetch_add(count as usize, Ordering::Relaxed);
    }

    fn reset() {
        REFS.store(1, Ordering::Relaxed);
        DESTROYS.store(0, Ordering::Relaxed);
        LOCKED.store(0, Ordering::Relaxed);
        COUNT.store(1, Ordering::Relaxed);
        SLOT.store(17, Ordering::Relaxed);
        KIND.store(0, Ordering::Relaxed);
        HOLE.store(PFN_NONE, Ordering::Relaxed);
        PINS.store(0, Ordering::Relaxed);
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

    #[test]
    fn pin_is_all_or_nothing_and_keeps_output_unchanged_on_missing_or_bad_pages() {
        reset(); COUNT.store(3, Ordering::Relaxed);
        let mut pages = [0xdead; 2];
        HOLE.store(2, Ordering::Relaxed);
        assert_eq!(unsafe { a20_vmo_pin_pages_rust(obj(), 1, 2, pages.as_mut_ptr()) }, -11);
        assert_eq!(pages, [0xdead; 2]); assert_eq!(PINS.load(Ordering::Relaxed), 0);
        HOLE.store(PFN_NONE, Ordering::Relaxed); SLOT.store(4095, Ordering::Relaxed);
        assert_eq!(unsafe { a20_vmo_pin_pages_rust(obj(), 0, 2, pages.as_mut_ptr()) }, -22);
        assert_eq!(pages, [0xdead; 2]); assert_eq!(PINS.load(Ordering::Relaxed), 0);
        assert_eq!(LOCKED.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn pin_range_supports_each_ram_kind_without_changing_vmo_refs() {
        for kind in [0, 2, 3, 4] {
            reset(); COUNT.store(3, Ordering::Relaxed); KIND.store(kind, Ordering::Relaxed);
            let mut pages = [0; 2];
            assert_eq!(unsafe { a20_vmo_pin_pages_rust(obj(), 1, 2, pages.as_mut_ptr()) }, 0);
            assert_eq!(pages, [18, 19]); assert_eq!(PINS.load(Ordering::Relaxed), 2);
            assert_eq!(REFS.load(Ordering::Relaxed), 1); assert_eq!(LOCKED.load(Ordering::Relaxed), 0);
        }
    }

    #[test]
    fn pin_rejects_physical_unknown_types_invalid_ranges_and_arguments() {
        reset(); let mut page = 0xdead;
        for kind in [1, 99] {
            KIND.store(kind, Ordering::Relaxed);
            assert_eq!(unsafe { a20_vmo_pin_pages_rust(obj(), 0, 1, &mut page) }, -95);
        }
        KIND.store(0, Ordering::Relaxed);
        for (first, count, error) in [(0, 0, -22), (1, 1, -22), (u32::MAX, 2, -22), (0, PIN_MAX_PAGES+1, -7)] {
            assert_eq!(unsafe { a20_vmo_pin_pages_rust(obj(), first, count, &mut page) }, error);
        }
        assert_eq!(unsafe { a20_vmo_pin_pages_rust(ptr::null_mut(), 0, 1, &mut page) }, -22);
        assert_eq!(unsafe { a20_vmo_pin_pages_rust(obj(), 0, 1, ptr::null_mut()) }, -22);
        assert_eq!(page, 0xdead); assert_eq!(PINS.load(Ordering::Relaxed), 0);
        assert_eq!(LOCKED.load(Ordering::Relaxed), 0);
    }

    #[test]
    fn pin_maximum_batch_includes_valid_physical_frame_zero() {
        reset(); COUNT.store(PIN_MAX_PAGES, Ordering::Relaxed); SLOT.store(0, Ordering::Relaxed);
        let mut pages = [PFN_NONE; PIN_MAX_PAGES as usize];
        assert_eq!(unsafe { a20_vmo_pin_pages_rust(obj(), 0, PIN_MAX_PAGES, pages.as_mut_ptr()) }, 0);
        assert_eq!(pages[0], 0); assert_eq!(pages[pages.len()-1], PIN_MAX_PAGES-1);
        assert_eq!(PINS.load(Ordering::Relaxed), PIN_MAX_PAGES as usize);
    }
}
