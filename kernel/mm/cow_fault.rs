#![no_std]

//! The COW write-isolation transaction (the locked path).
//!
//! A store that lands on a COW leaf must give the faulting address space its
//! own copy of the page before the store is retried.  This module owns that
//! transaction:
//!
//!   * a leaf that is neither COW nor writable is not ours to break;
//!   * a private file leaf may still *be* the canonical page-cache frame, whose
//!     allocator refcount describes cache ownership rather than the number of
//!     user mappings, so it is copied out unconditionally -- `rc == 1` must
//!     never make it writable in place;
//!   * otherwise the physical-frame reference count decides: `rc == 1` means
//!     this mapping is the only holder and the leaf is upgraded in place, and
//!     `rc > 1` means a real copy is allocated and installed;
//!   * the PTE is rewritten **before** the old frame reference is dropped, so
//!     two tasks that both see `rc > 1` cannot drive the count to zero and
//!     free a page the other one still points at;
//!   * the in-place path rewrites the PTE while the frame allocator's lock is
//!     still held, for the same reason: a timer interrupt must not schedule a
//!     task that releases the same frame between the decision and the write;
//!   * every rewrite brings the per-PTE status back in step, because a stale
//!     status is what a later status-driven fault would install verbatim.
//!
//! What stays C is the architecture and allocator surface: the leaf probe and
//! its predicates, the PTE encoders and flag subsets, the status sidecar, the
//! frame allocator's lock, the copy, the diagnostic hooks and the local TLB
//! flush.

use core::ffi::c_void;

const PFN_NONE: u32 = u32::MAX;

const VM_SHARED: u64 = 1 << 3;

/// Leaf facts the transaction needs, decoded by the C probe.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Probe {
    slot: *mut c_void,
    pte: usize,
    base: usize,
    size: usize,
    pa: usize,
    pfn: u32,
    present: i32,
    is_leaf: i32,
    user: i32,
    cow: i32,
    wr: i32,
    pfn_valid: i32,
    order: i32,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 72] = [(); core::mem::size_of::<Probe>()];

/// The COW-break flag subset.
const KIND_COW: i32 = 0;

/// Diagnostic tags, mapped to the C strings beside the arch hook.
const TAG_CACHE_COPY: i32 = 0;
const TAG_COPY: i32 = 1;

unsafe extern "C" {
    fn a20_cowf_has_pgdir(mm: *const c_void) -> i32;
    fn a20_cowf_probe(mm: *const c_void, va: usize, out: *mut Probe);
    fn a20_cowf_vma(
        mm: *mut c_void,
        va: usize,
        vm_flags: *mut u64,
        present: *mut i32,
    ) -> *mut c_void;
    fn a20_cowf_diag_begin(
        t: *mut c_void,
        mm: *mut c_void,
        va: usize,
        base: usize,
        size: usize,
        snapshot_pte: usize,
    ) -> i32;
    fn a20_cowf_alloc_page() -> u32;
    fn a20_cowf_alloc_order(order: i32) -> u32;
    fn a20_cowf_copy(dst: u32, src: u32, size: usize);
    fn a20_cowf_pfa_lock() -> u64;
    fn a20_cowf_pfa_refs(pfn: u32) -> u32;
    fn a20_cowf_pfa_unlock(flags: u64);
    fn a20_cowf_zero_ref(t: *mut c_void, va: usize, pfn: u32, order: i32);
    /// Write the replacement, bring the status in step and drop the local TLB
    /// entry.  `kind` selects the flag subset.
    fn a20_cowf_install(
        mm: *mut c_void,
        va: usize,
        new_pfn: u32,
        snapshot_pte: usize,
        base: usize,
        size: usize,
        vma: *mut c_void,
        slot: *mut c_void,
        t: *mut c_void,
        diag: i32,
        tag: i32,
        kind: i32,
    );
    /// The in-place upgrade: the same rewrite, with the allocator lock still
    /// held, and the unlock and flush after it.
    fn a20_cowf_reuse_install(
        mm: *mut c_void,
        va: usize,
        old_pa: usize,
        snapshot_pte: usize,
        base: usize,
        size: usize,
        vma: *mut c_void,
        slot: *mut c_void,
        t: *mut c_void,
        diag: i32,
        pfa_flags: u64,
    );
    /// The "already writable, just add the dirty bit" branch.
    fn a20_cowf_write_d(
        mm: *mut c_void,
        va: usize,
        snapshot_pte: usize,
        base: usize,
        size: usize,
        vma: *mut c_void,
        slot: *mut c_void,
    );
    fn mm_file_cache_mapping_get(vma: *mut c_void, va: usize, pfn: u32) -> *mut c_void;
    fn page_cache_put(page: *mut c_void);
}

#[no_mangle]
pub unsafe extern "C" fn a20_cow_fault_locked_rust(
    t: *mut c_void,
    mm: *mut c_void,
    stval: usize,
    old_pfn_out: *mut u32,
    old_page_out: *mut *mut c_void,
) -> i32 {
    if !old_pfn_out.is_null() {
        unsafe { *old_pfn_out = PFN_NONE };
    }
    if !old_page_out.is_null() {
        unsafe { *old_page_out = core::ptr::null_mut() };
    }
    if mm.is_null() || unsafe { a20_cowf_has_pgdir(mm) } == 0 {
        return -1;
    }

    let mut p = Probe::default();
    unsafe { a20_cowf_probe(mm, stval, &mut p) };
    if p.present == 0 || p.is_leaf == 0 || p.user == 0 {
        return -1;
    }
    let diag = unsafe { a20_cowf_diag_begin(t, mm, stval, p.base, p.size, p.pte) };

    if p.cow != 0 {
        if p.pfn_valid == 0 {
            return -1;
        }
        // Resolved once: the cache branch and the status sync describe the
        // same mapping.
        let mut flags = 0u64;
        let mut present = 0i32;
        let vma = unsafe { a20_cowf_vma(mm, p.base, &mut flags, &mut present) };

        // A private file page may still be the canonical page-cache frame.
        let cache_page = if p.size == 4096 {
            unsafe { mm_file_cache_mapping_get(vma, p.base, p.pfn) }
        } else {
            core::ptr::null_mut()
        };
        if !cache_page.is_null() && flags & VM_SHARED == 0 {
            let new_pfn = unsafe { a20_cowf_alloc_page() };
            if new_pfn == PFN_NONE {
                unsafe { page_cache_put(cache_page) };
                return -1;
            }
            unsafe { a20_cowf_copy(new_pfn, p.pfn, 4096) };
            unsafe {
                a20_cowf_install(
                    mm, stval, new_pfn, p.pte, p.base, p.size, vma, p.slot, t, diag,
                    TAG_CACHE_COPY, KIND_COW,
                )
            };
            if old_page_out.is_null() {
                unsafe { page_cache_put(cache_page) };
            } else {
                unsafe { *old_page_out = cache_page };
            }
            return 0;
        }
        if !cache_page.is_null() {
            unsafe { page_cache_put(cache_page) };
        }

        // The refcount work and the reuse decision happen inside the allocator
        // lock.  In the reuse case the PTE is rewritten before the lock is
        // dropped, or a timer interrupt could schedule a task that releases the
        // same frame.
        let pfa_flags = unsafe { a20_cowf_pfa_lock() };
        let rc = unsafe { a20_cowf_pfa_refs(p.pfn) };
        if rc == 0 {
            unsafe { a20_cowf_pfa_unlock(pfa_flags) };
            unsafe { a20_cowf_zero_ref(t, stval, p.pfn, p.order) };
            return -1;
        }
        if rc > 1 {
            unsafe { a20_cowf_pfa_unlock(pfa_flags) };
            let new_pfn = unsafe { a20_cowf_alloc_order(p.order) };
            if new_pfn == PFN_NONE {
                return -1;
            }
            unsafe { a20_cowf_copy(new_pfn, p.pfn, p.size) };
            // Install before the old reference is released: see above.
            unsafe {
                a20_cowf_install(
                    mm, stval, new_pfn, p.pte, p.base, p.size, vma, p.slot, t, diag, TAG_COPY,
                    KIND_COW,
                )
            };
            if !old_pfn_out.is_null() {
                unsafe { *old_pfn_out = p.pfn };
            }
            return 0;
        }
        unsafe {
            a20_cowf_reuse_install(
                mm, stval, p.pa, p.pte, p.base, p.size, vma, p.slot, t, diag, pfa_flags,
            )
        };
        return 0;
    }

    if p.wr != 0 {
        let mut vm_flags = 0u64;
        let mut vma_present = 0i32;
        let vma = unsafe { a20_cowf_vma(mm, p.base, &mut vm_flags, &mut vma_present) };
        let _ = (vm_flags, vma_present);
        unsafe { a20_cowf_write_d(mm, stval, p.pte, p.base, p.size, vma, p.slot) };
        return 0;
    }

    -1
}
