#![no_std]

//! The status-led COW break: the lockless fast path a private-anon write fault
//! takes before the `mm->lock` fallback is considered.
//!
//! The path is short and every branch is a decision:
//!
//!   * **Decline, and release exactly what was pinned.**  The cursor helper
//!     takes the leaf lock, verifies the PTE and the status class and pins the
//!     frame under `pfa.lock` before releasing the leaf lock.  A decline after
//!     that point has to give the pin back; a decline before it has nothing to
//!     give.  The class must still say COW, and a userfaultfd-marked page is
//!     left to the uffd path rather than copied here.
//!   * **Allocate without reclaim.**  `can_reclaim` is false for the same
//!     reason the anonymous fast path insists on it: a reclaiming allocation
//!     reaches `oom_try_reclaim()`, whose victim teardown wants the page-table
//!     node locks this path holds.
//!   * **Roll back the allocation and the charge together.**  The charge is
//!     taken after the frame is allocated and the frame is released before the
//!     charge on the way out, so no path can leave a charged frame behind or
//!     release a frame that was never charged.
//!   * **A lost compare-and-replace resolves the fault anyway.**  If the state
//!     moved underneath us -- fork rewrote it, another thread of this mm broke
//!     the COW first, the page was unmapped -- the pinned frame and the copy
//!     are both spare and the fault is over: the retried store either succeeds
//!     on the new state or re-faults into the `mm->lock` path.
//!   * **Defer the shootdown and both puts to the caller.**  Remote IPIs must
//!     not run while `mm->lock` is held, so the retired frame is reported back
//!     and the wrapper does the shootdown and the two `frame_put`s (the
//!     mapping's reference and the temporary snapshot pin) after unlocking.
//!
//! What stays C is the address-space cursor itself and the primitives it needs:
//! `mm_addrspace_lock` / `mm_cursor_unlock`, the leaf probe and pin, the
//! compare-and-replace, the physical frame allocator, the cgroup charge and the
//! performance counters.  The cursor lives in an opaque buffer whose size C
//! reports and asserts, so no `mm_cursor_t` layout is mirrored here.

use core::ffi::c_void;

const PFN_NONE: u32 = u32::MAX;

const MM_ST_COW_BIT: u8 = 0x08;
const MM_ST_ANON_MAPPED: u8 = 2;

/// Opaque storage for the C cursor plus this path's classification.  The size
/// is checked by `_Static_assert` on the C side; 16 words covers every active
/// architecture's `MM_CURSOR_PATH_MAX`.
const CTX_WORDS: usize = 16;

#[repr(C)]
struct Ctx {
    words: [u64; CTX_WORDS],
}

unsafe extern "C" {
    /// Take the address-space cursor over one page and pin the leaf's frame.
    /// Returns 1 with the facts filled, or 0 when the leaf is not there.
    fn a20_cowf_status_open(
        ctx: *mut Ctx,
        mm: *mut c_void,
        va: usize,
        cls_out: *mut u8,
        pa_out: *mut usize,
        uffd_out: *mut i32,
    ) -> i32;
    fn a20_cowf_status_replace(
        ctx: *mut Ctx,
        va: usize,
        expect_pa: usize,
        new_pa: usize,
        cls: u8,
        replaced_out: *mut usize,
    ) -> i32;
    fn a20_cowf_status_close(ctx: *mut Ctx);
    /// Non-reclaiming frame allocation: `pfa_alloc_flags(0, 0)`.
    fn a20_fault_status_alloc() -> u32;
    fn a20_cowf_status_pfn_of(pa: usize) -> u32;
    fn a20_cowf_status_pfn_valid(pfn: u32) -> i32;
    fn a20_fault_status_phys(pfn: u32) -> usize;
    fn a20_fault_status_charge(t: *mut c_void) -> i32;
    fn a20_fault_status_uncharge(t: *mut c_void);
    fn a20_cowf_status_copy(dst: u32, src: u32);
    fn a20_cowf_status_bump(t: *mut c_void);
    /// Existing MM symbols the break calls directly.
    fn frame_put(pfn: u32);
}

/// Serve a private-anon COW write fault from the status mark.
///
/// Returns 1 when the fault is resolved (and reports the frame whose shootdown
/// and two puts the caller owes), 0 to decline into the `mm->lock` path.
#[no_mangle]
pub unsafe extern "C" fn a20_cow_from_status_rust(
    t: *mut c_void,
    mm: *mut c_void,
    page_va: usize,
    retired_pfn: *mut u32,
) -> i32 {
    unsafe { *retired_pfn = PFN_NONE };

    let mut ctx = Ctx { words: [0; CTX_WORDS] };
    let mut cls_byte: u8 = 0;
    let mut old_pa: usize = 0;
    let mut uffd: i32 = 0;
    if unsafe {
        a20_cowf_status_open(&mut ctx, mm, page_va, &mut cls_byte, &mut old_pa, &mut uffd)
    } != 1
    {
        return 0;
    }

    /* The pin succeeded, so every decline past this point owes one frame_put
     * of the frame the snapshot pinned. */
    if cls_byte & MM_ST_COW_BIT == 0 || uffd != 0 {
        unsafe { a20_cowf_status_close(&mut ctx) };
        let pinned_pa_pfn = unsafe { a20_cowf_status_pfn_of(old_pa) };
        unsafe { frame_put(pinned_pa_pfn) };
        return 0;
    }

    let pinned_pfn = unsafe { a20_cowf_status_pfn_of(old_pa) };
    if unsafe { a20_cowf_status_pfn_valid(pinned_pfn) } == 0 {
        unsafe { a20_cowf_status_close(&mut ctx) };
        unsafe { frame_put(pinned_pfn) };
        return 0;
    }

    let new_pfn = unsafe { a20_fault_status_alloc() };
    if new_pfn == PFN_NONE {
        unsafe { a20_cowf_status_close(&mut ctx) };
        unsafe { frame_put(pinned_pfn) };
        return 0;
    }
    if unsafe { a20_fault_status_charge(t) } != 0 {
        unsafe { a20_cowf_status_close(&mut ctx) };
        unsafe { frame_put(new_pfn) };
        unsafe { frame_put(pinned_pfn) };
        return 0;
    }
    unsafe { a20_cowf_status_copy(new_pfn, pinned_pfn) };

    let mut replaced: usize = 0;
    let new_pa = unsafe { a20_fault_status_phys(new_pfn) };
    let r = unsafe {
        a20_cowf_status_replace(
            &mut ctx,
            page_va,
            old_pa,
            new_pa,
            MM_ST_ANON_MAPPED,
            &mut replaced,
        )
    };
    unsafe { a20_cowf_status_close(&mut ctx) };
    if r != 0 {
        unsafe { a20_fault_status_uncharge(t) };
        unsafe { frame_put(new_pfn) };
        unsafe { frame_put(pinned_pfn) };
        return 1;
    }

    unsafe { *retired_pfn = pinned_pfn };
    unsafe { a20_cowf_status_bump(t) };
    1
}
