//! Descriptor-table slot ownership transitions.
//!
//! The C adapter owns table allocation, task publication, locking, wakeups,
//! and VFS finalization. It calls these routines while holding the relevant
//! `files_struct` lock. This module owns the descriptor slots, close-on-exec
//! bits, open bitmap, and `next_fd` search hint. A duplicated or copied slot
//! acquires a VFile reference through the supplied callback; detached slots
//! are returned to C so their external cleanup can happen after unlocking.

#![no_std]

#[cfg(test)]
extern crate std;

use core::ffi::c_void;
use core::ptr;

const EBADF: i32 = -9;
const EMFILE: i32 = -24;
const EINVAL: i32 = -22;

// FFI precondition: every caller supplies slots/cloexec arrays with at least
// max_files elements and a mask with at least ceil(max_files / 64) words.
// C serializes all mutations with files_struct.lock; no Rust entry point takes
// that lock itself.
// Clone treats source next_fd as a trusted hint and copies it verbatim;
// allocation clamps/falls back to the bitmap. Production clone destinations
// are empty tables initialized with next_fd == 0.
pub type RefVfile = unsafe extern "C" fn(*mut c_void);
/// Return zero after acquiring one reference, or a negative errno without
/// acquiring one. Used by the checked inheritance path so host fault tests can
/// exercise rollback at every boundary; production vfile_get is infallible.
pub type TryRefVfile = unsafe extern "C" fn(*mut c_void) -> i32;
pub type DetachedSlot = unsafe extern "C" fn(i32);
pub type TableRef = unsafe extern "C" fn(*mut c_void);

unsafe fn occupied(mask: *const u64, fd: usize) -> bool {
    unsafe { mask.add(fd >> 6).read() & (1u64 << (fd & 63)) != 0 }
}

unsafe fn set_occupied(mask: *mut u64, fd: usize, value: bool) {
    let word = unsafe { mask.add(fd >> 6) };
    let bits = unsafe { word.read() };
    let bit = 1u64 << (fd & 63);
    unsafe { word.write(if value { bits | bit } else { bits & !bit }) };
}

/// Check whether an occupied descriptor slot points at `vfile`.
///
/// The caller owns the table lock and provides arrays sized for `max_files`.
/// Only set bits are visited; bits above the descriptor limit in the final
/// bitmap word are ignored.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_has_open_slot_rust(
    slots: *const *mut c_void,
    mask: *const u64,
    max_files: usize,
    vfile: *mut c_void,
) -> bool {
    if slots.is_null() || mask.is_null() || vfile.is_null() || max_files == 0 {
        return false;
    }
    let words = max_files / 64 + usize::from(max_files & 63 != 0);
    for word_index in 0..words {
        let mut open = unsafe { mask.add(word_index).read() };
        if word_index + 1 == words && max_files & 63 != 0 {
            open &= (1u64 << (max_files & 63)) - 1;
        }
        while open != 0 {
            let fd = (word_index << 6) + open.trailing_zeros() as usize;
            open &= open - 1;
            if unsafe { slots.add(fd).read() } == vfile {
                return true;
            }
        }
    }
    false
}

unsafe fn find_free(mask: *const u64, max_files: usize, min_fd: usize) -> Option<usize> {
    if min_fd >= max_files {
        return None;
    }
    let words = max_files.saturating_add(63) / 64;
    let mut word_index = min_fd >> 6;
    while word_index < words {
        let mut free = !unsafe { mask.add(word_index).read() };
        if word_index == min_fd >> 6 {
            free &= u64::MAX << (min_fd & 63);
        }
        if word_index + 1 == words && max_files & 63 != 0 {
            free &= (1u64 << (max_files & 63)) - 1;
        }
        if free != 0 {
            return Some((word_index << 6) + free.trailing_zeros() as usize);
        }
        word_index += 1;
    }
    None
}

unsafe fn note_alloc(mask: *mut u64, next_fd: *mut i32, fd: usize, max_files: usize) {
    unsafe { set_occupied(mask, fd, true) };
    if unsafe { *next_fd } < 0 || fd >= unsafe { *next_fd as usize } {
        unsafe {
            *next_fd =
                find_free(mask, max_files, fd.saturating_add(1)).map_or(-1, |next| next as i32)
        };
    }
}

unsafe fn note_free(mask: *mut u64, next_fd: *mut i32, fd: usize) {
    unsafe { set_occupied(mask, fd, false) };
    if unsafe { *next_fd } < 0 || fd < unsafe { *next_fd as usize } {
        unsafe { *next_fd = fd as i32 };
    }
}

/// Find a free descriptor slot at or above `min_fd`.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_find_free_rust(
    mask: *const u64,
    max_files: usize,
    min_fd: i32,
) -> i32 {
    if mask.is_null() || max_files == 0 {
        return -1;
    }
    let min = min_fd.max(0) as usize;
    unsafe { find_free(mask, max_files, min) }.map_or(-1, |fd| fd as i32)
}

/// Publish an already-filled slot (stdio/bootstrap path).
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_mark_alloc_rust(
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    fd: i32,
) -> i32 {
    if mask.is_null() || next_fd.is_null() || fd < 0 || fd as usize >= max_files {
        return EINVAL;
    }
    unsafe { note_alloc(mask, next_fd, fd as usize, max_files) };
    0
}

/// Remove the bitmap bit for an already-detached slot.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_mark_free_rust(
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    fd: i32,
) -> i32 {
    if mask.is_null() || next_fd.is_null() || fd < 0 || fd as usize >= max_files {
        return EINVAL;
    }
    unsafe { note_free(mask, next_fd, fd as usize) };
    0
}

/// Install a caller-owned reference in the first free slot below `limit`.
/// On failure the caller retains ownership of `vfile`.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_install_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    limit: usize,
    vfile: *mut c_void,
    set_cloexec: bool,
) -> i32 {
    if slots.is_null()
        || cloexec.is_null()
        || mask.is_null()
        || next_fd.is_null()
        || vfile.is_null()
        || max_files == 0
    {
        return EINVAL;
    }
    let limit = limit.min(max_files);
    let start = unsafe { *next_fd }.max(0) as usize;
    let fd = unsafe { find_free(mask, max_files, start.min(limit)) }
        .filter(|fd| *fd < limit)
        .or_else(|| unsafe { find_free(mask, max_files, 0) }.filter(|fd| *fd < limit));
    let Some(fd) = fd else { return EMFILE };
    if !unsafe { slots.add(fd).read() }.is_null() || unsafe { occupied(mask, fd) } {
        return EBADF;
    }
    unsafe {
        slots.add(fd).write(vfile);
        cloexec.add(fd).write(u8::from(set_cloexec));
        note_alloc(mask, next_fd, fd, max_files);
    }
    fd as i32
}

/// Detach one slot. C performs VFS lock cleanup and drops the returned ref.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_detach_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    fd: i32,
    detached: *mut *mut c_void,
) -> i32 {
    if slots.is_null()
        || cloexec.is_null()
        || mask.is_null()
        || next_fd.is_null()
        || detached.is_null()
        || fd < 0
        || fd as usize >= max_files
    {
        return EBADF;
    }
    let fd = fd as usize;
    let vf = unsafe { slots.add(fd).read() };
    if vf.is_null() || !unsafe { occupied(mask, fd) } {
        return EBADF;
    }
    unsafe {
        slots.add(fd).write(ptr::null_mut());
        cloexec.add(fd).write(0);
        note_free(mask, next_fd, fd);
        detached.write(vf);
    }
    0
}

/// Conditionally detach a slot only while it still owns `expected_vfile`.
/// This makes rollback safe when a shared descriptor table can be reused.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_detach_if_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    fd: i32,
    expected_vfile: *mut c_void,
    detached: *mut *mut c_void,
) -> i32 {
    if expected_vfile.is_null()
        || slots.is_null()
        || cloexec.is_null()
        || mask.is_null()
        || next_fd.is_null()
        || detached.is_null()
        || fd < 0
        || fd as usize >= max_files
    {
        return EBADF;
    }
    let fd = fd as usize;
    let vf = unsafe { slots.add(fd).read() };
    if vf != expected_vfile || !unsafe { occupied(mask, fd) } {
        return EBADF;
    }
    unsafe {
        slots.add(fd).write(ptr::null_mut());
        cloexec.add(fd).write(0);
        note_free(mask, next_fd, fd);
        detached.write(vf);
    }
    0
}

/// Duplicate an open slot into the lowest free slot at or above `min_fd`.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_dup_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    limit: usize,
    old_fd: i32,
    min_fd: i32,
    set_cloexec: bool,
    get_ref: Option<RefVfile>,
) -> i32 {
    if slots.is_null()
        || cloexec.is_null()
        || mask.is_null()
        || next_fd.is_null()
        || old_fd < 0
        || old_fd as usize >= max_files
    {
        return EBADF;
    }
    let old = old_fd as usize;
    let vf = unsafe { slots.add(old).read() };
    if vf.is_null() || !unsafe { occupied(mask, old) } {
        return EBADF;
    }
    let limit = limit.min(max_files);
    let min = min_fd.max(0) as usize;
    if min >= limit {
        return EMFILE;
    }
    let Some(fd) = (unsafe { find_free(mask, max_files, min) }).filter(|fd| *fd < limit) else {
        return EMFILE;
    };
    if !unsafe { slots.add(fd).read() }.is_null() || unsafe { occupied(mask, fd) } {
        return EBADF;
    }
    let Some(get_ref) = get_ref else {
        return EINVAL;
    };
    unsafe { get_ref(vf) };
    unsafe {
        slots.add(fd).write(vf);
        cloexec.add(fd).write(u8::from(set_cloexec));
        note_alloc(mask, next_fd, fd, max_files);
    }
    fd as i32
}

/// Replace `new_fd` with a duplicate of `old_fd`, returning the displaced ref.
/// The new reference is acquired before the old slot is changed.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_dup_to_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    limit: usize,
    old_fd: i32,
    new_fd: i32,
    set_cloexec: bool,
    get_ref: Option<RefVfile>,
    displaced: *mut *mut c_void,
) -> i32 {
    if slots.is_null()
        || cloexec.is_null()
        || mask.is_null()
        || next_fd.is_null()
        || displaced.is_null()
        || old_fd < 0
        || new_fd < 0
        || old_fd as usize >= max_files
        || new_fd as usize >= max_files
    {
        return EBADF;
    }
    if old_fd == new_fd {
        return EINVAL;
    }
    if new_fd as usize >= limit.min(max_files) {
        return EBADF;
    }
    let old = old_fd as usize;
    let new = new_fd as usize;
    let vf = unsafe { slots.add(old).read() };
    if vf.is_null() || !unsafe { occupied(mask, old) } {
        return EBADF;
    }
    let prior = unsafe { slots.add(new).read() };
    let prior_open = unsafe { occupied(mask, new) };
    if prior_open != !prior.is_null() {
        return EBADF;
    }
    let Some(get_ref) = get_ref else {
        return EINVAL;
    };
    unsafe { get_ref(vf) };
    unsafe {
        slots.add(new).write(vf);
        cloexec.add(new).write(u8::from(set_cloexec));
        note_alloc(mask, next_fd, new, max_files);
        displaced.write(prior);
    }
    new_fd
}

/// Copy open slots into an empty destination, retaining each VFile reference.
/// An inconsistent source is rejected before any destination changes.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_clone_rust(
    src_slots: *mut *mut c_void,
    src_cloexec: *mut u8,
    src_mask: *mut u64,
    src_next_fd: i32,
    dst_slots: *mut *mut c_void,
    dst_cloexec: *mut u8,
    dst_mask: *mut u64,
    dst_next_fd: *mut i32,
    max_files: usize,
    get_ref: Option<RefVfile>,
) -> i32 {
    if src_slots.is_null()
        || src_cloexec.is_null()
        || src_mask.is_null()
        || dst_slots.is_null()
        || dst_cloexec.is_null()
        || dst_mask.is_null()
        || dst_next_fd.is_null()
        || max_files == 0
    {
        return EINVAL;
    }
    let Some(get_ref) = get_ref else {
        return EINVAL;
    };
    for fd in 0..max_files {
        if unsafe { occupied(dst_mask, fd) }
            || !unsafe { dst_slots.add(fd).read() }.is_null()
            || unsafe { dst_cloexec.add(fd).read() } != 0
        {
            return EINVAL;
        }
    }
    // Validate both directions before acquiring any refs, so malformed tables
    // leave an empty destination and need no fallible rollback callback.
    for fd in 0..max_files {
        let is_open = unsafe { occupied(src_mask, fd) };
        let has_vfile = !unsafe { src_slots.add(fd).read() }.is_null();
        if is_open != has_vfile {
            return EBADF;
        }
    }
    for fd in 0..max_files {
        if !unsafe { occupied(src_mask, fd) } {
            continue;
        }
        let vf = unsafe { src_slots.add(fd).read() };
        unsafe { get_ref(vf) };
        unsafe {
            dst_slots.add(fd).write(vf);
            dst_cloexec.add(fd).write(src_cloexec.add(fd).read());
            set_occupied(dst_mask, fd, true);
        }
    }
    unsafe { *dst_next_fd = src_next_fd };
    0
}

/// Fallible transactional clone. The source and empty destination are fully
/// validated before the first reference is acquired. If a try_get callback
/// fails, all previously acquired references are dropped and the destination
/// is restored to its initial empty state. The callback contract guarantees
/// that a failed try_get did not acquire a reference.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_clone_checked_rust(
    src_slots: *mut *mut c_void,
    src_cloexec: *mut u8,
    src_mask: *mut u64,
    src_next_fd: i32,
    dst_slots: *mut *mut c_void,
    dst_cloexec: *mut u8,
    dst_mask: *mut u64,
    dst_next_fd: *mut i32,
    max_files: usize,
    try_get_ref: Option<TryRefVfile>,
    put_ref: Option<RefVfile>,
) -> i32 {
    if src_slots.is_null()
        || src_cloexec.is_null()
        || src_mask.is_null()
        || dst_slots.is_null()
        || dst_cloexec.is_null()
        || dst_mask.is_null()
        || dst_next_fd.is_null()
        || max_files == 0
    {
        return EINVAL;
    }
    if unsafe { *dst_next_fd } != 0 {
        return EINVAL;
    }
    let (Some(try_get_ref), Some(put_ref)) = (try_get_ref, put_ref) else {
        return EINVAL;
    };
    for fd in 0..max_files {
        if unsafe { occupied(dst_mask, fd) }
            || !unsafe { dst_slots.add(fd).read() }.is_null()
            || unsafe { dst_cloexec.add(fd).read() } != 0
        {
            return EINVAL;
        }
        let has_vfile = !unsafe { src_slots.add(fd).read() }.is_null();
        let is_open = unsafe { occupied(src_mask, fd) };
        let clo = unsafe { src_cloexec.add(fd).read() };
        if has_vfile != is_open || (!has_vfile && clo != 0) {
            return EBADF;
        }
    }

    for fd in 0..max_files {
        if !unsafe { occupied(src_mask, fd) } {
            continue;
        }
        let vf = unsafe { src_slots.add(fd).read() };
        let result = unsafe { try_get_ref(vf) };
        if result < 0 {
            for rollback_fd in 0..fd {
                let rollback_vf = unsafe { dst_slots.add(rollback_fd).read() };
                if !rollback_vf.is_null() {
                    unsafe { put_ref(rollback_vf) };
                    unsafe {
                        dst_slots.add(rollback_fd).write(ptr::null_mut());
                        dst_cloexec.add(rollback_fd).write(0);
                        set_occupied(dst_mask, rollback_fd, false);
                    }
                }
            }
            return result;
        }
        unsafe {
            dst_slots.add(fd).write(vf);
            dst_cloexec.add(fd).write(src_cloexec.add(fd).read());
            set_occupied(dst_mask, fd, true);
        }
    }
    unsafe { *dst_next_fd = src_next_fd };
    0
}

/// Install a borrowed stdio vfile into its fixed descriptor slot. Returns 1
/// when the slot was already occupied, 0 when installed, or a negative errno
/// on inconsistent table state. Caller serializes with the table lock.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_stdio_install_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    fd: usize,
    vfile: *mut c_void,
    get_ref: Option<RefVfile>,
) -> i32 {
    if slots.is_null()
        || cloexec.is_null()
        || mask.is_null()
        || next_fd.is_null()
        || vfile.is_null()
        || fd >= max_files
    {
        return EINVAL;
    }
    let slot = unsafe { slots.add(fd).read() };
    let is_open = unsafe { occupied(mask, fd) };
    if is_open != !slot.is_null() {
        return EBADF;
    }
    if is_open {
        return 1;
    }
    let Some(get_ref) = get_ref else { return EINVAL };
    unsafe { get_ref(vfile) };
    unsafe {
        slots.add(fd).write(vfile);
        cloexec.add(fd).write(0);
        note_alloc(mask, next_fd, fd, max_files);
    }
    0
}

/// Add one task owner to a pinned table. Caller holds the table lock and
/// publishes the task pointer under its park lock; the callback performs the
/// architecture-independent table reference increment.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_owner_acquire_rust(
    owners: *mut i32,
    table: *mut c_void,
    get_table_ref: Option<TableRef>,
) -> i32 {
    if owners.is_null() || table.is_null() {
        return EINVAL;
    }
    let Some(get_table_ref) = get_table_ref else {
        return EINVAL;
    };
    let current = unsafe { owners.read() };
    if current <= 0 || current == i32::MAX {
        return EINVAL;
    }
    unsafe { get_table_ref(table) };
    unsafe { owners.write(current + 1) };
    0
}

/// Remove one task owner and record which final task must release process
/// scoped file locks. Caller drops the corresponding table reference only
/// after the task and table locks have both been released.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_owner_release_rust(
    owners: *mut i32,
    release_owner_pid: *mut i32,
    pid: i32,
) -> i32 {
    if owners.is_null() || release_owner_pid.is_null() {
        return EINVAL;
    }
    let current = unsafe { owners.read() };
    if current <= 0 {
        return EINVAL;
    }
    let remaining = current - 1;
    unsafe { owners.write(remaining) };
    if remaining == 0 {
        unsafe { release_owner_pid.write(pid) };
    }
    remaining
}

/// Decide whether a table has multiple task owners. Caller holds its table
/// lock, making the result linearizable with owner acquire/release.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_owner_is_shared_rust(owners: i32) -> i32 {
    i32::from(owners > 1)
}

/// Read/write close-on-exec state only for a currently occupied slot.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_cloexec_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    max_files: usize,
    fd: i32,
    write_value: i32,
) -> i32 {
    if slots.is_null() || cloexec.is_null() || fd < 0 || fd as usize >= max_files {
        return EBADF;
    }
    let fd = fd as usize;
    if unsafe { slots.add(fd).read() }.is_null() {
        return EBADF;
    }
    if write_value >= 0 {
        unsafe { cloexec.add(fd).write(u8::from(write_value != 0)) };
        0
    } else if unsafe { cloexec.add(fd).read() } != 0 {
        1
    } else {
        0
    }
}

/// Validate the slot/bitmap/cloexec representation before a bulk transition.
/// `cloexec` must be zero in every empty slot. No table state is changed.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_validate_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    max_files: usize,
) -> i32 {
    if slots.is_null() || cloexec.is_null() || mask.is_null() || max_files == 0 {
        return EINVAL;
    }
    for fd in 0..max_files {
        let has_vfile = !unsafe { slots.add(fd).read() }.is_null();
        let is_open = unsafe { occupied(mask, fd) };
        let clo = unsafe { cloexec.add(fd).read() };
        if has_vfile != is_open || (!has_vfile && clo != 0) {
            return EBADF;
        }
    }
    0
}

/// Atomically detach all slots selected by `cloexec_only` after validating the
/// whole table and output capacity. `on_detach` runs under the caller's table
/// lock and must be a short, nonblocking metadata callback. On any error, table
/// state and output entries are untouched.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_detach_bulk_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    cloexec_only: bool,
    out_vfiles: *mut *mut c_void,
    out_capacity: usize,
    on_detach: Option<DetachedSlot>,
) -> i32 {
    if slots.is_null()
        || cloexec.is_null()
        || mask.is_null()
        || next_fd.is_null()
        || max_files == 0
    {
        return EINVAL;
    }
    let valid = unsafe { a20_fdtable_validate_rust(slots, cloexec, mask, max_files) };
    if valid < 0 {
        return valid;
    }
    let mut count = 0usize;
    for fd in 0..max_files {
        if unsafe { occupied(mask, fd) }
            && (!cloexec_only || unsafe { cloexec.add(fd).read() != 0 })
        {
            count += 1;
        }
    }
    if count > out_capacity || (count != 0 && out_vfiles.is_null()) {
        return EINVAL;
    }
    let mut out = 0usize;
    for fd in 0..max_files {
        if !unsafe { occupied(mask, fd) }
            || (cloexec_only && unsafe { cloexec.add(fd).read() == 0 })
        {
            continue;
        }
        let vf = unsafe { slots.add(fd).read() };
        unsafe {
            out_vfiles.add(out).write(vf);
            slots.add(fd).write(ptr::null_mut());
            cloexec.add(fd).write(0);
            note_free(mask, next_fd, fd);
        }
        if let Some(callback) = on_detach {
            unsafe { callback(fd as i32) };
        }
        out += 1;
    }
    count as i32
}

/// Detach the next selected slot at or after `start_fd`. This no-scratch
/// primitive supports the C adapter's allocation-failure cleanup path. The
/// caller validates the complete table once before looping while holding its
/// lock; local inconsistencies are still rejected without mutation.
#[no_mangle]
pub unsafe extern "C" fn a20_fdtable_detach_next_rust(
    slots: *mut *mut c_void,
    cloexec: *mut u8,
    mask: *mut u64,
    next_fd: *mut i32,
    max_files: usize,
    cloexec_only: bool,
    start_fd: usize,
    out_fd: *mut i32,
    out_vfile: *mut *mut c_void,
    on_detach: Option<DetachedSlot>,
) -> i32 {
    if slots.is_null()
        || cloexec.is_null()
        || mask.is_null()
        || next_fd.is_null()
        || out_fd.is_null()
        || out_vfile.is_null()
        || max_files == 0
    {
        return EINVAL;
    }
    for fd in start_fd.min(max_files)..max_files {
        let vf = unsafe { slots.add(fd).read() };
        let is_open = unsafe { occupied(mask, fd) };
        let clo = unsafe { cloexec.add(fd).read() };
        if is_open != !vf.is_null() || (vf.is_null() && clo != 0) {
            return EBADF;
        }
        if !is_open || (cloexec_only && clo == 0) {
            continue;
        }
        unsafe {
            slots.add(fd).write(ptr::null_mut());
            cloexec.add(fd).write(0);
            note_free(mask, next_fd, fd);
            out_fd.write(fd as i32);
            out_vfile.write(vf);
        }
        if let Some(callback) = on_detach {
            unsafe { callback(fd as i32) };
        }
        return 1;
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Arc, Mutex};
    use std::thread;

    unsafe extern "C" fn retain(p: *mut c_void) {
        let n = unsafe { &*(p as *const std::sync::atomic::AtomicUsize) };
        n.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    }

    unsafe extern "C" fn release(p: *mut c_void) {
        let n = unsafe { &*(p as *const std::sync::atomic::AtomicUsize) };
        n.fetch_sub(1, std::sync::atomic::Ordering::SeqCst);
    }

    static CLONE_TRY_CALLS: std::sync::atomic::AtomicUsize =
        std::sync::atomic::AtomicUsize::new(0);
    static CLONE_FAIL_AT: std::sync::atomic::AtomicUsize =
        std::sync::atomic::AtomicUsize::new(usize::MAX);

    unsafe extern "C" fn try_retain(p: *mut c_void) -> i32 {
        let call = CLONE_TRY_CALLS.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
        if call == CLONE_FAIL_AT.load(std::sync::atomic::Ordering::SeqCst) {
            return -12;
        }
        unsafe { retain(p) };
        0
    }

    unsafe extern "C" fn increment_table_ref(p: *mut c_void) {
        let refs = unsafe { &*(p as *const std::sync::atomic::AtomicUsize) };
        refs.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    }

    static DETACHED_CALLBACKS: std::sync::atomic::AtomicUsize =
        std::sync::atomic::AtomicUsize::new(0);

    unsafe extern "C" fn count_detached(_fd: i32) {
        DETACHED_CALLBACKS.fetch_add(1, std::sync::atomic::Ordering::SeqCst);
    }

    struct SharedTable([*mut c_void; 256], [u8; 256], [u64; 4], i32);
    // SAFETY: test code accesses the raw-pointer table only while holding the
    // surrounding mutex; the sentinel pointer is never dereferenced.
    unsafe impl Send for SharedTable {}

    #[test]
    fn allocate_permission_boundary_and_close_reuse() {
        let mut slots = [ptr::null_mut(); 130];
        let mut clo = [0u8; 130];
        let mut mask = [0u64; 3];
        let mut next = 0;
        let token = 0x100usize as *mut c_void;
        assert_eq!(
            unsafe {
                a20_fdtable_install_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    130,
                    4,
                    token,
                    true,
                )
            },
            0
        );
        assert_eq!(clo[0], 1);
        assert_eq!(
            unsafe { a20_fdtable_cloexec_rust(slots.as_mut_ptr(), clo.as_mut_ptr(), 130, 0, -1) },
            1
        );
        assert_eq!(
            unsafe { a20_fdtable_cloexec_rust(slots.as_mut_ptr(), clo.as_mut_ptr(), 130, 0, 0) },
            0
        );
        assert_eq!(clo[0], 0);
        assert_eq!(
            unsafe {
                a20_fdtable_install_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    130,
                    4,
                    token,
                    false,
                )
            },
            1
        );
        assert_eq!(
            unsafe {
                a20_fdtable_install_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    130,
                    2,
                    token,
                    false,
                )
            },
            EMFILE
        );
        assert_eq!(
            unsafe { a20_fdtable_find_free_rust(mask.as_ptr(), 130, 64) },
            64
        );
        let mut detached = ptr::null_mut();
        assert_eq!(
            unsafe {
                a20_fdtable_detach_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    130,
                    0,
                    &mut detached,
                )
            },
            0
        );
        assert_eq!(detached, token);
        assert_eq!(next, 0);
        assert_eq!(
            unsafe {
                a20_fdtable_install_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    130,
                    4,
                    token,
                    false,
                )
            },
            0
        );
    }

    #[test]
    fn dup_and_dup_to_preserve_refs_on_failure_and_release_displaced() {
        let refs = Arc::new(std::sync::atomic::AtomicUsize::new(1));
        let token = Arc::as_ptr(&refs) as *mut c_void;
        let mut slots = [ptr::null_mut(); 3];
        let mut clo = [0u8; 3];
        let mut mask = [0u64; 1];
        let mut next = 0;
        assert_eq!(
            unsafe {
                a20_fdtable_install_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    3,
                    3,
                    token,
                    false,
                )
            },
            0
        );
        assert_eq!(
            unsafe {
                a20_fdtable_dup_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    3,
                    1,
                    0,
                    1,
                    true,
                    Some(retain),
                )
            },
            EMFILE
        );
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 1);
        assert_eq!(
            unsafe {
                a20_fdtable_dup_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    3,
                    3,
                    0,
                    1,
                    true,
                    Some(retain),
                )
            },
            1
        );
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 2);
        assert_eq!(clo[1], 1);
        let mut displaced = ptr::null_mut();
        assert_eq!(
            unsafe {
                a20_fdtable_dup_to_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    3,
                    3,
                    0,
                    1,
                    false,
                    Some(retain),
                    &mut displaced,
                )
            },
            1
        );
        assert_eq!(displaced, token);
        unsafe { release(displaced) };
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 2);
    }

    #[test]
    fn clone_preflight_rejects_inconsistent_source_without_partial_copy() {
        let refs = Arc::new(std::sync::atomic::AtomicUsize::new(1));
        let token = Arc::as_ptr(&refs) as *mut c_void;
        let mut src = [ptr::null_mut(); 2];
        let mut src_clo = [0u8; 2];
        let mut src_mask = [0u64; 1];
        let mut dst = [ptr::null_mut(); 2];
        let mut dst_clo = [0u8; 2];
        let mut dst_mask = [0u64; 1];
        let mut next = 0;
        src[0] = token;
        src_mask[0] = 0b11;
        assert_eq!(
            unsafe {
                a20_fdtable_clone_rust(
                    src.as_mut_ptr(),
                    src_clo.as_mut_ptr(),
                    src_mask.as_mut_ptr(),
                    0,
                    dst.as_mut_ptr(),
                    dst_clo.as_mut_ptr(),
                    dst_mask.as_mut_ptr(),
                    &mut next,
                    2,
                    Some(retain),
                )
            },
            EBADF
        );
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 1);
        assert_eq!(dst[0], ptr::null_mut());
        assert_eq!(dst_mask[0], 0);

        // The inverse mismatch matters too: a stale pointer must not become
        // an untracked destination reference when the bitmap says free.
        src[1] = token;
        src_mask[0] = 1;
        assert_eq!(
            unsafe {
                a20_fdtable_clone_rust(
                    src.as_mut_ptr(),
                    src_clo.as_mut_ptr(),
                    src_mask.as_mut_ptr(),
                    0,
                    dst.as_mut_ptr(),
                    dst_clo.as_mut_ptr(),
                    dst_mask.as_mut_ptr(),
                    &mut next,
                    2,
                    Some(retain),
                )
            },
            EBADF
        );
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 1);
        assert_eq!(dst[1], ptr::null_mut());
        assert_eq!(dst_mask[0], 0);
    }

    #[test]
    fn clone_acquires_one_reference_per_slot_and_preserves_cloexec() {
        let refs = Arc::new(std::sync::atomic::AtomicUsize::new(1));
        let token = Arc::as_ptr(&refs) as *mut c_void;
        let mut src = [ptr::null_mut(); 3];
        let mut src_clo = [0u8; 3];
        let mut src_mask = [0u64; 1];
        let mut dst = [ptr::null_mut(); 3];
        let mut dst_clo = [0u8; 3];
        let mut dst_mask = [0u64; 1];
        let mut next = 0;
        src[1] = token;
        src_clo[1] = 1;
        src_mask[0] = 1 << 1;
        assert_eq!(
            unsafe {
                a20_fdtable_clone_rust(
                    src.as_mut_ptr(),
                    src_clo.as_mut_ptr(),
                    src_mask.as_mut_ptr(),
                    0,
                    dst.as_mut_ptr(),
                    dst_clo.as_mut_ptr(),
                    dst_mask.as_mut_ptr(),
                    &mut next,
                    3,
                    Some(retain),
                )
            },
            0
        );
        assert_eq!(dst[1], token);
        assert_eq!(dst_clo[1], 1);
        assert_eq!(dst_mask[0], 1 << 1);
        assert_eq!(next, 0);
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 2);
        let mut detached = ptr::null_mut();
        assert_eq!(
            unsafe {
                a20_fdtable_detach_rust(
                    dst.as_mut_ptr(),
                    dst_clo.as_mut_ptr(),
                    dst_mask.as_mut_ptr(),
                    &mut next,
                    3,
                    1,
                    &mut detached,
                )
            },
            0
        );
        assert_eq!(detached, token);
        unsafe { release(detached) };
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 1);
    }

    #[test]
    fn checked_clone_rolls_back_each_failed_reference_boundary() {
        for fail_at in 0..3 {
            let refs = Arc::new(std::sync::atomic::AtomicUsize::new(1));
            let token = Arc::as_ptr(&refs) as *mut c_void;
            let mut src = [ptr::null_mut(); 4];
            let mut src_clo = [0u8; 4];
            let mut src_mask = [0u64; 1];
            let mut dst = [ptr::null_mut(); 4];
            let mut dst_clo = [0u8; 4];
            let mut dst_mask = [0u64; 1];
            let mut next = 0;
            src[0] = token;
            src[2] = token;
            src[3] = token;
            src_clo[2] = 1;
            src_mask[0] = 0b1101;
            CLONE_TRY_CALLS.store(0, std::sync::atomic::Ordering::SeqCst);
            CLONE_FAIL_AT.store(fail_at, std::sync::atomic::Ordering::SeqCst);
            assert_eq!(
                unsafe {
                    a20_fdtable_clone_checked_rust(
                        src.as_mut_ptr(),
                        src_clo.as_mut_ptr(),
                        src_mask.as_mut_ptr(),
                        4,
                        dst.as_mut_ptr(),
                        dst_clo.as_mut_ptr(),
                        dst_mask.as_mut_ptr(),
                        &mut next,
                        4,
                        Some(try_retain),
                        Some(release),
                    )
                },
                -12
            );
            assert!(dst.iter().all(|vf| vf.is_null()));
            assert!(dst_clo.iter().all(|value| *value == 0));
            assert_eq!(dst_mask[0], 0);
            assert_eq!(next, 0);
            assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 1);
        }

        let refs = Arc::new(std::sync::atomic::AtomicUsize::new(1));
        let token = Arc::as_ptr(&refs) as *mut c_void;
        let mut src = [ptr::null_mut(); 3];
        let mut src_clo = [0u8; 3];
        let mut src_mask = [0u64; 1];
        let mut dst = [ptr::null_mut(); 3];
        let mut dst_clo = [0u8; 3];
        let mut dst_mask = [0u64; 1];
        let mut next = 0;
        src[0] = token;
        src[2] = token;
        src_clo[2] = 1;
        src_mask[0] = 0b101;
        CLONE_TRY_CALLS.store(0, std::sync::atomic::Ordering::SeqCst);
        CLONE_FAIL_AT.store(usize::MAX, std::sync::atomic::Ordering::SeqCst);
        assert_eq!(
            unsafe {
                a20_fdtable_clone_checked_rust(
                    src.as_mut_ptr(),
                    src_clo.as_mut_ptr(),
                    src_mask.as_mut_ptr(),
                    3,
                    dst.as_mut_ptr(),
                    dst_clo.as_mut_ptr(),
                    dst_mask.as_mut_ptr(),
                    &mut next,
                    3,
                    Some(try_retain),
                    Some(release),
                )
            },
            0
        );
        assert_eq!(dst_mask[0], 0b101);
        assert_eq!(dst_clo[2], 1);
        assert_eq!(next, 3);
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 3);
        unsafe {
            release(dst[0]);
            release(dst[2]);
        }
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 1);

        // A private destination must be entirely empty, including its
        // close-on-exec bytes; reject corruption before acquiring any refs.
        let refs = Arc::new(std::sync::atomic::AtomicUsize::new(1));
        let token = Arc::as_ptr(&refs) as *mut c_void;
        let mut src = [ptr::null_mut(); 1];
        let mut src_clo = [0u8; 1];
        let mut src_mask = [0u64; 1];
        let mut dst = [ptr::null_mut(); 1];
        let mut dst_clo = [0u8; 1];
        let mut dst_mask = [0u64; 1];
        let mut next = 19;
        src[0] = token;
        src_mask[0] = 1;
        assert_eq!(unsafe {
            a20_fdtable_clone_checked_rust(
                src.as_mut_ptr(), src_clo.as_mut_ptr(), src_mask.as_mut_ptr(), 1,
                dst.as_mut_ptr(), dst_clo.as_mut_ptr(), dst_mask.as_mut_ptr(),
                &mut next, 1, Some(try_retain), Some(release),
            )
        }, EINVAL);
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 1);
        assert_eq!(next, 19);

        dst_clo[0] = 1;
        next = 0;
        assert_eq!(unsafe {
            a20_fdtable_clone_checked_rust(
                src.as_mut_ptr(), src_clo.as_mut_ptr(), src_mask.as_mut_ptr(), 1,
                dst.as_mut_ptr(), dst_clo.as_mut_ptr(), dst_mask.as_mut_ptr(),
                &mut next, 1, Some(try_retain), Some(release),
            )
        }, EINVAL);
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 1);
        assert_eq!(next, 0);
    }

    #[test]
    fn stdio_helper_installs_fixed_slot_once_and_rejects_corruption() {
        let refs = Arc::new(std::sync::atomic::AtomicUsize::new(1));
        let token = Arc::as_ptr(&refs) as *mut c_void;
        let mut slots = [ptr::null_mut(); 4];
        let mut clo = [0u8; 4];
        let mut mask = [0u64; 1];
        let mut next = 0;
        let slots_ptr = slots.as_mut_ptr();
        let clo_ptr = clo.as_mut_ptr();
        let mask_ptr = mask.as_mut_ptr();
        let next_ptr = &mut next as *mut i32;
        let install = || unsafe {
            a20_fdtable_stdio_install_rust(
                slots_ptr, clo_ptr, mask_ptr, next_ptr,
                4, 2, token, Some(retain),
            )
        };
        assert_eq!(install(), 0);
        assert_eq!(slots[2], token);
        assert_eq!(mask[0], 1 << 2);
        assert_eq!(clo[2], 0);
        assert_eq!(next, 3);
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 2);
        assert_eq!(install(), 1);
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 2);
        unsafe { mask_ptr.write(0) };
        assert_eq!(install(), EBADF);
    }

    #[test]
    fn task_table_owner_transitions_pair_owner_and_table_refs() {
        let refs = std::sync::atomic::AtomicUsize::new(1);
        let table = (&refs as *const std::sync::atomic::AtomicUsize)
            .cast_mut()
            .cast::<c_void>();
        let mut owners = 1;
        let mut release_pid = -1;
        assert_eq!(unsafe {
            a20_fdtable_owner_acquire_rust(&mut owners, table, Some(increment_table_ref))
        }, 0);
        assert_eq!(owners, 2);
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 2);
        assert_eq!(unsafe { a20_fdtable_owner_is_shared_rust(owners) }, 1);
        assert_eq!(unsafe { a20_fdtable_owner_release_rust(&mut owners, &mut release_pid, 42) }, 1);
        assert_eq!(owners, 1);
        assert_eq!(release_pid, -1);
        assert_eq!(unsafe { a20_fdtable_owner_release_rust(&mut owners, &mut release_pid, 43) }, 0);
        assert_eq!(owners, 0);
        assert_eq!(release_pid, 43);
        assert_eq!(unsafe { a20_fdtable_owner_is_shared_rust(owners) }, 0);
        assert_eq!(unsafe { a20_fdtable_owner_release_rust(&mut owners, &mut release_pid, 44) }, EINVAL);
        assert_eq!(refs.load(std::sync::atomic::Ordering::SeqCst), 2);
    }

    #[test]
    fn open_slot_membership_uses_occupied_bits_and_clamps_last_word() {
        let mut slots = [ptr::null_mut(); 130];
        let mut mask = [0u64; 3];
        let target = 0xabcusize as *mut c_void;
        let other = 0xdefusize as *mut c_void;
        slots[63] = target;
        slots[64] = other;
        slots[129] = target;
        // Deliberately poison bits beyond max_files in the final word.
        mask[0] = 1u64 << 63;
        mask[1] = 1u64 << 0;
        mask[2] = (1u64 << 1) | (1u64 << 2) | (1u64 << 63);
        assert!(unsafe {
            a20_fdtable_has_open_slot_rust(slots.as_ptr(), mask.as_ptr(), 130, target)
        });
        assert!(unsafe {
            a20_fdtable_has_open_slot_rust(slots.as_ptr(), mask.as_ptr(), 130, other)
        });
        mask[0] = 0;
        assert!(unsafe {
            a20_fdtable_has_open_slot_rust(slots.as_ptr(), mask.as_ptr(), 130, target)
        });
        slots[129] = ptr::null_mut();
        mask[2] &= !(1u64 << 1);
        // A stale pointer is not a live descriptor without its occupied bit.
        slots[65] = target;
        assert!(!unsafe {
            a20_fdtable_has_open_slot_rust(slots.as_ptr(), mask.as_ptr(), 130, target)
        });
        assert!(!unsafe {
            a20_fdtable_has_open_slot_rust(slots.as_ptr(), mask.as_ptr(), 0, target)
        });
        assert!(!unsafe {
            a20_fdtable_has_open_slot_rust(ptr::null(), mask.as_ptr(), 130, target)
        });
        assert!(!unsafe {
            a20_fdtable_has_open_slot_rust(slots.as_ptr(), ptr::null(), 130, target)
        });
        assert!(!unsafe {
            a20_fdtable_has_open_slot_rust(slots.as_ptr(), mask.as_ptr(), 130, ptr::null_mut())
        });
    }

    #[test]
    fn conditional_detach_does_not_close_reused_descriptor() {
        let mut slots = [ptr::null_mut(); 1];
        let mut clo = [0u8; 1];
        let mut mask = [0u64; 1];
        let mut next = 0;
        let first = 0x100usize as *mut c_void;
        let replacement = 0x200usize as *mut c_void;
        assert_eq!(
            unsafe {
                a20_fdtable_install_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    1,
                    1,
                    first,
                    false,
                )
            },
            0
        );
        let mut detached = ptr::null_mut();
        assert_eq!(
            unsafe {
                a20_fdtable_detach_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    1,
                    0,
                    &mut detached,
                )
            },
            0
        );
        assert_eq!(detached, first);
        assert_eq!(
            unsafe {
                a20_fdtable_install_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    1,
                    1,
                    replacement,
                    false,
                )
            },
            0
        );
        assert_eq!(
            unsafe {
                a20_fdtable_detach_if_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    1,
                    0,
                    first,
                    &mut detached,
                )
            },
            EBADF
        );
        assert_eq!(slots[0], replacement);
        assert_eq!(mask[0], 1);
        assert_eq!(
            unsafe {
                a20_fdtable_detach_if_rust(
                    slots.as_mut_ptr(),
                    clo.as_mut_ptr(),
                    mask.as_mut_ptr(),
                    &mut next,
                    1,
                    0,
                    replacement,
                    &mut detached,
                )
            },
            0
        );
        assert_eq!(detached, replacement);
    }

    #[test]
    fn bulk_and_cursor_detach_validate_before_mutating() {
        let mut slots = [ptr::null_mut(); 4];
        let mut clo = [0u8; 4];
        let mut mask = [0u64; 1];
        let mut next = 0;
        let a = 0x100usize as *mut c_void;
        let b = 0x200usize as *mut c_void;
        slots[0] = a;
        slots[2] = b;
        clo[0] = 1;
        mask[0] = (1 << 0) | (1 << 2);
        let mut out = [0x300usize as *mut c_void; 4];
        DETACHED_CALLBACKS.store(0, std::sync::atomic::Ordering::SeqCst);

        // Non-null slot without a bitmap bit: bulk validation rejects the
        // entire operation before clearing a valid earlier cloexec slot.
        slots[3] = 0x400usize as *mut c_void;
        assert_eq!(
            unsafe {
                a20_fdtable_detach_bulk_rust(
                    slots.as_mut_ptr(), clo.as_mut_ptr(), mask.as_mut_ptr(),
                    &mut next, 4, true, out.as_mut_ptr(), 4, Some(count_detached),
                )
            },
            EBADF
        );
        assert_eq!(slots[0], a);
        assert_eq!(clo[0], 1);
        assert_eq!(mask[0], (1 << 0) | (1 << 2));
        assert_eq!(out[0], 0x300usize as *mut c_void);
        assert_eq!(DETACHED_CALLBACKS.load(std::sync::atomic::Ordering::SeqCst), 0);

        slots[3] = ptr::null_mut();
        // Bitmap bit without a VFile also rejects without acquiring/dropping
        // a reference or partially detaching the valid prefix.
        mask[0] |= 1 << 1;
        assert_eq!(
            unsafe { a20_fdtable_validate_rust(slots.as_mut_ptr(), clo.as_mut_ptr(), mask.as_mut_ptr(), 4) },
            EBADF
        );
        assert_eq!(
            unsafe {
                a20_fdtable_detach_bulk_rust(
                    slots.as_mut_ptr(), clo.as_mut_ptr(), mask.as_mut_ptr(),
                    &mut next, 4, true, out.as_mut_ptr(), 4, Some(count_detached),
                )
            },
            EBADF
        );
        assert_eq!(slots[0], a);
        assert_eq!(mask[0], (1 << 0) | (1 << 1) | (1 << 2));
        assert_eq!(DETACHED_CALLBACKS.load(std::sync::atomic::Ordering::SeqCst), 0);

        mask[0] &= !(1 << 1);
        assert_eq!(
            unsafe {
                a20_fdtable_detach_bulk_rust(
                    slots.as_mut_ptr(), clo.as_mut_ptr(), mask.as_mut_ptr(),
                    &mut next, 4, true, out.as_mut_ptr(), 4, Some(count_detached),
                )
            },
            1
        );
        assert_eq!(out[0], a);
        assert_eq!(slots[0], ptr::null_mut());
        assert_eq!(slots[2], b);
        assert_eq!(mask[0], 1 << 2);
        assert_eq!(DETACHED_CALLBACKS.load(std::sync::atomic::Ordering::SeqCst), 1);

        let mut out_fd = -1;
        let mut out_vfile = ptr::null_mut();
        assert_eq!(
            unsafe {
                a20_fdtable_detach_next_rust(
                    slots.as_mut_ptr(), clo.as_mut_ptr(), mask.as_mut_ptr(),
                    &mut next, 4, false, 0, &mut out_fd, &mut out_vfile,
                    Some(count_detached),
                )
            },
            1
        );
        assert_eq!(out_fd, 2);
        assert_eq!(out_vfile, b);
        assert_eq!(mask[0], 0);
        assert_eq!(
            unsafe {
                a20_fdtable_detach_next_rust(
                    slots.as_mut_ptr(), clo.as_mut_ptr(), mask.as_mut_ptr(),
                    &mut next, 4, false, 3, &mut out_fd, &mut out_vfile,
                    Some(count_detached),
                )
            },
            0
        );
        assert_eq!(DETACHED_CALLBACKS.load(std::sync::atomic::Ordering::SeqCst), 2);
        assert_eq!(
            unsafe { a20_fdtable_validate_rust(slots.as_mut_ptr(), clo.as_mut_ptr(), mask.as_mut_ptr(), 4) },
            0
        );
    }

    #[test]
    fn concurrent_allocations_are_unique_when_adapter_serializes_transitions() {
        let table = Arc::new(Mutex::new(SharedTable(
            [ptr::null_mut(); 256],
            [0u8; 256],
            [0u64; 4],
            0i32,
        )));
        let mut joins = std::vec::Vec::new();
        for _ in 0..8 {
            let table = table.clone();
            joins.push(thread::spawn(move || {
                let mut state = table.lock().unwrap();
                let SharedTable(slots, clo, mask, next) = &mut *state;
                unsafe {
                    a20_fdtable_install_rust(
                        slots.as_mut_ptr(),
                        clo.as_mut_ptr(),
                        mask.as_mut_ptr(),
                        next,
                        256,
                        256,
                        0x100usize as *mut c_void,
                        false,
                    )
                }
            }));
        }
        let mut got = std::vec::Vec::new();
        for join in joins {
            got.push(join.join().unwrap());
        }
        got.sort_unstable();
        assert_eq!(got, (0..8).collect::<std::vec::Vec<_>>());
        let state = table.lock().unwrap();
        assert_eq!(state.2[0].count_ones(), 8);
    }
}
