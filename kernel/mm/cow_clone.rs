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
const ANON_BATCH_PAGES: usize = 64;
const PAGE_SIZE: usize = 4096;

const PFN_NONE: u32 = u32::MAX;

/// Unlocked source-leaf probe. Kept separate from the pinned share snapshot:
/// a probe alone does not own either a frame reference or a node lock.
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

/// Pinned leaf and the exact node lock acquired by the C snapshot step.
/// Unlike the unlocked probe, these pointers stay valid under parent->lock
/// until the transaction explicitly drops the node lock before mapping child.
#[repr(C)]
#[derive(Default)]
struct SharedSnapshot {
    pcp: *mut c_void,
    slot: *mut c_void,
    node: *mut c_void,
    pa: usize,
    raw_flags: usize,
    cow_flags: usize,
    pfn: u32,
    w_or_cow: i32,
    class: u32,
    kind: i32,
}

/// Optional leaf location borrowed from the C walker while parent->lock is
/// held. Keeping the pair together prevents a partial hint from silently
/// falling back to a fresh root walk.
#[derive(Clone, Copy)]
struct LeafLocation {
    slot: *mut c_void,
    node: *mut c_void,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 64] = [(); core::mem::size_of::<SharedSnapshot>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 40] = [(); core::mem::size_of::<SharedSnapshot>()];

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
        pcp: *mut c_void,
    ) -> i32;
    fn a20_cow_share_snapshot(
        parent: *mut c_void,
        va: usize,
        level: i32,
        out: *mut SharedSnapshot,
    ) -> i32;
    fn a20_cow_share_snapshot_at_vma(
        parent: *mut c_void,
        va: usize,
        level: i32,
        slot: *mut c_void,
        node: *mut c_void,
        vma: *mut c_void,
        out: *mut SharedSnapshot,
    ) -> i32;
    fn a20_cow_share_downgrade(
        parent: *mut c_void,
        va: usize,
        level: i32,
        snapshot: *const SharedSnapshot,
        flags: usize,
    );
    fn a20_cow_share_unlock(snapshot: *const SharedSnapshot);
    fn a20_cow_release_ref(src: *const SharedSnapshot);
}

/* Must match kernel/mm/cow.c's bounded source snapshot record.  C has already
 * validated the backing class and computes the same flags as scalar clone. */
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct AnonBatchPage {
    pfn: u32,
    class: u32,
    flags: usize,
    pcp: *mut c_void,
    kind: u32,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); core::mem::size_of::<AnonBatchPage>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 20] = [(); core::mem::size_of::<AnonBatchPage>()];
const _: [(); 0] = [(); core::mem::offset_of!(AnonBatchPage, pfn)];
const _: [(); 4] = [(); core::mem::offset_of!(AnonBatchPage, class)];
const _: [(); 8] = [(); core::mem::offset_of!(AnonBatchPage, flags)];
#[cfg(target_pointer_width = "64")]
const _: [(); 16] = [(); core::mem::offset_of!(AnonBatchPage, pcp)];
#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); core::mem::offset_of!(AnonBatchPage, kind)];
#[cfg(target_pointer_width = "32")]
const _: [(); 12] = [(); core::mem::offset_of!(AnonBatchPage, pcp)];
#[cfg(target_pointer_width = "32")]
const _: [(); 16] = [(); core::mem::offset_of!(AnonBatchPage, kind)];

unsafe extern "C" {
    fn a20_cow_share_anon_batch_snapshot(
        parent: *mut c_void,
        child: *mut c_void,
        start: usize,
        node: *mut c_void,
        out: *mut AnonBatchPage,
        count: usize,
        shared: i32,
    ) -> i32;
    fn a20_cow_map_child_anon_batch(
        child: *mut c_void,
        va: usize,
        pages: *const AnonBatchPage,
        count: usize,
        shared: i32,
    ) -> i32;
    fn frame_put_many(pfns: *const u32, count: usize);
    fn page_cache_put(page: *mut c_void);
}

/* Owns the references pinned by the C snapshot until each child mapping
 * succeeds.  A failed map leaves its own pin and every later pin here; the
 * already-mapped prefix belongs to the child and is reclaimed by mm_destroy. */
struct AnonBatchGuard {
    pages: [AnonBatchPage; ANON_BATCH_PAGES],
    count: usize,
    pinned: bool,
    transferred: usize,
}

impl Drop for AnonBatchGuard {
    fn drop(&mut self) {
        if !self.pinned || self.transferred == self.count {
            return;
        }
        let mut pfns = [0u32; ANON_BATCH_PAGES];
        let mut frame_count = 0usize;
        let mut frame_out = pfns.iter_mut();
        for page in self.pages.iter().take(self.count).skip(self.transferred) {
            if page.kind == 2 {
                unsafe { page_cache_put(page.pcp) };
            } else if page.kind == 1 {
                if let Some(dst) = frame_out.next() {
                    *dst = page.pfn;
                    frame_count += 1;
                }
            }
        }
        if frame_count != 0 {
            unsafe { frame_put_many(pfns.as_ptr(), frame_count) };
        }
    }
}

/// Attempt one all-or-nothing shared or private source snapshot.  Zero means the C
/// preflight made no changes and the existing scalar walker should proceed;
/// one means every present child leaf in the window was mapped; negative aborts
/// this fork after the guard drains every reference not transferred to the child.
#[no_mangle]
pub unsafe extern "C" fn a20_cow_clone_anon_batch_rust(
    child: *mut c_void,
    parent: *mut c_void,
    va: usize,
    node: *mut c_void,
    count: usize,
    shared: i32,
) -> i32 {
    if count == 0 || count > ANON_BATCH_PAGES ||
        unsafe { a20_cow_requires_private_copy() } != 0 {
        return 0;
    }
    let mut guard = AnonBatchGuard {
        pages: [AnonBatchPage::default(); ANON_BATCH_PAGES],
        count,
        pinned: false,
        transferred: 0,
    };
    let snapshot = unsafe {
        a20_cow_share_anon_batch_snapshot(
            parent,
            child,
            va,
            node,
            guard.pages.as_mut_ptr(),
            count,
            shared,
        )
    };
    if snapshot <= 0 {
        return snapshot;
    }
    guard.pinned = true;

    let mapped = guard.pages.iter().take(count).filter(|page| page.class != 0).count();
    if mapped == 0 {
        // Holes and recoverable file leaves own no child references. Avoid
        // allocating destination page tables for an entirely omitted window.
        return 1;
    }

    let result = unsafe {
        a20_cow_map_child_anon_batch(
            child,
            va,
            guard.pages.as_ptr(),
            count,
            shared,
        )
    };
    if result < 0 {
        return result;
    }
    /* The C mapper validates the complete destination and has no failure
     * point after its first PTE write, so ownership and RSS move as one batch. */
    guard.transferred = count;
    unsafe { a20_cow_rss_add(child, mapped) };
    1
}

/// Clone one present leaf.  `level` is the leaf's page-table level, so a huge
/// leaf is copied or shared as a whole.
unsafe fn clone_leaf(
    child: *mut c_void,
    parent: *mut c_void,
    va: usize,
    level: i32,
    shared: i32,
    location: Option<LeafLocation>,
    vma: *mut c_void,
) -> i32 {
    let mut size = 0usize;
    let mut pmd_size = 0usize;
    let mut pmd_order = 0i32;
    unsafe { a20_cow_geometry(level, &mut size, &mut pmd_size, &mut pmd_order) };

    if shared == 0 && unsafe { a20_cow_requires_private_copy() } != 0 {
        // Allocation may reclaim memory. Pin the authoritative source leaf
        // before dropping its node lock, just as the sharing transaction does.
        let order = if size == pmd_size { pmd_order } else { 0 };
        if size != 4096 && size != pmd_size {
            return ENOMEM;
        }
        let mut src = SharedSnapshot::default();
        let pinned = match location {
            None => unsafe { a20_cow_share_snapshot(parent, va, level, &mut src) },
            Some(location) => unsafe {
                a20_cow_share_snapshot_at_vma(parent, va, level, location.slot,
                                              location.node, vma, &mut src)
            },
        };
        if pinned == 0 {
            return 0;
        }
        unsafe { a20_cow_share_unlock(&src) };
        let copy = unsafe { a20_cow_frame_alloc(order) };
        if copy == PFN_NONE {
            unsafe { a20_cow_release_ref(&src) };
            return ENOMEM;
        }
        unsafe { a20_cow_frame_copy(copy, src.pfn, size) };
        let r = unsafe {
            a20_cow_map_child(child, va, copy, src.raw_flags, src.class, level,
                              core::ptr::null_mut())
        };
        unsafe { a20_cow_release_ref(&src) };
        if r < 0 {
            unsafe { a20_cow_frame_free(copy, order) };
            return r;
        }
        unsafe { a20_cow_rss_add(child, size / 4096) };
        return 0;
    }

    // Share.  The snapshot pins the leaf's reference while the node lock is
    // held, so the frame cannot be freed by a racing COW before it is ours.
    let mut snap = SharedSnapshot::default();
    let snapshot_ok = match location {
        None => unsafe { a20_cow_share_snapshot(parent, va, level, &mut snap) },
        Some(location) => unsafe {
            a20_cow_share_snapshot_at_vma(
                parent,
                va,
                level,
                location.slot,
                location.node,
                vma,
                &mut snap,
            )
        },
    };
    if snapshot_ok == 0 {
        return 0; // absent leaf, or a cache-owned file page left for demand fault
    }
    let flags = if shared != 0 { snap.raw_flags } else { snap.cow_flags };
    if shared == 0 && snap.w_or_cow != 0 {
        unsafe {
            a20_cow_share_downgrade(parent, va, level, &snap, flags)
        };
    }
    unsafe { a20_cow_share_unlock(&snap) };

    let r = unsafe {
        a20_cow_map_child(child, va, snap.pfn, flags, snap.class, level, snap.pcp)
    };
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
    unsafe { clone_leaf(child, parent, va, src.level, shared, None,
                        core::ptr::null_mut()) }
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
    unsafe { clone_leaf(child, parent, va, level, shared, None,
                        core::ptr::null_mut()) }
}

/// Clone a leaf supplied by the C page-table walker. `slot` and `node` are
/// borrowed under the caller's parent->lock; the C snapshot takes the node
/// lock and revalidates the slot before pinning any backing reference.
#[no_mangle]
pub unsafe extern "C" fn a20_cow_clone_leaf_at_rust(
    child: *mut c_void,
    parent: *mut c_void,
    va: usize,
    level: i32,
    shared: i32,
    checked: i32,
    slot: *mut c_void,
    node: *mut c_void,
    vma: *mut c_void,
) -> i32 {
    if checked != 0 {
        let mut present = 0i32;
        let policy = unsafe { a20_cow_policy_flags(parent, va, &mut present) };
        if present != 0 && policy & (VM_DONTFORK | VM_WIPEONFORK) != 0 {
            return 0;
        }
        if unsafe { a20_cow_dst_present(child, va) } != 0 {
            return 0;
        }
    }
    unsafe {
        clone_leaf(
            child,
            parent,
            va,
            level,
            shared,
            Some(LeafLocation { slot, node }),
            vma,
        )
    }
}
