#![no_std]

//! Userfaultfd range ownership and lookup transactions.
//!
//! Rust owns the live range registry, overlap rules, and the references held
//! across fault resolution. C remains the mechanism bridge for MM mapping,
//! page-table marks, allocation, usercopy, and wait queues.

use core::ffi::c_void;
use core::ptr;

const UFFD_MISSING: u64 = 1;
const EINVAL: i32 = 22;
const EEXIST: i32 = 17;
const ENOMEM: i32 = 12;
const ENOENT: i32 = 2;

#[repr(C)]
struct Range {
    mm: *mut c_void,
    start: usize,
    end: usize,
    flags: u64,
    owner: *mut c_void,
    next: *mut Range,
    refs: usize,
    removed: u32,
}

static mut RANGES: *mut Range = ptr::null_mut();

unsafe extern "C" {
    fn a20_uffd_registry_lock() -> u64;
    fn a20_uffd_registry_unlock(flags: u64);
    fn a20_uffd_owner_get(owner: *mut c_void) -> bool;
    fn a20_uffd_owner_put(owner: *mut c_void);
    fn a20_uffd_clear_marks(mm: *mut c_void, start: usize, end: usize);
    fn mm_get(mm: *mut c_void) -> *mut c_void;
    fn mm_destroy(mm: *mut c_void);
    fn kmalloc(size: usize) -> *mut c_void;
    fn kfree(ptr: *mut c_void);
}

unsafe fn range_free(range: *mut Range) {
    let r = unsafe { &*range };
    unsafe {
        mm_destroy(r.mm);
        a20_uffd_owner_put(r.owner);
        kfree(range.cast());
    }
}

unsafe fn range_put_registry_ref(range: *mut Range) {
    let flags = unsafe { a20_uffd_registry_lock() };
    let r = unsafe { &mut *range };
    r.refs -= 1;
    let free = r.refs == 0 && r.removed != 0;
    unsafe { a20_uffd_registry_unlock(flags) };
    if free {
        unsafe { range_free(range) };
    }
}

struct RangePin(*mut Range);

impl RangePin {
    fn mm(&self) -> *mut c_void {
        unsafe { (*self.0).mm }
    }
}

impl Drop for RangePin {
    fn drop(&mut self) {
        unsafe { range_put_registry_ref(self.0) };
    }
}

/// Add a range after the caller has validated the anonymous private VMA span.
#[no_mangle]
pub unsafe extern "C" fn a20_uffd_range_register(
    owner: *mut c_void,
    mm: *mut c_void,
    start: usize,
    end: usize,
    flags: u64,
) -> i32 {
    if owner.is_null() || mm.is_null() || start >= end || flags & !UFFD_MISSING != 0 {
        return -EINVAL;
    }
    let range = unsafe { kmalloc(core::mem::size_of::<Range>()) }.cast::<Range>();
    if range.is_null() {
        return -ENOMEM;
    }
    let held_mm = unsafe { mm_get(mm) };
    if held_mm.is_null() || !unsafe { a20_uffd_owner_get(owner) } {
        if !held_mm.is_null() {
            unsafe { mm_destroy(held_mm) };
        }
        unsafe { kfree(range.cast()) };
        return -ENOENT;
    }
    unsafe {
        ptr::write(
            range,
            Range {
                mm: held_mm,
                start,
                end,
                flags,
                owner,
                next: ptr::null_mut(),
                refs: 1, // registry's ownership reference
                removed: 0,
            },
        );
    }

    let lock_flags = unsafe { a20_uffd_registry_lock() };
    let mut cur = unsafe { RANGES };
    while !cur.is_null() {
        let r = unsafe { &*cur };
        if r.owner == owner && r.start < end && r.end > start {
            unsafe { a20_uffd_registry_unlock(lock_flags) };
            unsafe {
                mm_destroy((*range).mm);
                a20_uffd_owner_put((*range).owner);
                kfree(range.cast());
            }
            return -EEXIST;
        }
        cur = r.next;
    }
    unsafe {
        (*range).next = RANGES;
        RANGES = range;
        a20_uffd_registry_unlock(lock_flags);
    }
    0
}

/// Find a covering range and pin its registration across an MM operation.
unsafe fn range_acquire(owner: *mut c_void, start: u64, len: u64) -> Result<RangePin, i32> {
    if owner.is_null() || len == 0 {
        return Err(-EINVAL);
    }
    let Some(end) = start.checked_add(len) else {
        return Err(-EINVAL);
    };
    let Ok(start) = usize::try_from(start) else {
        return Err(-ENOENT);
    };
    let Ok(end) = usize::try_from(end) else {
        return Err(-ENOENT);
    };
    let flags = unsafe { a20_uffd_registry_lock() };
    let mut cur = unsafe { RANGES };
    while !cur.is_null() {
        let r = unsafe { &mut *cur };
        if r.owner == owner && r.start <= start && r.end >= end && r.flags & UFFD_MISSING != 0 {
            r.refs += 1;
            unsafe {
                a20_uffd_registry_unlock(flags);
            }
            return Ok(RangePin(cur));
        }
        cur = r.next;
    }
    unsafe { a20_uffd_registry_unlock(flags) };
    Err(-ENOENT)
}

/// Run an MM operation while holding a scoped registration pin. If unregister
/// races the callback, the detached range and its mm/owner references remain
/// alive until this guard is dropped on every return path.
#[no_mangle]
pub unsafe extern "C" fn a20_uffd_range_with(
    owner: *mut c_void,
    start: u64,
    len: u64,
    callback: Option<unsafe extern "C" fn(*mut c_void, *mut c_void) -> i32>,
    context: *mut c_void,
) -> i32 {
    let Some(callback) = callback else {
        return -EINVAL;
    };
    let pin = match unsafe { range_acquire(owner, start, len) } {
        Ok(pin) => pin,
        Err(error) => return error,
    };
    unsafe { callback(pin.mm(), context) }
}

/// Pin the owner while the fault path posts an event and waits on its queue.
#[no_mangle]
pub unsafe extern "C" fn a20_uffd_range_lookup_owner(mm: *mut c_void, page: usize) -> *mut c_void {
    if mm.is_null() {
        return ptr::null_mut();
    }
    let flags = unsafe { a20_uffd_registry_lock() };
    let mut cur = unsafe { RANGES };
    while !cur.is_null() {
        let r = unsafe { &*cur };
        if r.mm == mm && r.start <= page && page < r.end && r.flags & UFFD_MISSING != 0 {
            let owner = r.owner;
            let held = unsafe { a20_uffd_owner_get(owner) };
            unsafe { a20_uffd_registry_unlock(flags) };
            return if held { owner } else { ptr::null_mut() };
        }
        cur = r.next;
    }
    unsafe { a20_uffd_registry_unlock(flags) };
    ptr::null_mut()
}

/// Test whether any live missing-mode registration covers a page.
#[no_mangle]
pub unsafe extern "C" fn a20_uffd_range_present(mm: *mut c_void, page: usize) -> i32 {
    if mm.is_null() {
        return 0;
    }
    let flags = unsafe { a20_uffd_registry_lock() };
    let mut cur = unsafe { RANGES };
    while !cur.is_null() {
        let r = unsafe { &*cur };
        if r.mm == mm && r.start <= page && page < r.end && r.flags & UFFD_MISSING != 0 {
            unsafe { a20_uffd_registry_unlock(flags) };
            return 1;
        }
        cur = r.next;
    }
    unsafe { a20_uffd_registry_unlock(flags) };
    0
}

unsafe fn detach_ranges(
    owner: *mut c_void,
    mm: *mut c_void,
    start: usize,
    end: usize,
) -> (*mut Range, usize) {
    let flags = unsafe { a20_uffd_registry_lock() };
    let mut detached = ptr::null_mut();
    let mut count = 0usize;
    let mut link = ptr::addr_of_mut!(RANGES);
    unsafe {
        while !(*link).is_null() {
            let range = *link;
            let r = &mut *range;
            let owner_match = owner.is_null() || r.owner == owner;
            let mm_match = mm.is_null() || r.mm == mm;
            let span_match = start == end || (r.start < end && r.end > start);
            if owner_match && mm_match && span_match {
                *link = r.next;
                r.removed = 1;
                r.next = detached;
                detached = range;
                count += 1;
            } else {
                link = &mut r.next;
            }
        }
        a20_uffd_registry_unlock(flags);
    }
    (detached, count)
}

unsafe fn release_detached(mut detached: *mut Range) {
    while !detached.is_null() {
        let range = detached;
        detached = unsafe { (*range).next };
        unsafe {
            (*range).next = ptr::null_mut();
            a20_uffd_clear_marks((*range).mm, (*range).start, (*range).end);
        }
        unsafe { range_put_registry_ref(range) };
    }
}

/// Unregister every owner range intersecting the requested interval.
#[no_mangle]
pub unsafe extern "C" fn a20_uffd_range_unregister(
    owner: *mut c_void,
    start: usize,
    end: usize,
) -> usize {
    let (ranges, count) = unsafe { detach_ranges(owner, ptr::null_mut(), start, end) };
    unsafe { release_detached(ranges) };
    count
}

/// Release all owner registrations when the descriptor closes.
#[no_mangle]
pub unsafe extern "C" fn a20_uffd_range_release_owner(owner: *mut c_void) -> usize {
    let (ranges, count) = unsafe { detach_ranges(owner, ptr::null_mut(), 0, 0) };
    unsafe { release_detached(ranges) };
    count
}
