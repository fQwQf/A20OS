#![no_std]

//! Entry-state owner for Native handle slots.
//!
//! C owns the table lock, allocation bitmap/capacity, table references and
//! object refcounts. With this implementation selected, every entry mutation
//! and state-dependent lookup is performed here while C holds that lock.
//! These leaf routines never allocate, lock, call back into C, or release an
//! object. C retains detached object pointers and performs refcount/stat work
//! after the state transition.

use core::ffi::c_void;
use core::mem::{align_of, offset_of, size_of};
use core::ptr;

const FREE: u8 = 0;
const ACTIVE: u8 = 1;
const EXPIRED: u8 = 2;
const CLOSING: u8 = 3;
const TEMP_EXPIRY: u32 = 1;
const TEMP_OP_COUNT: u32 = 2;
const TEMP_AUTO_CLOSE: u32 = 4;
const OBJ_INVALID: u16 = 0;
const ERR_BAD_HANDLE: i64 = -5;
const ERR_ACCESS: i64 = -7;
const ERR_EXPIRED: i64 = -25;

type Rights = u64;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Entry {
    object: *mut c_void,
    object_type: u16,
    pad: u16,
    rights: Rights,
    expiry_tick: u64,
    remaining_ops: u32,
    temporal_flags: u32,
    security_label: u8,
    state: u8,
    pad2: [u8; 6],
}

const _: () = {
    assert!(size_of::<Entry>() == if usize::BITS == 64 { 48 } else { 40 });
    assert!(offset_of!(Entry, object) == 0);
    assert!(offset_of!(Entry, object_type) == size_of::<usize>());
    assert!(offset_of!(Entry, rights) == if usize::BITS == 64 { 16 } else { 8 });
    assert!(offset_of!(Entry, expiry_tick) == if usize::BITS == 64 { 24 } else { 16 });
    assert!(offset_of!(Entry, remaining_ops) == if usize::BITS == 64 { 32 } else { 24 });
    assert!(offset_of!(Entry, temporal_flags) == if usize::BITS == 64 { 36 } else { 28 });
    assert!(offset_of!(Entry, security_label) == if usize::BITS == 64 { 40 } else { 32 });
    #[cfg(target_arch = "arm")]
    assert!(align_of::<Entry>() == 8);
    #[cfg(not(target_arch = "arm"))]
    assert!(align_of::<Entry>() == if usize::BITS == 64 { 8 } else { 8 });
};

#[inline]
unsafe fn entry_mut<'a>(entry: *mut Entry) -> Option<&'a mut Entry> {
    if entry.is_null() || (entry as usize) % align_of::<Entry>() != 0 {
        None
    } else {
        // SAFETY: all exported callers hold the table lock and provide a live,
        // aligned entry pointer in the table's allocation.
        Some(unsafe { &mut *entry })
    }
}

#[inline]
fn clear(e: &mut Entry) {
    e.object = ptr::null_mut();
    e.object_type = OBJ_INVALID;
    e.rights = 0;
    e.expiry_tick = 0;
    e.remaining_ops = 0;
    e.temporal_flags = 0;
    e.security_label = 0;
    e.state = FREE;
}

#[no_mangle]
pub extern "C" fn a20_rust_handle_slot_quota_allows(
    used: u32, requested: u32, quota: u32,
) -> i32 {
    i32::from(requested <= quota && used <= quota - requested)
}

#[no_mangle]
/// # Safety
/// `entry` must identify a live, aligned table slot while its table lock is
/// held. `object` must be non-null and ownership of one reference is transferred
/// by the caller; this routine does not acquire or release that reference.
pub unsafe extern "C" fn a20_rust_handle_slot_install(
    entry: *mut Entry, object: *mut c_void, object_type: u16, rights: Rights,
    expiry_tick: u64, remaining_ops: u32, temporal_flags: u32,
    security_label: u8,
) -> i32 {
    // SAFETY: caller must satisfy the lock and pointer contract in handle_slot.h.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return 0 };
    if e.state != FREE || !e.object.is_null() || object.is_null() { return 0; }
    e.object = object;
    e.object_type = object_type;
    e.rights = rights;
    e.expiry_tick = expiry_tick;
    e.remaining_ops = remaining_ops;
    e.temporal_flags = temporal_flags;
    e.security_label = security_label;
    e.state = ACTIVE;
    1
}

#[no_mangle]
/// # Safety
/// `entry` must identify a live, aligned table slot while its table lock is held.
pub unsafe extern "C" fn a20_rust_handle_slot_reserve(entry: *mut Entry) -> i32 {
    // SAFETY: caller must satisfy the lock and pointer contract in handle_slot.h.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return 0 };
    if e.state != FREE || !e.object.is_null() { return 0; }
    e.state = CLOSING;
    1
}

#[no_mangle]
/// # Safety
/// `entry` must identify a live, aligned table slot while its table lock is held.
pub unsafe extern "C" fn a20_rust_handle_slot_abort(entry: *mut Entry) -> i32 {
    // SAFETY: caller must satisfy the lock and pointer contract in handle_slot.h.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return 0 };
    if e.state != CLOSING || !e.object.is_null() { return 0; }
    clear(e);
    1
}

#[no_mangle]
/// # Safety
/// `entry` must identify a live, aligned table slot while its table lock is
/// held. `object` must be non-null and ownership of one reference is transferred
/// by the caller; this routine does not acquire or release that reference.
pub unsafe extern "C" fn a20_rust_handle_slot_commit(
    entry: *mut Entry, object: *mut c_void, object_type: u16, rights: Rights,
    expiry_tick: u64, remaining_ops: u32, temporal_flags: u32,
    security_label: u8,
) -> i32 {
    // SAFETY: caller must satisfy the lock and pointer contract in handle_slot.h.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return 0 };
    if e.state != CLOSING || !e.object.is_null() || object.is_null() { return 0; }
    e.object = object;
    e.object_type = object_type;
    e.rights = rights;
    e.expiry_tick = expiry_tick;
    e.remaining_ops = remaining_ops;
    e.temporal_flags = temporal_flags;
    e.security_label = security_label;
    e.state = ACTIVE;
    1
}

#[no_mangle]
/// # Safety
/// `entry` must be live and locked. `object` and `object_type` must be valid,
/// writable, mutually non-overlapping outputs that do not alias `entry`.
pub unsafe extern "C" fn a20_rust_handle_slot_remove(
    entry: *mut Entry, object: *mut *mut c_void, object_type: *mut u16,
) -> i32 {
    // SAFETY: caller must satisfy the lock and provide valid out pointers.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return 0 };
    if object.is_null() || object_type.is_null() || e.object.is_null()
        || e.state == FREE || e.state == CLOSING { return 0; }
    unsafe { *object = e.object; *object_type = e.object_type; }
    clear(e);
    1
}

#[no_mangle]
/// # Safety
/// `entry` must be live and locked. `out` must be a valid aligned writable
/// entry-sized region that does not overlap `entry`.
pub unsafe extern "C" fn a20_rust_handle_slot_lookup(
    entry: *mut Entry, expected_type: u16, required_rights: Rights,
    now: u64, out: *mut Entry,
) -> i64 {
    // SAFETY: caller must satisfy the lock and provide a valid output entry.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return ERR_BAD_HANDLE };
    if out.is_null() { return ERR_BAD_HANDLE; }
    if e.object.is_null() || e.state == FREE || e.state == CLOSING { return ERR_BAD_HANDLE; }
    if e.state == EXPIRED { return ERR_EXPIRED; }
    if expected_type != OBJ_INVALID && e.object_type != expected_type { return ERR_BAD_HANDLE; }
    let mut effective = e.rights;
    if e.expiry_tick > 0 && now >= e.expiry_tick { effective = 0; }
    if e.temporal_flags & TEMP_OP_COUNT != 0 && e.remaining_ops == 0 { effective = 0; }
    if effective & required_rights != required_rights { return ERR_ACCESS; }
    if e.temporal_flags & TEMP_OP_COUNT != 0 && e.remaining_ops > 0 { e.remaining_ops -= 1; }
    let mut snapshot = *e;
    snapshot.rights = effective;
    unsafe { *out = snapshot; }
    0
}

#[no_mangle]
/// # Safety
/// `entry` must identify a live, aligned table slot while its table lock is held.
pub unsafe extern "C" fn a20_rust_handle_slot_set_temporal(
    entry: *mut Entry, expiry_tick: u64, remaining_ops: u32, temporal_flags: u32,
) -> i32 {
    // SAFETY: caller must satisfy the lock and pointer contract in handle_slot.h.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return -12 };
    if temporal_flags & !(TEMP_EXPIRY | TEMP_OP_COUNT | TEMP_AUTO_CLOSE) != 0 { return -12; }
    if e.object.is_null() || e.state != ACTIVE { return -5; }
    if e.temporal_flags & !temporal_flags != 0 { return -7; }
    if e.temporal_flags & TEMP_EXPIRY != 0 && e.expiry_tick > 0
        && (expiry_tick == 0 || expiry_tick > e.expiry_tick) { return -7; }
    if e.temporal_flags & TEMP_OP_COUNT != 0 && remaining_ops > e.remaining_ops { return -7; }
    if temporal_flags & TEMP_EXPIRY != 0 { e.expiry_tick = expiry_tick; }
    if temporal_flags & TEMP_OP_COUNT != 0 { e.remaining_ops = remaining_ops; }
    e.temporal_flags = temporal_flags;
    0
}

#[no_mangle]
/// # Safety
/// `entry` must be live and locked. The three outputs must be valid writable
/// pointers, pairwise non-overlapping, and must not overlap `entry`.
pub unsafe extern "C" fn a20_rust_handle_slot_get_temporal(
    entry: *const Entry, expiry_tick: *mut u64, remaining_ops: *mut u32,
    temporal_flags: *mut u32,
) -> i32 {
    if entry.is_null() || expiry_tick.is_null() || remaining_ops.is_null() || temporal_flags.is_null()
        || (entry as usize) % align_of::<Entry>() != 0 { return -5; }
    // SAFETY: caller holds table lock and provides valid output pointers.
    let e = unsafe { &*entry };
    if e.object.is_null() || e.state == FREE || e.state == CLOSING { return -5; }
    unsafe { *expiry_tick = e.expiry_tick; *remaining_ops = e.remaining_ops; *temporal_flags = e.temporal_flags; }
    0
}

#[no_mangle]
/// # Safety
/// `entry` must identify a live, aligned table slot while its table lock is held.
pub unsafe extern "C" fn a20_rust_handle_slot_set_label(entry: *mut Entry, label: u8) -> i32 {
    // SAFETY: caller must satisfy the lock and pointer contract in handle_slot.h.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return -12 };
    if label > 2 { return -12; }
    if e.object.is_null() || e.state != ACTIVE { return -5; }
    if label < e.security_label { return -7; }
    e.security_label = label;
    0
}

#[no_mangle]
/// # Safety
/// `entry` must be live and locked. The outputs must be valid writable,
/// non-overlapping locations disjoint from `entry`.
pub unsafe extern "C" fn a20_rust_handle_slot_expire(
    entry: *mut Entry, now: u64, object: *mut *mut c_void, object_type: *mut u16,
) -> i32 {
    // SAFETY: caller holds lock and provides valid out pointers.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return 0 };
    if object.is_null() || object_type.is_null() || e.object.is_null() || e.state != ACTIVE { return 0; }
    let expired = (e.temporal_flags & TEMP_EXPIRY != 0 && e.expiry_tick > 0 && now >= e.expiry_tick)
        || (e.temporal_flags & TEMP_OP_COUNT != 0 && e.remaining_ops == 0);
    if !expired { return 0; }
    if e.temporal_flags & TEMP_AUTO_CLOSE != 0 {
        unsafe { *object = e.object; *object_type = e.object_type; }
        clear(e);
        return 1;
    }
    e.state = EXPIRED;
    e.rights = 0;
    0
}

#[no_mangle]
/// # Safety
/// `entry` must be live during final table teardown. The outputs must be valid
/// writable, non-overlapping locations disjoint from `entry`.
pub unsafe extern "C" fn a20_rust_handle_slot_destroy(
    entry: *mut Entry, object: *mut *mut c_void, object_type: *mut u16,
) -> i32 {
    // SAFETY: table is finalizing; caller provides valid out pointers.
    let Some(e) = (unsafe { entry_mut(entry) }) else { return 0 };
    if object.is_null() || object_type.is_null() || e.object.is_null() || e.state == FREE { return 0; }
    unsafe { *object = e.object; *object_type = e.object_type; }
    clear(e);
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    fn empty() -> Entry {
        Entry {
            object: ptr::null_mut(), object_type: OBJ_INVALID, pad: 0, rights: 0,
            expiry_tick: 0, remaining_ops: 0, temporal_flags: 0,
            security_label: 0, state: FREE, pad2: [0; 6],
        }
    }

    #[test]
    fn quota_boundary_and_overflow() {
        assert_eq!(a20_rust_handle_slot_quota_allows(4095, 1, 4096), 1);
        assert_eq!(a20_rust_handle_slot_quota_allows(4096, 1, 4096), 0);
        assert_eq!(a20_rust_handle_slot_quota_allows(0, u32::MAX, 4096), 0);
        assert_eq!(a20_rust_handle_slot_quota_allows(9, 0, 8), 0);
    }

    #[test]
    fn reserve_commit_lookup_expire_remove_lifecycle() {
        let mut e = empty();
        let object = 0x1000usize as *mut c_void;
        let mut out = empty();
        let mut detached = ptr::null_mut();
        let mut detached_type = OBJ_INVALID;
        unsafe {
            assert_eq!(a20_rust_handle_slot_reserve(&mut e), 1);
            assert_eq!(e.state, CLOSING);
            assert_eq!(a20_rust_handle_slot_commit(&mut e, object, 4, 7, 100, 2,
                                                    TEMP_EXPIRY | TEMP_OP_COUNT,
                                                    1), 1);
            assert_eq!(a20_rust_handle_slot_commit(&mut e, object, 4, 7, 100, 2,
                                                    TEMP_EXPIRY | TEMP_OP_COUNT,
                                                    1), 0);
            assert_eq!(a20_rust_handle_slot_lookup(&mut e, 4, 1, 10, &mut out), 0);
            assert_eq!(e.remaining_ops, 1);
            assert_eq!(out.rights, 7);
            assert_eq!(a20_rust_handle_slot_expire(&mut e, 100, &mut detached,
                                                    &mut detached_type), 0);
            assert_eq!(e.state, EXPIRED);
            assert_eq!(a20_rust_handle_slot_lookup(&mut e, 4, 1, 100, &mut out), ERR_EXPIRED);
            assert_eq!(a20_rust_handle_slot_remove(&mut e, &mut detached,
                                                   &mut detached_type), 1);
            assert_eq!(detached, object);
            assert_eq!(detached_type, 4);
            assert_eq!(e.state, FREE);
            assert!(e.object.is_null());
        }
    }

    #[test]
    fn abort_and_auto_close_return_exactly_one_reference() {
        let mut e = empty();
        let object = 0x2000usize as *mut c_void;
        let mut detached = ptr::null_mut();
        let mut detached_type = OBJ_INVALID;
        unsafe {
            assert_eq!(a20_rust_handle_slot_reserve(&mut e), 1);
            assert_eq!(a20_rust_handle_slot_abort(&mut e), 1);
            assert_eq!(a20_rust_handle_slot_abort(&mut e), 0);
            assert_eq!(a20_rust_handle_slot_install(&mut e, object, 6, 3, 0, 0,
                                                     TEMP_OP_COUNT | TEMP_AUTO_CLOSE,
                                                     0), 1);
            assert_eq!(a20_rust_handle_slot_expire(&mut e, 0, &mut detached,
                                                    &mut detached_type), 1);
            assert_eq!(detached, object);
            assert_eq!(detached_type, 6);
            assert_eq!(e.state, FREE);
            assert_eq!(a20_rust_handle_slot_expire(&mut e, 0, &mut detached,
                                                    &mut detached_type), 0);
        }
    }
}
