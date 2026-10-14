#![no_std]

//! Opaque owner for Native handle-table storage and slot allocation metadata.
//!
//! The C shell owns the IRQ-safe lock, table lifetime reference, registry,
//! object references and callbacks. This module owns the entry/bitmap buffers,
//! capacity, quota accounting, allocation hint and reserved/live slot count.
//! Every mutating entry point requires the caller to hold the table lock.
//! Allocator callbacks are invoked synchronously under that same lock, matching
//! the existing `ht_grow()` call sites; they must not sleep or re-enter a table.

use core::ffi::c_void;
use core::mem::{align_of, size_of};
use core::ptr;
#[path = "handle_entry.rs"]
mod handle_entry;
use handle_entry::Entry;

const ERR_INVALID: i32 = -12; // -A20_ERR_INVALID_ARGUMENT
const ERR_NO_SPACE: i32 = -13; // -A20_ERR_NO_SPACE
const ERR_BUSY: i32 = -9; // -A20_ERR_BUSY

pub type AllocFn = unsafe extern "C" fn(usize) -> *mut u8;
pub type FreeFn = unsafe extern "C" fn(*mut u8);

extern "C" {
    fn a20_rust_handle_slot_install(
        entry: *mut c_void, object: *mut c_void, object_type: u16, rights: u64,
        expiry_tick: u64, remaining_ops: u32, temporal_flags: u32,
        security_label: u8,
    ) -> i32;
    fn a20_rust_handle_slot_reserve(entry: *mut c_void) -> i32;
    fn a20_rust_handle_slot_abort(entry: *mut c_void) -> i32;
    fn a20_rust_handle_slot_commit(
        entry: *mut c_void, object: *mut c_void, object_type: u16, rights: u64,
        expiry_tick: u64, remaining_ops: u32, temporal_flags: u32,
        security_label: u8,
    ) -> i32;
    fn a20_rust_handle_slot_remove(
        entry: *mut c_void, object: *mut *mut c_void, object_type: *mut u16,
    ) -> i32;
    fn a20_rust_handle_slot_destroy(
        entry: *mut c_void, object: *mut *mut c_void, object_type: *mut u16,
    ) -> i32;
    fn a20_rust_handle_slot_lookup(
        entry: *mut c_void, expected_type: u16, required_rights: u64,
        now: u64, out: *mut c_void,
    ) -> i64;
    fn a20_rust_handle_slot_set_temporal(
        entry: *mut c_void, expiry_tick: u64, remaining_ops: u32,
        temporal_flags: u32,
    ) -> i32;
    fn a20_rust_handle_slot_get_temporal(
        entry: *const c_void, expiry_tick: *mut u64, remaining_ops: *mut u32,
        temporal_flags: *mut u32,
    ) -> i32;
    fn a20_rust_handle_slot_set_label(entry: *mut c_void, label: u8) -> i32;
    fn a20_rust_handle_slot_expire(
        entry: *mut c_void, now: u64, object: *mut *mut c_void,
        object_type: *mut u16,
    ) -> i32;
}

/// C sees only this zero-sized opaque declaration. Its storage is allocated
/// with the size/alignment returned by the accessors below and initialized by
/// `a20_rust_handle_table_state_init`.
#[repr(C)]
pub struct OpaqueHandleTableState {
    _opaque: [u8; 0],
}

/// Private implementation. No C code may read or write these fields.
#[repr(C)]
struct State {
    entries: *mut u8,
    entry_size: usize,
    entry_align: usize,
    capacity: u32,
    used: u32,
    free_hint: u32,
    bitmap: *mut u64,
    bitmap_words: u32,
    max_capacity: u32,
    max_handles: u32,
    alloc: Option<AllocFn>,
    free: Option<FreeFn>,
    initialized: u32,
}

const _: () = {
    assert!(align_of::<State>() >= align_of::<usize>());
    assert!(size_of::<State>() > 0);
};

#[inline]
unsafe fn state_mut<'a>(state: *mut OpaqueHandleTableState) -> Option<&'a mut State> {
    if state.is_null() || (state as usize) % align_of::<State>() != 0 {
        return None;
    }
    // SAFETY: the exported caller guarantees initialized, exclusive access
    // under the table lock, or exclusive destruction after refcount reaches 0.
    let value = unsafe { &mut *(state.cast::<State>()) };
    (value.initialized == 1).then_some(value)
}

#[inline]
fn words_for(capacity: u32) -> Option<u32> {
    capacity.checked_add(63).map(|n| n / 64)
}

#[inline]
fn bit_is_set(state: &State, slot: u32) -> bool {
    let word = unsafe { *state.bitmap.add((slot / 64) as usize) };
    word & (1u64 << (slot % 64)) != 0
}

#[inline]
fn set_bit(state: &mut State, slot: u32) {
    unsafe { *state.bitmap.add((slot / 64) as usize) |= 1u64 << (slot % 64) };
}

#[inline]
fn clear_bit(state: &mut State, slot: u32) {
    unsafe { *state.bitmap.add((slot / 64) as usize) &= !(1u64 << (slot % 64)) };
}

#[inline]
fn range_overlaps(a: *const u8, a_len: usize, b: *const u8, b_len: usize) -> bool {
    let a_start = a as usize;
    let b_start = b as usize;
    let Some(a_end) = a_start.checked_add(a_len) else { return true };
    let Some(b_end) = b_start.checked_add(b_len) else { return true };
    a_start < b_end && b_start < a_end
}

/// Grow both backing buffers transactionally. Neither state pointer changes
/// until every allocation succeeds and the copies are complete.
unsafe fn grow(state: &mut State, min_capacity: u32) -> Result<(), i32> {
    if min_capacity > state.max_capacity || state.capacity >= state.max_capacity {
        return Err(ERR_NO_SPACE);
    }
    let doubled = state.capacity.checked_mul(2).ok_or(ERR_NO_SPACE)?;
    let new_capacity = doubled.max(min_capacity).min(state.max_capacity);
    if new_capacity < min_capacity || new_capacity <= state.capacity {
        return Err(ERR_NO_SPACE);
    }
    let new_words = words_for(new_capacity).ok_or(ERR_NO_SPACE)?;
    let entry_bytes = (new_capacity as usize)
        .checked_mul(state.entry_size)
        .ok_or(ERR_NO_SPACE)?;
    let bitmap_bytes = (new_words as usize)
        .checked_mul(size_of::<u64>())
        .ok_or(ERR_NO_SPACE)?;
    let alloc = state.alloc.ok_or(ERR_INVALID)?;
    let free = state.free.ok_or(ERR_INVALID)?;

    // SAFETY: these callbacks are supplied by C and have the non-sleeping,
    // non-reentrant allocator contract documented on the exported init API.
    let old_entry_bytes = (state.capacity as usize)
        .checked_mul(state.entry_size)
        .ok_or(ERR_NO_SPACE)?;
    let old_bitmap_bytes = (state.bitmap_words as usize)
        .checked_mul(size_of::<u64>())
        .ok_or(ERR_NO_SPACE)?;
    let new_entries = unsafe { alloc(entry_bytes) };
    if new_entries.is_null() {
        return Err(ERR_NO_SPACE);
    }
    let new_bitmap_bytes = unsafe { alloc(bitmap_bytes) };
    let new_bitmap = new_bitmap_bytes.cast::<u64>();
    if new_bitmap.is_null() {
        unsafe { free(new_entries) };
        return Err(ERR_NO_SPACE);
    }

    // Allocator callback's unsafe contract guarantees each successful return
    // is a fresh, exclusive allocation disjoint from state and old buffers.
    if (new_entries as usize) & (state.entry_align - 1) != 0
        || (new_bitmap as usize) & (align_of::<u64>() - 1) != 0 {
        unsafe { free(new_entries); free(new_bitmap_bytes); }
        return Err(ERR_NO_SPACE);
    }

    unsafe {
        ptr::write_bytes(new_entries, 0, entry_bytes);
        ptr::copy_nonoverlapping(
            state.entries,
            new_entries,
            old_entry_bytes,
        );
        ptr::write_bytes(new_bitmap, 0, bitmap_bytes / size_of::<u64>());
        ptr::copy_nonoverlapping(
            state.bitmap.cast::<u8>(),
            new_bitmap_bytes,
            old_bitmap_bytes,
        );
    }

    let old_entries = state.entries;
    let old_bitmap = state.bitmap.cast::<u8>();
    state.entries = new_entries;
    state.bitmap = new_bitmap;
    state.capacity = new_capacity;
    state.bitmap_words = new_words;
    unsafe {
        free(old_entries);
        free(old_bitmap);
    }
    Ok(())
}

/// Return the required storage size for an opaque state.
#[no_mangle]
pub extern "C" fn a20_rust_handle_table_state_size() -> usize {
    size_of::<State>()
}

/// Return the required storage alignment for an opaque state.
#[no_mangle]
pub extern "C" fn a20_rust_handle_table_state_align() -> usize {
    align_of::<State>()
}

/// Initialize opaque table state around C-allocated initial buffers.
///
/// # Safety
/// `storage` must point to suitably aligned, writable storage of at least
/// `state_size()` bytes. `storage`, `entries`, and `bitmap` must be pairwise
/// disjoint writable allocations sized for their supplied capacities. Entries
/// must satisfy `entry_align` and `entry_size` must be a multiple of it; bitmap
/// must satisfy u64 alignment. `alloc` must return fresh, exclusive,
/// correctly aligned buffers disjoint from state and every live buffer; it
/// must not return an old live pointer. `alloc` and `free` must be a matching
/// non-sleeping allocator pair, safe while the caller holds the
/// IRQ-saving table lock, and must not re-enter this table. No other access to
/// the state or buffers may occur until init returns.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_handle_table_state_init(
    storage: *mut OpaqueHandleTableState,
    entries: *mut u8,
    entry_size: usize,
    entry_align: usize,
    capacity: u32,
    bitmap: *mut u64,
    bitmap_words: u32,
    max_capacity: u32,
    max_handles: u32,
    alloc: Option<AllocFn>,
    free: Option<FreeFn>,
) -> i32 {
    if storage.is_null()
        || (storage as usize) % align_of::<State>() != 0
        || entries.is_null()
        || bitmap.is_null()
        || entry_size == 0
        || entry_align == 0
        || !entry_align.is_power_of_two()
        || entry_size != size_of::<Entry>()
        || entry_align != align_of::<Entry>()
        || entry_size & (entry_align - 1) != 0
        || (entries as usize) & (entry_align - 1) != 0
        || (bitmap as usize) & (align_of::<u64>() - 1) != 0
        || capacity == 0
        || capacity > max_capacity
        || max_handles > max_capacity
        || bitmap_words != words_for(capacity).unwrap_or(u32::MAX)
        || alloc.is_none()
        || free.is_none()
    {
        return ERR_INVALID;
    }
    let Some(entry_bytes) = (capacity as usize).checked_mul(entry_size) else {
        return ERR_INVALID;
    };
    let Some(bitmap_bytes) = (bitmap_words as usize).checked_mul(size_of::<u64>()) else {
        return ERR_INVALID;
    };
    if range_overlaps(storage.cast::<u8>(), size_of::<State>(), entries, entry_bytes)
        || range_overlaps(storage.cast::<u8>(), size_of::<State>(), bitmap.cast::<u8>(), bitmap_bytes)
        || range_overlaps(entries, entry_bytes, bitmap.cast::<u8>(), bitmap_bytes)
    {
        return ERR_INVALID;
    }
    // Caller supplies distinct regions; initialize both while state is not yet
    // published. Zeroing here keeps bitmap ownership on the Rust side.
    unsafe {
        ptr::write_bytes(entries, 0, entry_bytes);
        ptr::write_bytes(bitmap, 0, bitmap_bytes / size_of::<u64>());
        storage.cast::<State>().write(State {
            entries,
            entry_size,
            entry_align,
            capacity,
            used: 0,
            free_hint: 0,
            bitmap,
            bitmap_words,
            max_capacity,
            max_handles,
            alloc,
            free,
            initialized: 1,
        });
    }
    0
}

/// Atomically reserve `count` free slots. Reservations count toward the quota
/// until released or committed; entry-state transition remains a separate
/// Rust call made under the same C-held lock in the initial integration.
///
/// # Safety
/// `state` must be initialized and exclusively protected by its table lock.
/// `out_slots` must address `count` writable u32s and must not overlap state,
/// entries, bitmap, or itself in a way that creates aliased elements.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_handle_table_reserve_many(
    state: *mut OpaqueHandleTableState,
    count: u32,
    out_slots: *mut u32,
) -> i32 {
    if count == 0 {
        return 0;
    }
    if out_slots.is_null() {
        return ERR_INVALID;
    }
    // SAFETY: caller fulfills the opaque-state and output-pointer contract.
    let Some(state) = (unsafe { state_mut(state) }) else { return ERR_INVALID };
    if count > state.max_handles || state.used > state.max_handles - count {
        return ERR_NO_SPACE;
    }
    if state.used > state.capacity {
        return ERR_INVALID;
    }
    if state.capacity - state.used < count {
        // Match C ht_grow(): one attempt doubles capacity. A single large
        // reservation may therefore fail after one growth, leaving the grown
        // capacity available to later smaller calls.
        let grow_to = state.capacity.checked_add(1).ok_or(ERR_NO_SPACE);
        let Ok(grow_to) = grow_to else { return ERR_NO_SPACE };
        // SAFETY: allocator callbacks are part of initialized state and caller
        // holds the same lock required by the callbacks' contract.
        if unsafe { grow(state, grow_to) }.is_err() {
            return ERR_NO_SPACE;
        }
        if state.capacity - state.used < count { return ERR_NO_SPACE; }
    }

    let original_hint = state.free_hint;
    let mut found = 0u32;
    for _ in 0..count {
        if let Some(slot) = allocate_one(state) {
            unsafe { out_slots.add(found as usize).write(slot) };
            found += 1;
        } else {
            break;
        }
    }
    if found != count {
        for i in 0..found {
            let slot = unsafe { out_slots.add(i as usize).read() };
            clear_bit(state, slot);
        }
        state.free_hint = original_hint;
        return ERR_NO_SPACE;
    }
    let mut transitioned = 0u32;
    for i in 0..count {
        let slot = unsafe { out_slots.add(i as usize).read() };
        // SAFETY: the slot pointer is valid and stable under the table lock.
        if unsafe { a20_rust_handle_slot_reserve(entry_ptr(state, slot)) } != 1 {
            for j in 0..transitioned {
                let previous = unsafe { out_slots.add(j as usize).read() };
                unsafe { a20_rust_handle_slot_abort(entry_ptr(state, previous)); }
            }
            for j in 0..count {
                let reserved = unsafe { out_slots.add(j as usize).read() };
                clear_bit(state, reserved);
            }
            state.free_hint = original_hint;
            return ERR_INVALID;
        }
        transitioned += 1;
    }
    state.used += count;
    0
}

/// Match `ht_alloc_slot`: start at the word containing free_hint, select that
/// word's lowest free bit, then scan later words. It only wraps after a free
/// operation lowers free_hint to an earlier word.
fn allocate_one(state: &mut State) -> Option<u32> {
    let first_word = state.free_hint / 64;
    for word_index in first_word..state.bitmap_words {
        let base = word_index * 64;
        let mut free_bits = !unsafe { *state.bitmap.add(word_index as usize) };
        while free_bits != 0 {
            let bit = free_bits.trailing_zeros();
            let slot = base.checked_add(bit)?;
            if slot < state.capacity {
                set_bit(state, slot);
                state.free_hint = slot.saturating_add(1);
                return Some(slot);
            }
            free_bits &= free_bits - 1;
        }
    }
    None
}

#[inline]
fn entry_ptr(state: &State, slot: u32) -> *mut c_void {
    if slot >= state.capacity { return ptr::null_mut(); }
    unsafe { state.entries.add((slot as usize) * state.entry_size).cast() }
}

#[no_mangle]
/// Install object into a newly allocated slot in one state-owner operation.
///
/// # Safety
/// Caller must hold the table lock. `object` is non-null and transfers one
/// owned object reference on success. `state`/entry buffers are live.
pub unsafe extern "C" fn a20_rust_handle_table_install(
    opaque: *mut OpaqueHandleTableState,
    object: *mut c_void,
    object_type: u16,
    rights: u64,
    expiry_tick: u64,
    remaining_ops: u32,
    temporal_flags: u32,
    security_label: u8,
) -> i64 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return ERR_INVALID as i64 };
    if object.is_null() { return ERR_INVALID as i64; }
    if state.used >= state.max_handles { return ERR_NO_SPACE as i64; }
    if state.used >= state.capacity {
        let needed = match state.used.checked_add(1) { Some(n) => n, None => return ERR_NO_SPACE as i64 };
        if unsafe { grow(state, needed) }.is_err() { return ERR_NO_SPACE as i64; }
    }
    let old_hint = state.free_hint;
    let Some(slot) = allocate_one(state) else { return ERR_NO_SPACE as i64 };
    // SAFETY: state lock and slot pointer contract are inherited from caller.
    if unsafe { a20_rust_handle_slot_install(entry_ptr(state, slot), object,
            object_type, rights, expiry_tick, remaining_ops, temporal_flags,
            security_label) } != 1 {
        clear_bit(state, slot);
        state.free_hint = old_hint;
        return -5;
    }
    state.used += 1;
    slot as i64
}

#[no_mangle]
/// Install into a caller-selected slot, including reservation and Entry
/// transition. This matches Native fd-table installation semantics.
///
/// # Safety
/// Caller holds table lock; object is non-null and transfers one owned
/// reference on success.
pub unsafe extern "C" fn a20_rust_handle_table_install_at(
    opaque: *mut OpaqueHandleTableState, slot: u32, object: *mut c_void,
    object_type: u16, rights: u64, expiry_tick: u64, remaining_ops: u32,
    temporal_flags: u32, security_label: u8,
) -> i64 {
    if object.is_null() { return ERR_INVALID as i64; }
    let status = unsafe { reserve_at(opaque, slot) };
    if status != 0 { return status as i64; }
    let Some(state) = (unsafe { state_mut(opaque) }) else { return ERR_INVALID as i64 };
    // reserve_at changes FREE -> CLOSING, so commit (not install) performs
    // the matching CLOSING -> ACTIVE transition.
    if unsafe { a20_rust_handle_slot_commit(entry_ptr(state, slot), object,
            object_type, rights, expiry_tick, remaining_ops, temporal_flags,
            security_label) } == 1 {
        return slot as i64;
    }
    unsafe { abort_slot(opaque, slot); }
    -5
}

#[no_mangle]
/// Commit object ownership into a previously reserved slot.
///
/// # Safety
/// Caller must hold the table lock. The slot must be reserved and object must
/// be a non-null owned reference transferred on success.
pub unsafe extern "C" fn a20_rust_handle_table_commit(
    opaque: *mut OpaqueHandleTableState, slot: u32, object: *mut c_void,
    object_type: u16, rights: u64, expiry_tick: u64, remaining_ops: u32,
    temporal_flags: u32, security_label: u8,
) -> i32 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return ERR_INVALID };
    if slot >= state.capacity || !bit_is_set(state, slot) || object.is_null() {
        return ERR_INVALID;
    }
    // SAFETY: caller holds table lock and owns object transfer on success.
    if unsafe { a20_rust_handle_slot_commit(entry_ptr(state, slot), object,
        object_type, rights, expiry_tick, remaining_ops, temporal_flags,
        security_label) } == 1 {
        0
    } else {
        ERR_INVALID
    }
}

/// Abort a list of reservations, updating Entry state, bitmap, quota count and
/// allocation hint together. Returns the number of slots actually aborted.
///
/// # Safety
/// Caller must hold table lock and `slots` must address `count` readable u32s.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_handle_table_abort_many(
    opaque: *mut OpaqueHandleTableState, slots: *const u32, count: u32,
) -> u32 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return 0 };
    if count != 0 && slots.is_null() { return 0; }
    let mut aborted = 0;
    for i in 0..count {
        let slot = unsafe { slots.add(i as usize).read() };
        if slot < state.capacity && bit_is_set(state, slot)
            && unsafe { a20_rust_handle_slot_abort(entry_ptr(state, slot)) } == 1 {
            clear_bit(state, slot);
            state.used -= 1;
            state.free_hint = state.free_hint.min(slot);
            aborted += 1;
        }
    }
    aborted
}

#[no_mangle]
/// Remove a live/expired entry and reconcile table accounting. C releases the
/// returned object reference after dropping the table lock.
///
/// # Safety
/// Caller must hold table lock. The outputs must be valid, non-overlapping,
/// and disjoint from state, entry buffers, and slot list memory.
pub unsafe extern "C" fn a20_rust_handle_table_remove(
    opaque: *mut OpaqueHandleTableState, slot: u32,
    object: *mut *mut c_void, object_type: *mut u16,
) -> i32 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return 0 };
    if slot >= state.capacity || !bit_is_set(state, slot) { return 0; }
    // SAFETY: the caller supplies disjoint valid output locations.
    if unsafe { a20_rust_handle_slot_remove(entry_ptr(state, slot), object, object_type) } != 1 {
        return 0;
    }
    clear_bit(state, slot);
    state.used -= 1;
    state.free_hint = state.free_hint.min(slot);
    1
}

#[no_mangle]
/// Permission/type/temporal lookup with operation-count consumption in the
/// same locked state transition. C performs the object ref callback before
/// dropping its table lock when requested.
///
/// # Safety
/// Caller must hold table lock; `out` is a valid, aligned, disjoint Entry-sized
/// writable buffer.
pub unsafe extern "C" fn a20_rust_handle_table_lookup(
    opaque: *mut OpaqueHandleTableState, slot: u32, expected_type: u16,
    required_rights: u64, now: u64, out: *mut c_void,
) -> i64 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return -5 };
    if slot >= state.capacity || !bit_is_set(state, slot) { return -5; }
    // SAFETY: caller supplies the matching Entry snapshot buffer.
    unsafe { a20_rust_handle_slot_lookup(entry_ptr(state, slot), expected_type,
        required_rights, now, out) }
}

#[no_mangle]
/// Set temporal constraints with the Entry transition protected by the same
/// table lock as the table state.
///
/// # Safety
/// Caller holds table lock and state is live.
pub unsafe extern "C" fn a20_rust_handle_table_set_temporal(
    opaque: *mut OpaqueHandleTableState, slot: u32, expiry_tick: u64,
    remaining_ops: u32, temporal_flags: u32,
) -> i32 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return ERR_INVALID };
    if slot >= state.capacity || !bit_is_set(state, slot) { return -5; }
    unsafe { a20_rust_handle_slot_set_temporal(entry_ptr(state, slot), expiry_tick,
        remaining_ops, temporal_flags) }
}

#[no_mangle]
/// Read temporal metadata while the table lock is held.
///
/// # Safety
/// Caller holds table lock; outputs are valid, mutually disjoint pointers that
/// do not overlap state or the selected Entry.
pub unsafe extern "C" fn a20_rust_handle_table_get_temporal(
    opaque: *mut OpaqueHandleTableState, slot: u32, expiry_tick: *mut u64,
    remaining_ops: *mut u32, temporal_flags: *mut u32,
) -> i32 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return ERR_INVALID };
    if slot >= state.capacity || !bit_is_set(state, slot) { return -5; }
    unsafe { a20_rust_handle_slot_get_temporal(entry_ptr(state, slot), expiry_tick,
        remaining_ops, temporal_flags) }
}

#[no_mangle]
/// Raise a handle's security label without changing its allocation state.
///
/// # Safety
/// Caller holds table lock and state is live.
pub unsafe extern "C" fn a20_rust_handle_table_set_label(
    opaque: *mut OpaqueHandleTableState, slot: u32, label: u8,
) -> i32 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return ERR_INVALID };
    if slot >= state.capacity || !bit_is_set(state, slot) { return -5; }
    unsafe { a20_rust_handle_slot_set_label(entry_ptr(state, slot), label) }
}

#[no_mangle]
/// Expire an entry. AUTO_CLOSE detachment also frees its bitmap slot/count;
/// ordinary expiry leaves the slot allocated and unusable until close.
///
/// # Safety
/// Caller holds table lock; outputs are valid, disjoint pointers.
pub unsafe extern "C" fn a20_rust_handle_table_expire(
    opaque: *mut OpaqueHandleTableState, slot: u32, now: u64,
    object: *mut *mut c_void, object_type: *mut u16,
) -> i32 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return 0 };
    if slot >= state.capacity || !bit_is_set(state, slot) { return 0; }
    if unsafe { a20_rust_handle_slot_expire(entry_ptr(state, slot), now, object, object_type) } != 1 {
        return 0;
    }
    clear_bit(state, slot);
    state.used -= 1;
    state.free_hint = state.free_hint.min(slot);
    1
}

#[no_mangle]
/// Destroy at most one occupied Entry and remove its bitmap/count state.
/// Repeated calls with an increasing cursor drain the table; object release is
/// left to C after this call returns.
///
/// # Safety
/// The table has no concurrent references. Outputs are valid, disjoint and do
/// not alias the state or entry buffers.
pub unsafe extern "C" fn a20_rust_handle_table_destroy_next(
    opaque: *mut OpaqueHandleTableState, cursor: *mut u32,
    object: *mut *mut c_void, object_type: *mut u16,
) -> i32 {
    let Some(state) = (unsafe { state_mut(opaque) }) else { return 0 };
    if cursor.is_null() || object.is_null() || object_type.is_null() { return 0; }
    let mut slot = unsafe { *cursor };
    while slot < state.capacity {
        unsafe { *cursor = slot + 1; }
        if bit_is_set(state, slot) {
            if unsafe { a20_rust_handle_slot_destroy(entry_ptr(state, slot), object, object_type) } == 1 {
                clear_bit(state, slot);
                state.used -= 1;
                state.free_hint = state.free_hint.min(slot);
                return 1;
            }
            // A reservation owns a bitmap/quota slot but has no object for
            // `slot_destroy` to return. Abort its CLOSING entry and release
            // the same accounting here so finalization cannot strand it.
            if unsafe { a20_rust_handle_slot_abort(entry_ptr(state, slot)) } == 1 {
                clear_bit(state, slot);
                state.used -= 1;
                state.free_hint = state.free_hint.min(slot);
                return 1;
            }
        }
        slot += 1;
    }
    0
}

/// Reserve an explicit slot, growing first if needed. Returns 0 on success,
/// `-A20_ERR_EXISTS` (10) if already allocated, and `-A20_ERR_NO_SPACE` on
/// quota/capacity/allocation failure.
///
/// # Safety
/// `state` must be initialized and exclusively protected by its table lock.
unsafe fn reserve_at(
    state: *mut OpaqueHandleTableState,
    slot: u32,
) -> i32 {
    // SAFETY: caller fulfills the opaque-state contract.
    let Some(state) = (unsafe { state_mut(state) }) else { return ERR_INVALID };
    if slot >= state.max_capacity {
        return ERR_NO_SPACE;
    }
    while slot >= state.capacity {
        // SAFETY: caller holds lock; grow is transactional.
        if unsafe { grow(state, slot + 1) }.is_err() {
            return ERR_NO_SPACE;
        }
    }
    if bit_is_set(state, slot) {
        return -10;
    }
    if state.used >= state.max_handles { return ERR_NO_SPACE; }
    let old_hint = state.free_hint;
    set_bit(state, slot);
    // SAFETY: the slot pointer is valid and stable under the table lock.
    if unsafe { a20_rust_handle_slot_reserve(entry_ptr(state, slot)) } != 1 {
        clear_bit(state, slot);
        state.free_hint = old_hint;
        return ERR_INVALID;
    }
    state.used += 1;
    if state.free_hint <= slot {
        state.free_hint = slot.saturating_add(1).min(state.capacity);
    }
    0
}

/// Release a previously reserved or live slot after its Entry transition has
/// detached/cleared the object. Prevents count underflow and duplicate frees.
///
/// # Safety
/// `state` must be initialized and exclusively protected by its table lock.
/// Abort one reserved slot and reconcile its bitmap/count in the same Rust
/// operation. Kept private so C cannot bypass the combined table API.
unsafe fn abort_slot(
    state: *mut OpaqueHandleTableState,
    slot: u32,
) -> i32 {
    let Some(state) = (unsafe { state_mut(state) }) else { return ERR_INVALID };
    if slot >= state.capacity || !bit_is_set(state, slot) || state.used == 0 {
        return ERR_INVALID;
    }
    // SAFETY: the slot pointer is valid and stable under the table lock.
    if unsafe { a20_rust_handle_slot_abort(entry_ptr(state, slot)) } != 1 {
        return ERR_INVALID;
    }
    clear_bit(state, slot);
    state.used -= 1;
    state.free_hint = state.free_hint.min(slot);
    0
}

/// Get the capacity or accounted slot count for loop bounds/quota diagnostics.
/// These are read-only snapshots and still require the table lock.
///
/// # Safety
/// `state` must be initialized and remain protected for the call.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_handle_table_capacity(
    state: *const OpaqueHandleTableState,
) -> u32 {
    if state.is_null() || (state as usize) % align_of::<State>() != 0 { return 0; }
    let value = unsafe { &*state.cast::<State>() };
    if value.initialized == 1 { value.capacity } else { 0 }
}

/// # Safety
/// Same as `a20_rust_handle_table_capacity`.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_handle_table_used(
    state: *const OpaqueHandleTableState,
) -> u32 {
    if state.is_null() || (state as usize) % align_of::<State>() != 0 { return 0; }
    let value = unsafe { &*state.cast::<State>() };
    if value.initialized == 1 { value.used } else { 0 }
}

/// Update the table's hard handle quota while holding its lock. Used by the
/// host-only quota test hook; production tables receive their quota at init.
///
/// # Safety
/// `state` must be initialized and remain protected by the table lock.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_handle_table_set_quota(
    state: *mut OpaqueHandleTableState,
    quota: u32,
) -> i32 {
    let Some(state) = (unsafe { state_mut(state) }) else { return ERR_INVALID };
    if quota > state.max_capacity || quota < state.used { return ERR_INVALID; }
    state.max_handles = quota;
    0
}

/// Explicit finalization after every allocated slot was detached and released.
/// The caller then frees the opaque state storage itself.
///
/// # Safety
/// No concurrent reference or lock operation may exist. `used` must be zero
/// and all bitmap words must be clear. The matching allocator callbacks must
/// remain valid. On success this consumes the state exactly once; the caller
/// must not call any state function or fini again before freeing storage.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_handle_table_state_fini(
    storage: *mut OpaqueHandleTableState,
) -> i32 {
    // SAFETY: caller guarantees exclusive finalization.
    let Some(state) = (unsafe { state_mut(storage) }) else { return ERR_INVALID };
    if state.used != 0 || (0..state.bitmap_words).any(|i| unsafe { *state.bitmap.add(i as usize) != 0 }) {
        return ERR_BUSY;
    }
    if state.free.is_none() { return ERR_INVALID; }
    // Drop is the single owner of the backing buffers. It deliberately does
    // not release entries; C must detach every object before calling fini.
    unsafe { ptr::drop_in_place(storage.cast::<State>()); }
    0
}

impl Drop for State {
    fn drop(&mut self) {
        if self.initialized != 1 {
            return;
        }
        if let Some(free) = self.free {
            unsafe {
                free(self.entries);
                free(self.bitmap.cast::<u8>());
            }
        }
        self.initialized = 0;
    }
}
