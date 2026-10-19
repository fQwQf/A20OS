#![no_std]

//! Fork / copy-on-write leaf cloning: the share-versus-copy transaction.
//!
//! `mm_fork()` walks the parent's page tables and, for every present leaf,
//! asks whether the child may share it or must get a private copy.  This
//! module owns that decision and the order of every step around it:
//!
//!   * a mapping the parent marked `VM_DONTFORK`/`VM_WIPEONFORK` is not cloned
//!     at all;
//!   * a swap entry is duplicated rather than materialised;
//!   * only the leaf base carries a clone (a huge leaf is cloned once, at its
//!     base), and a child that already has a present leaf there is left alone;
//!   * an architecture that requires a private copy at fork gets one: allocate
//!     the frame, copy the leaf, map the child, and free the frame if the map
//!     fails -- so a partial failure never leaves a child leaf pointing at a
//!     frame nobody owns;
//!   * otherwise the leaf is shared: take the parent's node lock, re-read the
//!     leaf under it (the snapshot and the reference it pins must be one step,
//!     or a racing COW can free the frame before it is retained), take the
//!     leaf's own reference -- the page-cache pin when the leaf is the
//!     canonical cache frame, a frame reference otherwise, and none at all for
//!     a VMO-owned leaf -- compute the COW flags, downgrade the parent when it
//!     was writable, map the child, and drop the reference again if that map
//!     fails.
//!
//! What stays C is the page-table walker and the architecture surface: the
//! leaf probe, the node lock and status sidecar, the PTE encoders,
//! `pt_map`/`pt_map_huge`, the frame allocator, the page-cache lookup and the
//! swap-entry duplication.

use core::ffi::c_void;

const VM_DONTFORK: u64 = 1 << 9;
const VM_WIPEONFORK: u64 = 1 << 10;

const EEXIST: i32 = -17;
const EAGAIN: i32 = -11;
const ENOMEM: i32 = -12;

const PFN_NONE: u32 = u32::MAX;

/// Everything the transaction needs to know about a source leaf.  Filled by
/// the C probe; `kind` records which reference the share snapshot took so the
/// failure path can give back exactly that one.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Src {
    pcp: *mut c_void,
    pte: usize,
    base: usize,
    size: usize,
    pa: usize,
    raw_flags: usize,
    cow_flags: usize,
    pfn: u32,
    level: i32,
    present: i32,
    swap: i32,
    is_leaf: i32,
    w_or_cow: i32,
    class: u32,
    kind: i32,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 88] = [(); core::mem::size_of::<Src>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 60] = [(); core::mem::size_of::<Src>()];

unsafe extern "C" {
    fn a20_cow_requires_private_copy() -> i32;
    fn a20_cow_rss_add(mm: *mut c_void, pages: usize);

    fn a20_cow_policy_flags(parent: *const c_void, va: usize, present: *mut i32) -> u64;
    fn a20_cow_probe(parent: *const c_void, va: usize, out: *mut Src);
    fn a20_cow_dst_present(child: *const c_void, va: usize) -> i32;
    fn a20_cow_clone_swap(child: *mut c_void, parent: *mut c_void, va: usize) -> i32;
    fn a20_cow_geometry(level: i32, size: *mut usize, pmd_size: *mut usize, pmd_order: *mut i32);
    fn a20_cow_frame_alloc(order: i32) -> u32;
    fn a20_cow_frame_free(pfn: u32, order: i32);
    fn a20_cow_frame_copy(dst: u32, src: u32, size: usize);
    fn a20_cow_map_child(
        child: *mut c_void,
        va: usize,
        pfn: u32,
        flags: usize,
        class: u32,
        level: i32,
    ) -> i32;
    fn a20_cow_share_snapshot(
        parent: *mut c_void,
        va: usize,
        level: i32,
        out: *mut Src,
    ) -> i32;
    fn a20_cow_share_downgrade(
        parent: *mut c_void,
        va: usize,
        level: i32,
        pa: usize,
        flags: usize,
        class: u32,
    );
    fn a20_cow_share_unlock(parent: *mut c_void, va: usize);
    fn a20_cow_release_ref(src: *const Src);
}

/// Clone one present leaf.  `level` is the leaf's page-table level, so a huge
/// leaf is copied or shared as a whole.
unsafe fn clone_leaf(
    child: *mut c_void,
    parent: *mut c_void,
    va: usize,
    level: i32,
    shared: i32,
) -> i32 {
    let mut src = Src::default();
    unsafe { a20_cow_probe(parent, va, &mut src) };
    if src.present == 0 || src.is_leaf == 0 {
        return 0;
    }

    let mut size = 0usize;
    let mut pmd_size = 0usize;
    let mut pmd_order = 0i32;
    unsafe { a20_cow_geometry(level, &mut size, &mut pmd_size, &mut pmd_order) };

    if shared == 0 && unsafe { a20_cow_requires_private_copy() } != 0 {
        let order = if size == pmd_size { pmd_order } else { 0 };
        if size != 4096 && size != pmd_size {
            return ENOMEM;
        }
        let copy = unsafe { a20_cow_frame_alloc(order) };
        if copy == PFN_NONE {
            return ENOMEM;
        }
        unsafe { a20_cow_frame_copy(copy, src.pfn, size) };
        let r = unsafe { a20_cow_map_child(child, va, copy, src.raw_flags, src.class, level) };
        if r < 0 {
            unsafe { a20_cow_frame_free(copy, order) };
            return r;
        }
        unsafe { a20_cow_rss_add(child, size / 4096) };
        return 0;
    }

    // Share.  The snapshot pins the leaf's reference while the node lock is
    // held, so the frame cannot be freed by a racing COW before it is ours.
    let mut snap = Src::default();
    if unsafe { a20_cow_share_snapshot(parent, va, level, &mut snap) } == 0 {
        return 0; // the leaf vanished or is no longer a leaf
    }
    let flags = if shared != 0 { snap.raw_flags } else { snap.cow_flags };
    if shared == 0 && snap.w_or_cow != 0 {
        unsafe {
            a20_cow_share_downgrade(parent, va, level, snap.pa, flags, snap.class)
        };
    }
    unsafe { a20_cow_share_unlock(parent, va) };

    let r = unsafe { a20_cow_map_child(child, va, snap.pfn, flags, snap.class, level) };
    if r < 0 {
        unsafe { a20_cow_release_ref(&snap) };
        return r;
    }
    unsafe { a20_cow_rss_add(child, size / 4096) };
    0
}

/// The per-address front half of fork's page-table clone.
#[no_mangle]
pub unsafe extern "C" fn a20_fork_clone_page_rust(
    child: *mut c_void,
    parent: *mut c_void,
    va: usize,
    shared: i32,
) -> i32 {
    let mut present = 0i32;
    let policy = unsafe { a20_cow_policy_flags(parent, va, &mut present) };
    if present != 0 && policy & (VM_DONTFORK | VM_WIPEONFORK) != 0 {
        return 0;
    }

    let mut src = Src::default();
    unsafe { a20_cow_probe(parent, va, &mut src) };
    if src.swap != 0 {
        let r = unsafe { a20_cow_clone_swap(child, parent, va) };
        return if r == EEXIST || r == EAGAIN { 0 } else { r };
    }
    if src.present == 0 || src.is_leaf == 0 {
        return 0;
    }
    if va != src.base {
        return 0;
    }
    if unsafe { a20_cow_dst_present(child, va) } != 0 {
        return 0;
    }
    unsafe { clone_leaf(child, parent, va, src.level, shared) }
}

/// Entry point for the C level walker, which already knows which leaf entry it
/// is looking at.
#[no_mangle]
pub unsafe extern "C" fn a20_cow_clone_leaf_rust(
    child: *mut c_void,
    parent: *mut c_void,
    va: usize,
    level: i32,
    shared: i32,
) -> i32 {
    unsafe { clone_leaf(child, parent, va, level, shared) }
}
