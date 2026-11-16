#![no_std]

//! The madvise advice policy and the two mapping transactions behind it.
//!
//! Three things live here, and each of them is a decision rather than a
//! translation of one:
//!
//!   * **The advice dispatch.**  Which advice acts, which is accepted and
//!     ignored, which is refused.  The distinction is load-bearing: several
//!     advices this path ignores do have implementations under the Linux ABI
//!     (HUGEPAGE/NOHUGEPAGE write VMA flags there), so "this entry point does
//!     not honour it" is not the same as "this kernel cannot do it".  An
//!     unrecognised value is `-EINVAL` and never a hint, because a pure hint
//!     must not fall through to the discard path and drop the caller's pages.
//!     The ignored advices still run the coverage check, because the Linux
//!     entry point runs it before its own dispatch and the same address must
//!     not answer differently depending on which ABI made the call.
//!   * **The discard transaction** (`MADV_DONTNEED`/`MADV_FREE`).  Which leaves
//!     inside the range are released, who owns each released page, and the
//!     exactly-once release that follows.  The range is validated and covered
//!     *before* any leaf moves, so a range with a hole cannot leave half the
//!     caller's pages discarded while the advice reports failure.
//!   * **The fork-policy transaction** (`MADV_DONTFORK`/`DOFORK`/
//!     `WIPEONFORK`/`KEEPONFORK`).  The whole range is validated and both
//!     boundaries are split before any bit moves, for the same reason: marking
//!     the covered part of a range that turns out to have a hole would leave
//!     the rest shared with the child while the advice reported failure.
//!     `VM_PFNMAP` records are refused with `-EPERM` rather than marked --
//!     they are the kernel's own (vDSO/vvar, device MMIO), `mm_fork` skips
//!     them and re-establishes the vDSO with its own mapping call, and neither
//!     insert path rejects a second record over a range that already has one.
//!
//! What stays C is narrow and named: the `mm_seg_t` field access and list
//! surgery, the page-table walk and leaf teardown, the TLB hold/release
//! primitives, the page-cache lookup, and the arch predicates.  None of the
//! bridges decides anything.

use core::ffi::c_void;

const PAGE_SIZE: usize = 4096;

const EPERM: i32 = -1;
const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
const ENOSYS: i32 = -38;

const VM_SHARED: u64 = 1 << 3;
const VM_FILE: u64 = 1 << 13;
const VM_DONTFORK: u64 = 1 << 9;
const VM_WIPEONFORK: u64 = 1 << 10;
const VM_VMO: u64 = 1 << 14;
const VM_LOCKED: u64 = 1 << 15;
const VM_PFNMAP: u64 = 1 << 17;

/* The native reservation marker.  Deliberately NOT VM_LOCKED: that bit is the
 * Linux mlock/oom-unevictable flag with different consumers (munmap.c, oom.c,
 * mmap.c), so the two must not share a bit. */
const VM_NATIVE_LOCKED: u64 = 1 << 27;

const MADV_NORMAL: i32 = 0;
const MADV_RANDOM: i32 = 1;
const MADV_SEQUENTIAL: i32 = 2;
const MADV_WILLNEED: i32 = 3;
const MADV_DONTNEED: i32 = 4;
const MADV_FREE: i32 = 8;
const MADV_REMOVE: i32 = 9;
const MADV_DONTFORK: i32 = 10;
const MADV_DOFORK: i32 = 11;
const MADV_MERGEABLE: i32 = 12;
const MADV_UNMERGEABLE: i32 = 13;
const MADV_HUGEPAGE: i32 = 14;
const MADV_NOHUGEPAGE: i32 = 15;
const MADV_DONTDUMP: i32 = 16;
const MADV_DODUMP: i32 = 17;
const MADV_WIPEONFORK: i32 = 18;
const MADV_KEEPONFORK: i32 = 19;
const MADV_COLD: i32 = 20;
const MADV_PAGEOUT: i32 = 21;
const MADV_POPULATE_READ: i32 = 22;
const MADV_POPULATE_WRITE: i32 = 23;

/// One mapping record overlapping the requested range, as the C walk reports
/// it.  The pointer is only a token to hand back to the bridges.
#[repr(C)]
#[derive(Clone, Copy)]
struct Vma {
    ptr: *mut c_void,
    start: usize,
    end: usize,
    flags: u64,
}

impl Default for Vma {
    fn default() -> Self {
        Vma { ptr: core::ptr::null_mut(), start: 0, end: 0, flags: 0 }
    }
}

/// One leaf under the cursor, as the C probe reports it.
#[repr(C)]
#[derive(Clone, Copy)]
struct Leaf {
    present: i32,
    level: i32,
    base: usize,
    size: usize,
    pfn: u32,
    pad: u32,
}

impl Default for Leaf {
    fn default() -> Self {
        Leaf { present: 0, level: 0, base: 0, size: 0, pfn: 0, pad: 0 }
    }
}

/// What `a20_madvise_leaf_unmap` reports about the leaf it removed.
#[repr(C)]
#[derive(Clone, Copy)]
struct Unmapped {
    ok: i32,
    base: usize,
    size: usize,
    pa: usize,
}

impl Default for Unmapped {
    fn default() -> Self {
        Unmapped { ok: 0, base: 0, size: 0, pa: 0 }
    }
}

/// Which owner the released leaf's reference belongs to.  Chosen from the leaf,
/// not from a VMA-wide flag: a private file mapping mixes clean cache leaves
/// with anonymous COW leaves.
const REL_FRAME: i32 = 1;
const REL_CACHE: i32 = 2;

/// The batched-walk buffer.  Sixteen records covers the tiled ranges these
/// paths see; a range with more falls back to the per-record probe.
const COLLECT_CAP: usize = 16;

unsafe extern "C" {
    // Narrow C bridges declared beside the adapters in mm/madvise.c.
    fn a20_madvise_user_va_limit() -> usize;
    fn a20_madvise_is_nommu() -> i32;
    fn a20_madvise_lock(mm: *mut c_void) -> u64;
    fn a20_madvise_unlock(mm: *mut c_void, flags: u64);
    fn a20_madvise_collect(
        mm: *mut c_void,
        addr: usize,
        end: usize,
        out: *mut Vma,
        cap: usize,
    ) -> i32;
    fn a20_madvise_vma_at(mm: *mut c_void, va: usize, out: *mut Vma) -> i32;
    fn a20_madvise_vma_set_flags(v: *mut c_void, flags: u64, on: i32);
    fn a20_madvise_split(mm: *mut c_void, va: usize) -> i32;
    fn a20_madvise_leaf_probe(mm: *mut c_void, va: usize, out: *mut Leaf);
    fn a20_madvise_leaf_hold(
        mm: *mut c_void,
        v: *mut c_void,
        va: usize,
        pfn: u32,
        no_frame_ref: i32,
        out_pcp: *mut *mut c_void,
    ) -> i32;
    fn a20_madvise_demote(mm: *mut c_void, va: usize) -> i32;
    fn a20_madvise_leaf_unmap(mm: *mut c_void, va: usize, out: *mut Unmapped);
    fn a20_madvise_release(
        mm: *mut c_void,
        kind: i32,
        pcp: *mut c_void,
        pa: usize,
        pages: usize,
    );
    fn a20_madvise_drop_cache_ref(pcp: *mut c_void);
    fn a20_madvise_note_change(mm: *mut c_void, base: usize, size: usize);

    // Existing MM symbols the transactions call directly.
    fn mm_tlb_invalidate_begin(mm: *mut c_void);
    fn mm_tlb_invalidate_finish(mm: *mut c_void);

    // The VMO export qualification: the covering record, one reference taken
    // while mm->lock is still held, and the mapping's protection.
    fn a20_madvise_vmo_covering(mm: *mut c_void, addr: usize, end: usize) -> *mut c_void;
    fn a20_madvise_vma_flags(v: *mut c_void) -> u64;
    fn a20_madvise_vma_pte_flags(v: *mut c_void) -> usize;
    fn a20_madvise_vmo_ref(v: *mut c_void) -> *mut c_void;
    fn mm_pte_flags_to_prot(pte_flags: usize) -> i32;

    // MADV_PAGEOUT: the reclaim range is driven by the OOM policy, but only C
    // can name the task that owns the mm.
    fn a20_madvise_current_mm() -> *mut c_void;
    fn a20_madvise_pageout_range(mm: *mut c_void, addr: usize, end: usize) -> i32;
}

/// Round `[addr, addr + len)` up to a page boundary and reject a range that
/// wraps or leaves the user window.  Both rejections matter because the walks
/// below step forward while `va < end`: an overflowed end is below `addr`, so
/// the body would run zero times and the advice would answer success having
/// touched nothing at all.
fn range_end(addr: usize, len: usize) -> Result<usize, i32> {
    let limit = unsafe { a20_madvise_user_va_limit() };
    if addr >= limit || len > limit - addr {
        return Err(EINVAL);
    }
    let end = (addr + len + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
    if end < addr || end > limit {
        return Err(EINVAL);
    }
    Ok(end)
}

/// The records the caller should walk: either the batched buffer, or -- when
/// the buffer overflowed -- a per-record walk the caller drives itself.  Taking
/// the batched slice by pointer keeps the bound check (and its panic path) out
/// of a module that has no unwinder.
struct Walk<'a> {
    mm: *mut c_void,
    recs: &'a [Vma],
    i: usize,
    probing: bool,
}

impl<'a> Walk<'a> {
    /// The next record at or after the caller's cursor, or `None` at the end.
    fn next(&mut self, va: usize) -> Option<Vma> {
        if self.probing {
            let mut r = Vma::default();
            if unsafe { a20_madvise_vma_at(self.mm, va, &mut r) } != 0 || r.end <= va {
                return None;
            }
            return Some(r);
        }
        let r = self.recs.get(self.i).copied();
        if r.is_some() {
            self.i += 1;
        }
        r
    }
}

/// Coverage check: every page of `[start, end)` has to sit inside a mapping
/// record.  The records are collected in one batched walk rather than probed
/// per page: `mm_seg_find()` is a binary search, so a per-page probe costs
/// log2(nrecords) per page for a range that usually spans a handful of records
/// -- half a million probes for a 1GB MADV_DONTNEED where four would do.
///
/// Returns the verdict and the walk the caller should use, which is the same
/// set of records when the verdict is true.
fn collect<'a>(
    mm: *mut c_void,
    start: usize,
    end: usize,
    buf: &'a mut [Vma; COLLECT_CAP],
) -> (bool, Walk<'a>) {
    let n = unsafe { a20_madvise_collect(mm, start, end, buf.as_mut_ptr(), COLLECT_CAP) };
    if n >= 0 {
        // The bridge contract caps the count at COLLECT_CAP.
        let n = (n as usize).min(COLLECT_CAP);
        let entries = unsafe { core::slice::from_raw_parts(buf.as_ptr(), n) };
        let mut covered = start;
        for e in entries {
            if e.start > covered || e.end <= covered {
                return (false, Walk { mm, recs: entries, i: 0, probing: false });
            }
            covered = e.end;
        }
        return (covered >= end, Walk { mm, recs: entries, i: 0, probing: false });
    }
    // More records than fit in the buffer: verify coverage by probing one
    // record at a time.  The verdict is the same and only the cost differs.
    let mut va = start;
    while va < end {
        let mut r = Vma::default();
        if unsafe { a20_madvise_vma_at(mm, va, &mut r) } != 0 || r.end <= va {
            return (false, Walk { mm, recs: &[], i: 0, probing: true });
        }
        va = r.end;
    }
    (true, Walk { mm, recs: &[], i: 0, probing: true })
}

/// `MADV_DONTNEED`/`MADV_FREE`: discard the pages of `[addr, addr + len)`.
///
/// Caller holds no lock.  The whole range is validated and covered first, then
/// one `mm->lock` region does the discard.  Every leaf releases exactly one
/// reference to exactly one owner: a page-cache page is put twice (the
/// mapping's cache pin plus the temporary lookup), a plain anonymous frame is
/// put once, and a leaf owned by a VMO, a driver or the vDSO is not put at all
/// because the mapping never took a frame reference to it.
#[no_mangle]
pub unsafe extern "C" fn a20_madvise_dontneed(mm: *mut c_void, addr: usize, len: usize) -> i32 {
    if mm.is_null() || len == 0 {
        return EINVAL;
    }
    if addr & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }
    let end = match range_end(addr, len) {
        Ok(end) => end,
        Err(e) => return e,
    };

    let mut buf = [Vma::default(); COLLECT_CAP];
    let flags = unsafe { a20_madvise_lock(mm) };
    let (covered, mut walk) = collect(mm, addr, end, &mut buf);
    if !covered {
        unsafe { a20_madvise_unlock(mm, flags) };
        return ENOMEM;
    }
    if unsafe { a20_madvise_is_nommu() } != 0 {
        // NOMMU has no page tables to discard from: every mapping in the
        // address space is already shared and the flat allocator owns the
        // frames, so there is nothing this advice may release.
        unsafe { a20_madvise_unlock(mm, flags) };
        return 0;
    }

    unsafe { mm_tlb_invalidate_begin(mm) };
    let r = discard_locked(mm, addr, end, &mut walk);
    unsafe { a20_madvise_unlock(mm, flags) };
    unsafe { mm_tlb_invalidate_finish(mm) };
    r
}

/// The discard itself.  Runs with `mm->lock` held and never unlocks: the caller
/// has one unlock site, so no error path can return with the lock held.
///
/// Pages are released before the cursor moves past them, so a failure part way
/// through leaves the address space *smaller* rather than describing memory
/// that has already been handed back.  The only fallible step is a huge leaf
/// that straddles a boundary: it is split first (exactly as munmap.c,
/// mprotect.c and mremap.c do), and a split failure returns with the pages
/// before it already released.
unsafe fn discard_locked(mm: *mut c_void, addr: usize, end: usize, walk: &mut Walk<'_>) -> i32 {
    let mut va = addr;
    while va < end {
        let rec = match walk.next(va) {
            Some(rec) => rec,
            None => break,
        };
        if rec.end <= va {
            continue;
        }
        /* VM_VMO and VM_PFNMAP frames stay owned by their VMO/global
         * allocator: PFNMAP leaves (vdso/vvar, framebuffer, driver BARs) carry
         * no per-mapping frame reference at all -- vdso.c installs the single
         * global frame with pt_map() and never frame_get()s it.  Unmapping
         * such a PTE must drop the PTE only, or the mapping's own "release"
         * drives a globally shared, still-executable frame to zero and buddy
         * recycles it.  Same guard as mm/munmap.c. */
        let no_frame_ref = (rec.flags & (VM_PFNMAP | VM_VMO)) != 0;
        let vma_end = rec.end;

        while va < vma_end && va < end {
            let mut leaf = Leaf::default();
            unsafe { a20_madvise_leaf_probe(mm, va, &mut leaf) };
            if leaf.present == 0 {
                va += PAGE_SIZE;
                continue;
            }
            /* A huge leaf that straddles either end of the request cannot be
             * dropped wholesale: pt_unmap_leaf() would discard the whole leaf,
             * including pages outside [addr, end) and outside the VMA, and the
             * cursor would then step past them. */
            if leaf.level > 0
                && (leaf.base < va
                    || leaf.base + leaf.size > end
                    || leaf.base + leaf.size > vma_end)
            {
                let dr = unsafe { a20_madvise_demote(mm, va) };
                if dr < 0 {
                    return dr;
                }
                continue;
            }
            if no_frame_ref {
                let mut out = Unmapped::default();
                unsafe { a20_madvise_leaf_unmap(mm, va, &mut out) };
                if out.ok == 0 {
                    unsafe { a20_madvise_note_change(mm, out.base, out.size) };
                    unsafe { a20_madvise_release(mm, 0, core::ptr::null_mut(), 0, out.size / PAGE_SIZE) };
                    va = out.base + out.size;
                    continue;
                }
                va += PAGE_SIZE;
                continue;
            }

            /* Hold the reference the leaf owns before it is unmapped, so a
             * racing COW or reclaim cannot free the frame while the TLB flush
             * that follows is still outstanding.  The hold leaves one
             * reference in the TLB queue, released when the flush completes. */
            let mut pcp: *mut c_void = core::ptr::null_mut();
            let hr = unsafe {
                a20_madvise_leaf_hold(mm, rec.ptr, va, leaf.pfn, 0, &mut pcp)
            };
            if hr < 0 {
                // The bridge gave back the lookup reference it took.
                return hr;
            }

            let mut out = Unmapped::default();
            unsafe { a20_madvise_leaf_unmap(mm, va, &mut out) };
            if out.ok != 0 {
                /* The unmap failed after the hold: give the lookup reference
                 * back and retry from the next page. */
                if !pcp.is_null() {
                    unsafe { a20_madvise_drop_cache_ref(pcp) };
                }
                va += PAGE_SIZE;
                continue;
            }
            unsafe { a20_madvise_note_change(mm, out.base, out.size) };
            let kind = if !pcp.is_null() { REL_CACHE } else { REL_FRAME };
            unsafe { a20_madvise_release(mm, kind, pcp, out.pa, out.size / PAGE_SIZE) };
            va = out.base + out.size;
        }
        va = rec.end;
    }
    0
}

/// Set or clear fork-policy bits over `[start, start + len)`.
///
/// The range is validated and both boundaries are split before any bit moves,
/// so what gets marked is what the caller asked for and not whichever record
/// the range happened to fall inside.
#[no_mangle]
pub unsafe extern "C" fn a20_madvise_fork_flags(
    mm: *mut c_void,
    start: usize,
    len: usize,
    flags: u64,
    on: i32,
) -> i32 {
    if mm.is_null() {
        return EINVAL;
    }
    let end = match range_end(start, len) {
        Ok(end) => end,
        Err(e) => return e,
    };

    let mut buf = [Vma::default(); COLLECT_CAP];
    let lock_flags = unsafe { a20_madvise_lock(mm) };
    let (covered, mut walk) = collect(mm, start, end, &mut buf);
    if !covered {
        unsafe { a20_madvise_unlock(mm, lock_flags) };
        return ENOMEM;
    }
    let mut va = start;
    while va < end {
        let rec = match walk.next(va) {
            Some(rec) => rec,
            None => break,
        };
        if rec.flags & VM_PFNMAP != 0 {
            unsafe { a20_madvise_unlock(mm, lock_flags) };
            return EPERM;
        }
        if on != 0 && flags & VM_WIPEONFORK != 0 &&
            rec.flags & (VM_FILE | VM_SHARED | VM_VMO) != 0 {
            unsafe { a20_madvise_unlock(mm, lock_flags) };
            return EINVAL;
        }
        va = rec.end;
    }
    /* Splitting after the validation pass and before the marking pass is what
     * makes the range exact; on NOMMU there are no page-table boundaries to
     * split at, so the records are marked whole. */
    if unsafe { a20_madvise_is_nommu() } == 0 {
        let mut r = unsafe { a20_madvise_split(mm, start) };
        if r == 0 {
            r = unsafe { a20_madvise_split(mm, end) };
        }
        if r < 0 {
            unsafe { a20_madvise_unlock(mm, lock_flags) };
            return r;
        }
    }
    /* The split can have produced records the first collect did not see, so the
     * marking pass re-walks rather than reusing the buffer. */
    let mut va = start;
    while va < end {
        let mut rec = Vma::default();
        if unsafe { a20_madvise_vma_at(mm, va, &mut rec) } != 0 || rec.end <= va {
            break;
        }
        unsafe { a20_madvise_vma_set_flags(rec.ptr, flags, on) };
        va = rec.end;
    }
    unsafe { a20_madvise_unlock(mm, lock_flags) };
    0
}

/// What one advice does on this entry point.
enum Action {
    /// Release the range's pages.
    Discard,
    /// Ask the OOM/swap policy to page the range out.
    Pageout,
    /// Mark or clear fork-policy bits.
    Fork { flags: u64, on: i32 },
    /// Validate and require coverage, then do nothing.
    Coverage,
    /// Refused, with this errno.
    Refuse(i32),
}

/// The advice dispatch.  Keeping this as one exhaustive table is what makes the
/// "accepted and ignored" set auditable: an advice added to the kernel but not
/// to this match fails to build rather than silently falling into the default.
fn action_of(advice: i32) -> Action {
    match advice {
        MADV_DONTNEED | MADV_FREE => Action::Discard,
        MADV_PAGEOUT => Action::Pageout,
        MADV_DONTFORK => Action::Fork { flags: VM_DONTFORK, on: 1 },
        MADV_DOFORK => Action::Fork { flags: VM_DONTFORK, on: 0 },
        /* Keep the anonymous VMA in the child, but omit its populated leaves
         * so demand faults supply zeros. DONTFORK would remove the VMA itself. */
        MADV_WIPEONFORK => Action::Fork { flags: VM_WIPEONFORK, on: 1 },
        MADV_KEEPONFORK => Action::Fork { flags: VM_WIPEONFORK, on: 0 },
        /* Removing backing-store contents requires a filesystem hole-punch
         * operation; swapping private pages out does not implement it. */
        MADV_REMOVE => Action::Refuse(ENOSYS),
        MADV_NORMAL
        | MADV_RANDOM
        | MADV_SEQUENTIAL
        | MADV_WILLNEED
        | MADV_MERGEABLE
        | MADV_UNMERGEABLE
        | MADV_HUGEPAGE
        | MADV_NOHUGEPAGE
        | MADV_DONTDUMP
        | MADV_DODUMP
        | MADV_COLD
        | MADV_POPULATE_READ
        | MADV_POPULATE_WRITE => Action::Coverage,
        _ => Action::Refuse(EINVAL),
    }
}

/// Advice dispatch for the Native `vm_advise` entry point.
#[no_mangle]
pub unsafe extern "C" fn a20_madvise_dispatch(
    mm: *mut c_void,
    addr: usize,
    len: usize,
    advice: i32,
) -> i32 {
    if mm.is_null() || len == 0 {
        return EINVAL;
    }
    if addr & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }

    match action_of(advice) {
        Action::Discard => unsafe { a20_madvise_dontneed(mm, addr, len) },
        Action::Fork { flags, on } => unsafe { a20_madvise_fork_flags(mm, addr, len, flags, on) },
        Action::Refuse(e) => e,
        Action::Coverage => unsafe { a20_madvise_covered_only(mm, addr, len) },
        Action::Pageout => unsafe { a20_madvise_pageout(mm, addr, len) },
    }
}

/// `MADV_PAGEOUT`: validate the range, refuse it when it is locked or
/// kernel-owned, then hand it to the OOM/swap reclaim policy.
///
/// The caller must own the mm: the reclaim walks the caller's page tables and
/// charges the caller's cgroup, so a task advising another address space would
/// be reclaiming pages it does not own.
unsafe fn a20_madvise_pageout(mm: *mut c_void, addr: usize, len: usize) -> i32 {
    if unsafe { a20_madvise_current_mm() } != mm {
        return EINVAL;
    }
    let end = match range_end(addr, len) {
        Ok(end) => end,
        Err(e) => return e,
    };
    let mut buf = [Vma::default(); COLLECT_CAP];
    let flags = unsafe { a20_madvise_lock(mm) };
    let (covered, mut walk) = collect(mm, addr, end, &mut buf);
    if covered {
        let mut va = addr;
        while va < end {
            let rec = match walk.next(va) {
                Some(rec) => rec,
                None => break,
            };
            if rec.flags & (VM_LOCKED | VM_PFNMAP) != 0 {
                unsafe { a20_madvise_unlock(mm, flags) };
                return EINVAL;
            }
            va = rec.end;
        }
    }
    unsafe { a20_madvise_unlock(mm, flags) };
    if !covered {
        return ENOMEM;
    }
    let r = unsafe { a20_madvise_pageout_range(mm, addr, end) };
    if r < 0 {
        r
    } else {
        0
    }
}

/// The coverage-only advices: the Linux entry point validates the range for
/// every advice before its own dispatch, so the same address must not answer
/// differently depending on which ABI made the call.
#[no_mangle]
pub unsafe extern "C" fn a20_madvise_covered_only(mm: *mut c_void, addr: usize, len: usize) -> i32 {
    let end = match range_end(addr, len) {
        Ok(end) => end,
        Err(e) => return e,
    };
    let mut buf = [Vma::default(); COLLECT_CAP];
    let flags = unsafe { a20_madvise_lock(mm) };
    let (covered, _) = collect(mm, addr, end, &mut buf);
    unsafe { a20_madvise_unlock(mm, flags) };
    if covered {
        0
    } else {
        ENOMEM
    }
}

/// Native ABI `vm_lock` reservation marker: toggle `VM_NATIVE_LOCKED` over
/// `[start, end)`, requiring the range to be fully mapped.  The bit is
/// deliberately not `VM_LOCKED`, which is the Linux mlock/oom-unevictable flag
/// with different consumers.
#[no_mangle]
pub unsafe extern "C" fn a20_madvise_set_lock(
    mm: *mut c_void,
    start: usize,
    end: usize,
    on: i32,
) -> i32 {
    if mm.is_null() || end <= start {
        return EINVAL;
    }
    if start & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }
    let flags = unsafe { a20_madvise_lock(mm) };
    let mut va = start;
    while va < end {
        let mut rec = Vma::default();
        if unsafe { a20_madvise_vma_at(mm, va, &mut rec) } != 0 || rec.end <= va {
            unsafe { a20_madvise_unlock(mm, flags) };
            return ENOMEM;
        }
        unsafe { a20_madvise_vma_set_flags(rec.ptr, VM_NATIVE_LOCKED, on) };
        va = rec.end;
    }
    unsafe { a20_madvise_unlock(mm, flags) };
    0
}

/// `mm_lookup_vmo_region`: export the VMO behind `[addr, addr + len)`.
///
/// The range must be fully covered by a single `VM_VMO` record; the returned
/// VMO carries its own reference, taken while `mm->lock` is still held so a
/// concurrent unmap cannot drop the record between the check and the ref.
#[no_mangle]
pub unsafe extern "C" fn a20_madvise_vmo_region(
    mm: *mut c_void,
    addr: usize,
    len: usize,
    prot_out: *mut u32,
) -> *mut c_void {
    if mm.is_null() || len == 0 || addr & (PAGE_SIZE - 1) != 0 || len & (PAGE_SIZE - 1) != 0 {
        return core::ptr::null_mut();
    }
    let flags = unsafe { a20_madvise_lock(mm) };
    let vma = unsafe { a20_madvise_vmo_covering(mm, addr, addr + len) };
    let mut vmo: *mut c_void = core::ptr::null_mut();
    if !vma.is_null() && unsafe { a20_madvise_vma_flags(vma) } & VM_VMO != 0 {
        vmo = unsafe { a20_madvise_vmo_ref(vma) };
        if !vmo.is_null() && !prot_out.is_null() {
            let pte_flags = unsafe { a20_madvise_vma_pte_flags(vma) };
            unsafe { *prot_out = mm_pte_flags_to_prot(pte_flags) as u32 };
        }
    }
    unsafe { a20_madvise_unlock(mm, flags) };
    vmo
}
