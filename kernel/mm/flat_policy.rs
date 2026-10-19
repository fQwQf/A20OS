#![no_std]

//! Portable ownership for trusted NOMMU programs: the flat-space decisions.
//!
//! This tracker says which address ranges a trusted program may pass to the
//! kernel; it does not provide hardware isolation, so what it owns is the
//! bookkeeping that keeps a range from being handed out twice, released while
//! someone is using it, or leaked when an allocation fails part way through:
//!
//!   * **Round and reject before touching the space.**  A zero or overflowing
//!     length is refused, so no caller can ask for a range whose end wraps.
//!   * **Allocate outside the lock, publish inside it.**  `kcalloc` can
//!     reclaim, and no lock may be held across that.  The region is therefore
//!     built first and re-checked against the list under the lock, because
//!     another task may have taken the address meanwhile; a lost re-check frees
//!     both the region and its backing allocation, so nothing leaks and nothing
//!     is published.
//!   * **Refuse to release a pinned range.**  A pin is the wait lease that
//!     keeps unmap and address reuse apart, so `unmap` and `destroy` answer
//!     `-EBUSY` while any pin is held rather than freeing memory a caller is
//!     still reading.
//!   * **Free outside the lock.**  Both release paths detach the region under
//!     the lock and free it after, so the allocator never runs with the space
//!     lock held.
//!   * **A closed space takes no more work.**  `destroy` marks the space closed
//!     while detaching the list, and every entry point refuses once it is.
//!
//! What stays C is the `a20_flat_region_t`/`a20_flat_space_t` layout and the
//! list surgery, the spinlock, the allocator and `memcpy`.  None of the bridges
//! decides anything.

use core::ffi::c_void;

const PAGE_SIZE: usize = 4096;

const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
const EBUSY: i32 = -16;
const EFAULT: i32 = -14;

unsafe extern "C" {
    fn a20_flat_init_space(s: *mut c_void);
    fn a20_flat_lock(s: *mut c_void) -> u64;
    fn a20_flat_unlock(s: *mut c_void, flags: u64);
    fn a20_flat_is_closed(s: *const c_void) -> i32;
    fn a20_flat_set_closed(s: *mut c_void);
    fn a20_flat_head(s: *const c_void) -> *mut c_void;
    fn a20_flat_set_head(s: *mut c_void, r: *mut c_void);
    fn a20_flat_next(r: *const c_void) -> *mut c_void;
    fn a20_flat_set_next(r: *mut c_void, next: *mut c_void);
    fn a20_flat_region_fields(
        r: *const c_void,
        base: *mut usize,
        size: *mut usize,
        allocation: *mut *mut c_void,
        pins: *mut u32,
    );
    fn a20_flat_set_region(
        r: *mut c_void,
        next: *mut c_void,
        base: usize,
        size: usize,
        allocation: *mut c_void,
    );
    fn a20_flat_set_pins(r: *mut c_void, pins: u32);
    /// Allocate one region record, and the page-aligned backing store of at
    /// least `size` bytes for it.  C owns both sizes.
    fn a20_flat_region_alloc() -> *mut c_void;
    fn a20_flat_backing_alloc(size: usize) -> *mut c_void;
    fn a20_flat_kfree(p: *mut c_void);
    fn a20_flat_copy_bytes(dst: *mut c_void, src: *const c_void, n: usize);
}

/// Round a request up to a page and reject one that cannot be represented.
fn rounded(n: usize) -> Result<usize, i32> {
    if n == 0 || n > usize::MAX - (PAGE_SIZE - 1) {
        return Err(EINVAL);
    }
    Ok((n + PAGE_SIZE - 1) & !(PAGE_SIZE - 1))
}

/// The first region containing `[p, p + n)`, or null.  The space must be open
/// and the caller must hold the lock.
unsafe fn find(s: *mut c_void, p: usize, n: usize) -> *mut c_void {
    if unsafe { a20_flat_is_closed(s) } != 0 {
        return core::ptr::null_mut();
    }
    let mut r = unsafe { a20_flat_head(s) };
    while !r.is_null() {
        let mut base = 0usize;
        let mut size = 0usize;
        let mut alloc: *mut c_void = core::ptr::null_mut();
        let mut pins = 0u32;
        unsafe { a20_flat_region_fields(r, &mut base, &mut size, &mut alloc, &mut pins) };
        if p >= base && p - base <= size && n <= size - (p - base) {
            return r;
        }
        r = unsafe { a20_flat_next(r) };
    }
    core::ptr::null_mut()
}

#[no_mangle]
pub unsafe extern "C" fn a20_flat_init(s: *mut c_void) {
    unsafe { a20_flat_init_space(s) };
}

/// Register a caller-owned region at a fixed address.
#[no_mangle]
pub unsafe extern "C" fn a20_flat_add_fixed(
    s: *mut c_void,
    r: *mut c_void,
    p: usize,
    n: usize,
) -> i32 {
    if s.is_null() || r.is_null() || p == 0 || n == 0 || n > usize::MAX - p {
        return EINVAL;
    }
    let flags = unsafe { a20_flat_lock(s) };
    let mut result = if unsafe { a20_flat_is_closed(s) } != 0 { EINVAL } else { 0 };
    let mut v = unsafe { a20_flat_head(s) };
    while result == 0 && !v.is_null() {
        let (mut base, mut size, mut alloc, mut pins) = (0usize, 0usize, core::ptr::null_mut(), 0u32);
        unsafe { a20_flat_region_fields(v, &mut base, &mut size, &mut alloc, &mut pins) };
        if v == r || (p < base + size && base < p + n) {
            result = EINVAL;
        }
        v = unsafe { a20_flat_next(v) };
    }
    if result == 0 {
        let head = unsafe { a20_flat_head(s) };
        unsafe { a20_flat_set_region(r, head, p, n, core::ptr::null_mut()) };
        unsafe { a20_flat_set_head(s, r) };
    }
    unsafe { a20_flat_unlock(s, flags) };
    result
}

/// Answer whether `[p, p + n)` is inside a region.  The caller holds the lock.
#[no_mangle]
pub unsafe extern "C" fn a20_flat_range_locked(s: *mut c_void, p: usize, n: usize) -> i32 {
    if s.is_null() {
        return 0;
    }
    i32::from(!unsafe { find(s, p, n) }.is_null())
}

/// Allocate a page-aligned range of at least `n` bytes.
#[no_mangle]
pub unsafe extern "C" fn a20_flat_alloc(s: *mut c_void, n: usize, out: *mut usize) -> i32 {
    if s.is_null() || out.is_null() {
        return EINVAL;
    }
    let size = match rounded(n) {
        Ok(size) => size,
        Err(e) => return e,
    };
    if size > usize::MAX - (PAGE_SIZE - 1) {
        return EINVAL;
    }
    /* Pre-flight under the lock, then allocate outside it: the allocator can
     * reclaim, and no lock may be held across that. */
    let preflight = unsafe { a20_flat_lock(s) };
    let closed = unsafe { a20_flat_is_closed(s) };
    unsafe { a20_flat_unlock(s, preflight) };
    if closed != 0 {
        return EINVAL;
    }

    let r = unsafe { a20_flat_region_alloc() };
    if r.is_null() {
        return ENOMEM;
    }
    let allocation = unsafe { a20_flat_backing_alloc(size) };
    if allocation.is_null() {
        unsafe { a20_flat_kfree(r) };
        return ENOMEM;
    }
    let base = ((allocation as usize) + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);

    let flags = unsafe { a20_flat_lock(s) };
    let mut invalid = unsafe { a20_flat_is_closed(s) } != 0;
    let mut v = unsafe { a20_flat_head(s) };
    while !invalid && !v.is_null() {
        let mut vbase = 0usize;
        let mut vsize = 0usize;
        let mut valloc: *mut c_void = core::ptr::null_mut();
        let mut vpins = 0u32;
        unsafe { a20_flat_region_fields(v, &mut vbase, &mut vsize, &mut valloc, &mut vpins) };
        if base < vbase + vsize && vbase < base + size {
            invalid = true;
        }
        v = unsafe { a20_flat_next(v) };
    }
    if invalid {
        unsafe { a20_flat_unlock(s, flags) };
        unsafe { a20_flat_kfree(allocation) };
        unsafe { a20_flat_kfree(r) };
        return EINVAL;
    }
    let head = unsafe { a20_flat_head(s) };
    unsafe { a20_flat_set_region(r, head, base, size, allocation) };
    unsafe { a20_flat_set_head(s, r) };
    unsafe { *out = base };
    unsafe { a20_flat_unlock(s, flags) };
    0
}

/// Release exactly the region registered at `p` with `n`.
#[no_mangle]
pub unsafe extern "C" fn a20_flat_unmap(s: *mut c_void, p: usize, n: usize) -> i32 {
    if s.is_null() || p == 0 || p & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }
    let size = match rounded(n) {
        Ok(size) => size,
        Err(e) => return e,
    };
    let flags = unsafe { a20_flat_lock(s) };
    let mut at = unsafe { a20_flat_head(s) };
    let mut prev: *mut c_void = core::ptr::null_mut();
    loop {
        if at.is_null() {
            break;
        }
        let (mut base, mut rsize, mut alloc, mut pins) = (0usize, 0usize, core::ptr::null_mut(), 0u32);
        unsafe { a20_flat_region_fields(at, &mut base, &mut rsize, &mut alloc, &mut pins) };
        if base == p && rsize == size {
            break;
        }
        prev = at;
        at = unsafe { a20_flat_next(at) };
    }
    let result = if unsafe { a20_flat_is_closed(s) } != 0 || at.is_null() {
        EINVAL
    } else {
        let mut abase = 0usize;
        let mut asize = 0usize;
        let mut alloc: *mut c_void = core::ptr::null_mut();
        let mut pins = 0u32;
        unsafe { a20_flat_region_fields(at, &mut abase, &mut asize, &mut alloc, &mut pins) };
        if alloc.is_null() {
            EINVAL
        } else if pins != 0 {
            EBUSY
        } else {
            0
        }
    };
    let mut detached: *mut c_void = core::ptr::null_mut();
    if result == 0 {
        let next = unsafe { a20_flat_next(at) };
        if prev.is_null() {
            unsafe { a20_flat_set_head(s, next) };
        } else {
            unsafe { a20_flat_set_next(prev, next) };
        }
        detached = at;
    }
    unsafe { a20_flat_unlock(s, flags) };
    if result == 0 {
        let mut dbase = 0usize;
        let mut dsize = 0usize;
        let mut alloc: *mut c_void = core::ptr::null_mut();
        let mut dpins = 0u32;
        unsafe { a20_flat_region_fields(detached, &mut dbase, &mut dsize, &mut alloc, &mut dpins) };
        unsafe { a20_flat_kfree(alloc) };
        unsafe { a20_flat_kfree(detached) };
    }
    result
}

/// Take a wait lease on `[p, p + n)`; null when the range is not usable.
#[no_mangle]
pub unsafe extern "C" fn a20_flat_pin(s: *mut c_void, p: usize, n: usize) -> *mut c_void {
    if s.is_null() {
        return core::ptr::null_mut();
    }
    let flags = unsafe { a20_flat_lock(s) };
    let r = unsafe { find(s, p, n) };
    let mut out = r;
    if !r.is_null() {
        let (mut base, mut rsize, mut alloc, mut pins) = (0usize, 0usize, core::ptr::null_mut(), 0u32);
        unsafe { a20_flat_region_fields(r, &mut base, &mut rsize, &mut alloc, &mut pins) };
        if pins == u32::MAX {
            out = core::ptr::null_mut();
        } else {
            unsafe { a20_flat_set_pins(r, pins + 1) };
        }
    }
    unsafe { a20_flat_unlock(s, flags) };
    out
}

#[no_mangle]
pub unsafe extern "C" fn a20_flat_unpin(s: *mut c_void, r: *mut c_void) {
    if r.is_null() {
        return;
    }
    let flags = unsafe { a20_flat_lock(s) };
    let (mut base, mut rsize, mut alloc, mut pins) = (0usize, 0usize, core::ptr::null_mut(), 0u32);
    unsafe { a20_flat_region_fields(r, &mut base, &mut rsize, &mut alloc, &mut pins) };
    if pins != 0 {
        unsafe { a20_flat_set_pins(r, pins - 1) };
    }
    unsafe { a20_flat_unlock(s, flags) };
}

/// Copy to or from a registered range, under the lock so the range cannot be
/// released between the check and the copy.
#[no_mangle]
pub unsafe extern "C" fn a20_flat_copy(
    s: *mut c_void,
    kernel: *mut c_void,
    user: *mut c_void,
    n: usize,
    to_user: i32,
) -> i32 {
    if s.is_null() || (n != 0 && kernel.is_null()) {
        return EFAULT;
    }
    let flags = unsafe { a20_flat_lock(s) };
    let result = if unsafe { find(s, user as usize, n) }.is_null() { EFAULT } else { 0 };
    if result == 0 && n != 0 {
        if to_user != 0 {
            unsafe { a20_flat_copy_bytes(user, kernel, n) };
        } else {
            unsafe { a20_flat_copy_bytes(kernel, user, n) };
        }
    }
    unsafe { a20_flat_unlock(s, flags) };
    result
}

/// Tear the space down.  Refuses while any range is pinned.
#[no_mangle]
pub unsafe extern "C" fn a20_flat_destroy(s: *mut c_void) -> i32 {
    if s.is_null() {
        return EINVAL;
    }
    let flags = unsafe { a20_flat_lock(s) };
    let mut r = unsafe { a20_flat_head(s) };
    while !r.is_null() {
        let (mut base, mut size, mut alloc, mut pins) = (0usize, 0usize, core::ptr::null_mut(), 0u32);
        unsafe { a20_flat_region_fields(r, &mut base, &mut size, &mut alloc, &mut pins) };
        if pins != 0 {
            unsafe { a20_flat_unlock(s, flags) };
            return EBUSY;
        }
        r = unsafe { a20_flat_next(r) };
    }
    let list = unsafe { a20_flat_head(s) };
    unsafe { a20_flat_set_head(s, core::ptr::null_mut()) };
    unsafe { a20_flat_set_closed(s) };
    unsafe { a20_flat_unlock(s, flags) };

    let mut at = list;
    while !at.is_null() {
        let (mut base, mut size, mut alloc, mut pins) = (0usize, 0usize, core::ptr::null_mut(), 0u32);
        unsafe { a20_flat_region_fields(at, &mut base, &mut size, &mut alloc, &mut pins) };
        let next = unsafe { a20_flat_next(at) };
        if !alloc.is_null() {
            unsafe { a20_flat_kfree(alloc) };
            unsafe { a20_flat_kfree(at) };
        }
        at = next;
    }
    0
}
