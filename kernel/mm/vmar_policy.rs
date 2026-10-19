#![no_std]

//! The VMAR reservation engine.
//!
//! A VMAR is a reservation hierarchy only: page tables and VMAs stay in the
//! owning `mm_struct`.  What this file owns is the three rules the tree adds on
//! top of a plain range allocator, plus the ordering that makes them hold:
//!
//!   * **Containment.**  Every child range lies inside its parent, so a
//!     sub-allocation that would escape is refused before anything is created.
//!   * **Monotonic ceilings.**  A child's capability ceiling is
//!     `request & parent.cap`, so rights can only narrow down the tree.
//!   * **No overlap between siblings.**  The test and the insert are one atomic
//!     step under the parent's lock: two tasks must not both observe a free
//!     range and both insert.  Only two siblings can overlap a candidate --
//!     the last one starting below it and the first starting at or above it --
//!     because the child list is ordered and non-overlapping by construction,
//!     so the search stops there instead of comparing against every sibling.
//!
//! The release path has its own ordering invariant: a dying node unlinks itself
//! from its parent *under the parent's lock*, then drops the reference that
//! kept the parent alive *after* the lock is released, so a cascading teardown
//! cannot re-enter the same list.
//!
//! What stays C is the node's storage and the primitives it needs: `kmalloc`/
//! `kfree`, the spinlock and the refcount, and the list surgery itself (link,
//! unlink and the tail recompute).  The decisions above are not C's.

use core::ffi::c_void;

const PAGE_SIZE: u64 = 4096;

const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
const ENOSPC: i32 = -28;

const VMAR_CAN_MAP_READ: u32 = 0x01;
const VMAR_CAN_MAP_WRITE: u32 = 0x02;
const VMAR_CAN_MAP_EXEC: u32 = 0x04;
const VMAR_CAN_MAP_SPECIFIC: u32 = 0x08;

/// The node fields the decisions read.  Everything else about a `vmar_t`
/// (the lock, the refcount, the list links) belongs to the C bridges.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Geometry {
    base: u64,
    len: u64,
    cap: u32,
    pad: u32,
    parent: *mut c_void,
}

unsafe extern "C" {
    fn a20_vmar_user_va_limit() -> u64;
    /// Allocate a node with its constant fields set and the lock and refcount
    /// initialised; the caller's reference is already held.
    fn a20_vmar_alloc(base: u64, len: u64, cap: u32) -> *mut c_void;
    fn a20_vmar_set_parent(v: *mut c_void, parent: *mut c_void);
    fn a20_vmar_geometry(v: *const c_void, out: *mut Geometry);
    /// The first child starting at or above `base`, with `out_pred` set to the
    /// last child starting below it.  No overlap decision is made here.
    fn a20_vmar_locate(
        parent: *mut c_void,
        base: u64,
        out_pred: *mut *mut c_void,
    ) -> *mut c_void;
    fn a20_vmar_link(parent: *mut c_void, child: *mut c_void, pred: *mut c_void);
    fn a20_vmar_unlink(parent: *mut c_void, child: *mut c_void);
    fn a20_vmar_free(v: *mut c_void);
    fn a20_vmar_lock(parent: *mut c_void) -> u64;
    fn a20_vmar_unlock(parent: *mut c_void, flags: u64);
    fn a20_vmar_get(v: *mut c_void);
    /// Drop one reference; 1 when it was the last one.
    fn a20_vmar_put(v: *mut c_void) -> i32;
}

/// Two ranges overlap.  Half-open, so a range ending exactly where the next
/// begins is not an overlap.
fn ranges_overlap(a1: u64, l1: u64, a2: u64, l2: u64) -> bool {
    a1 < a2 + l2 && a2 < a1 + l1
}

fn cap_mask(cap: u32) -> u32 {
    cap & (VMAR_CAN_MAP_READ | VMAR_CAN_MAP_WRITE | VMAR_CAN_MAP_EXEC | VMAR_CAN_MAP_SPECIFIC)
}

/// Root VMAR over `[base, base + len)`: the whole range must sit inside the
/// user window and both ends must be page aligned.  NULL on a bad range or OOM.
#[no_mangle]
pub unsafe extern "C" fn vmar_create_root(
    _mm: *mut c_void,
    base: u64,
    len: u64,
    cap: u32,
) -> *mut c_void {
    let limit = unsafe { a20_vmar_user_va_limit() };
    if len == 0 || base >= limit || len > limit - base {
        return core::ptr::null_mut();
    }
    if base & (PAGE_SIZE - 1) != 0 || len & (PAGE_SIZE - 1) != 0 {
        return core::ptr::null_mut();
    }
    unsafe { a20_vmar_alloc(base, len, cap_mask(cap)) }
}

/// Sub-allocate `[base, base + len)` under `parent`.
///
/// The node is allocated outside the lock, because `kmalloc` can reclaim and no
/// lock may be held across that; it stays unpublished until the insert below,
/// which is where the no-overlap test and the insertion happen as one step.
#[no_mangle]
pub unsafe extern "C" fn vmar_create_child(
    parent: *mut c_void,
    base: u64,
    len: u64,
    cap_request: u32,
    out: *mut *mut c_void,
) -> i64 {
    if parent.is_null() || out.is_null() {
        return EINVAL as i64;
    }
    if len == 0 || base + len < base {
        return EINVAL as i64;
    }
    if base & (PAGE_SIZE - 1) != 0 || len & (PAGE_SIZE - 1) != 0 {
        return EINVAL as i64;
    }
    if unsafe { vmar_contains(parent, base, len) } == 0 {
        return ENOSPC as i64;
    }

    let mut pg = Geometry::default();
    unsafe { a20_vmar_geometry(parent, &mut pg) };
    let child = unsafe { a20_vmar_alloc(base, len, cap_mask(cap_request) & pg.cap) };
    if child.is_null() {
        return ENOMEM as i64;
    }
    unsafe { a20_vmar_set_parent(child, parent) };

    let flags = unsafe { a20_vmar_lock(parent) };
    let mut pred: *mut c_void = core::ptr::null_mut();
    let cur = unsafe { a20_vmar_locate(parent, base, &mut pred) };
    let mut overlap = false;
    if !pred.is_null() {
        let mut g = Geometry::default();
        unsafe { a20_vmar_geometry(pred, &mut g) };
        overlap = ranges_overlap(base, len, g.base, g.len);
    }
    if !overlap && !cur.is_null() {
        let mut g = Geometry::default();
        unsafe { a20_vmar_geometry(cur, &mut g) };
        overlap = ranges_overlap(base, len, g.base, g.len);
    }
    if overlap {
        unsafe { a20_vmar_unlock(parent, flags) };
        unsafe { a20_vmar_free(child) };
        return ENOSPC as i64;
    }
    unsafe { a20_vmar_link(parent, child, pred) };
    unsafe { a20_vmar_get(parent) }; /* the child keeps its parent alive */
    unsafe { a20_vmar_unlock(parent, flags) };

    unsafe { *out = child };
    0
}

/// Containment test used by map/protect routing: the whole range has to sit
/// inside the node, and the comparison is written so it cannot wrap.
#[no_mangle]
pub unsafe extern "C" fn vmar_contains(v: *const c_void, addr: u64, len: u64) -> i32 {
    if v.is_null() || len == 0 || addr + len < addr {
        return 0;
    }
    let mut g = Geometry::default();
    unsafe { a20_vmar_geometry(v, &mut g) };
    let inside = addr >= g.base && len <= g.len && addr - g.base <= g.len - len;
    if inside { 1 } else { 0 }
}

/// Ceiling query: every requested capability bit has to be granted by the node.
#[no_mangle]
pub unsafe extern "C" fn vmar_cap_allows(v: *const c_void, map_bits: u32) -> i32 {
    let mut g = Geometry::default();
    unsafe { a20_vmar_geometry(v, &mut g) };
    let granted = g.cap & (VMAR_CAN_MAP_READ | VMAR_CAN_MAP_WRITE | VMAR_CAN_MAP_EXEC);
    if map_bits & !granted == 0 { 1 } else { 0 }
}

#[no_mangle]
pub unsafe extern "C" fn vmar_acquire(v: *mut c_void) {
    if !v.is_null() {
        unsafe { a20_vmar_get(v) };
    }
}

/// Drop one reference and free the node at zero.
///
/// The unlink happens under the parent's lock; the parent's own reference is
/// dropped only after that lock is released, so a cascading teardown cannot
/// re-enter the list it just left.
#[no_mangle]
pub unsafe extern "C" fn vmar_release(v: *mut c_void) {
    if v.is_null() {
        return;
    }
    if unsafe { a20_vmar_put(v) } != 1 {
        return;
    }
    let mut g = Geometry::default();
    unsafe { a20_vmar_geometry(v, &mut g) };
    if !g.parent.is_null() {
        unsafe { a20_vmar_unlink(g.parent, v) };
        unsafe { vmar_release(g.parent) };
    }
    unsafe { a20_vmar_free(v) };
}
