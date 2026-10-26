#![no_std]

//! The unmap transaction.
//!
//! `mm_munmap_locked` decides and orders everything that can fail: which VMAs
//! the range covers, whether the range is even allowed to be released, the
//! clip each covered record to, the release of the physical leaves inside that
//! clip, the accounting that follows it, and the record surgery that turns the
//! remaining extent into one, two or zero mappings.  It is the transaction the
//! whole address space depends on: `exit`, `exec`, `MAP_FIXED` replacement,
//! `mremap`, `brk` shrink and the syscall all arrive here.
//!
//! Two properties are the reason it is written as one function rather than a
//! sequence of helpers, and both are about partial failure:
//!
//!   * Pages are released before the record is cut, so a later failure leaves
//!     the address space *smaller* rather than describing memory that has
//!     already been handed back.  The only fallible step after the release
//!     loop is the middle cut's tail allocation, and it returns the error with
//!     the already-released pages gone -- never with the records inconsistent.
//!     Callers therefore see either "nothing happened" or "-ENOMEM after the
//!     bytes before this record were released", which is what the ABI
//!     promises.
//!   * Every leaf releases exactly one reference to exactly one owner: a
//!     page-cache page is put twice (the mapping's cache pin plus the
//!     temporary lookup), a plain anonymous frame is put once, and a leaf
//!     owned by a VMO, a driver or the vDSO is not put at all because the
//!     mapping never took a frame reference to it.  Choosing the owner from
//!     the leaf's PFN, not from a VMA-wide flag, is what keeps a private file
//!     mapping -- which mixes clean cache leaves with anonymous COW leaves --
//!     correct after only some of its pages were copied.
//!
//! What stays C is deliberately narrow: `mm_seg_t` field access and the
//! list surgery, the page-table walk and leaf teardown, the architecture
//! predicates (presence, swap, leaf, physical address, dirty bit), and the TLB
//! hold/release primitives.  Those are layout and hardware bridges; the
//! decisions above are not.

use core::ffi::c_void;

const PAGE_SIZE: usize = 4096;

const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
const EPERM: i32 = -1;

const VM_SHARED: u64 = 1 << 3;
const VM_FILE: u64 = 1 << 13;
const VM_VMO: u64 = 1 << 14;
const VM_LOCKED: u64 = 1 << 15;
const VM_SYSV_SHM: u64 = 1 << 16;
const VM_PFNMAP: u64 = 1 << 17;
const VM_SEALED: u64 = 1 << 18;

/// The `mm_seg_t` fields the transaction reads.  Only the extent, the flags and
/// the backing identity are needed; everything else about the record is the C
/// bridge's business.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct VmaInfo {
    start: usize,
    end: usize,
    vm_flags: u64,
    backing_offset: u64,
    file_vnode: *mut c_void,
}

/// The three fields the range scans need.  Walking a range that skips
/// thousands of records must not copy the backing identity of each one, so the
/// scan gets its own narrow snapshot and the full one is fetched only for a
/// record the transaction actually operates on.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct VmaBounds {
    start: usize,
    end: usize,
    vm_flags: u64,
}

/// One covered record, collected in one C walk.  The transaction only ever
/// operates on records the range actually covers, so the walk itself belongs
/// in C: a per-record bridge call would pay a call per *visited* record, and a
/// range that starts late in a large address space visits thousands of them.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Entry {
    vma: *mut c_void,
    start: usize,
    end: usize,
    vm_flags: u64,
    backing_offset: u64,
    file_vnode: *mut c_void,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 48] = [(); core::mem::size_of::<Entry>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 40] = [(); core::mem::size_of::<Entry>()];

const ENTRIES: usize = 32;

/// One probed leaf.  `aux` is opaque: on a probe it carries the architecture's
/// raw PTE value (which the harvest bridge decodes) and on an unmap it carries
/// the physical address `pt_unmap_leaf` reported, so one out-struct serves
/// both without this file knowing the bit layout.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Leaf {
    aux: usize,
    base: usize,
    size: usize,
    pfn: u32,
    level: i32,
    present: i32,
    swap: i32,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 40] = [(); core::mem::size_of::<Leaf>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 28] = [(); core::mem::size_of::<Leaf>()];

unsafe extern "C" {
    // Existing MM symbols the transaction calls directly.
    fn mm_seg_index_invalidate(mm: *mut c_void);
    fn mm_seg_index_clear(mm: *mut c_void);

    // Narrow C bridges declared beside the adapters in mm/munmap.c.
    fn a20_munmap_has_pgdir(mm: *mut c_void) -> i32;
    fn a20_munmap_user_va_limit() -> usize;
    fn a20_munmap_vma_first(mm: *mut c_void) -> *mut c_void;
    fn a20_munmap_vma_next(v: *mut c_void) -> *mut c_void;
    fn a20_munmap_vma_info(v: *mut c_void, out: *mut VmaInfo);
    fn a20_munmap_vma_bounds(v: *mut c_void, out: *mut VmaBounds);
    /// Collect the records overlapping `[addr, end)` into `out`.  Returns the
    /// count, or `-ENOSPC` when there are more than `cap` (the caller then
    /// walks the list itself, which is rare and correct but slower).
    fn a20_munmap_collect(
        mm: *mut c_void,
        addr: usize,
        end: usize,
        out: *mut Entry,
        cap: usize,
    ) -> i32;
    fn a20_munmap_account(mm: *mut c_void, freed_pages: usize, locked_bytes: usize);
    fn a20_munmap_drop_whole(mm: *mut c_void, v: *mut c_void, s: usize, e: usize);
    fn a20_munmap_cut_head(mm: *mut c_void, v: *mut c_void, clip_end: usize);
    fn a20_munmap_cut_tail(mm: *mut c_void, v: *mut c_void, clip_start: usize);
    fn a20_munmap_cut_middle(
        mm: *mut c_void,
        v: *mut c_void,
        clip_start: usize,
        clip_end: usize,
    ) -> i32;
    fn a20_munmap_is_nommu() -> i32;
    fn a20_munmap_release_flat(mm: *mut c_void, vma: *mut c_void, s: usize, e: usize);
    fn a20_munmap_leaf_probe(mm: *mut c_void, va: usize, out: *mut Leaf);
    fn a20_munmap_retire_reservation(mm: *mut c_void, va: usize);
    fn a20_munmap_demote(mm: *mut c_void, va: usize) -> i32;
    fn a20_munmap_leaf_harvest(
        mm: *mut c_void,
        v: *mut c_void,
        va: usize,
        clip_start: usize,
        clip_end: usize,
        leaf: *const Leaf,
        shared_file: i32,
    );
    fn a20_munmap_leaf_hold(
        mm: *mut c_void,
        v: *mut c_void,
        va: usize,
        pfn: u32,
        no_frame_ref: i32,
        out_pcp: *mut *mut c_void,
    ) -> i32;
    fn a20_munmap_leaf_unmap(mm: *mut c_void, va: usize, out: *mut Leaf) -> i32;
    fn a20_munmap_leaf_release(
        mm: *mut c_void,
        no_frame_ref: i32,
        pa: usize,
        base: usize,
        size: usize,
        pcp: *mut c_void,
    );
    fn a20_munmap_page_put(pcp: *mut c_void);
}

#[inline]
fn no_frame_ref(vm_flags: u64) -> i32 {
    i32::from(vm_flags & (VM_PFNMAP | VM_VMO) != 0)
}

/// Release every leaf of `[clip_start, clip_end)` inside one record.
///
/// Returns `Err(-ENOMEM)` when a TLB hold or a huge-page demotion fails; the
/// leaves released before that point stay released, which is the same contract
/// the C version had.
unsafe fn release_leaves(
    mm: *mut c_void,
    vma: *mut c_void,
    info: &VmaInfo,
    clip_start: usize,
    clip_end: usize,
) -> Result<(), i32> {
    let shared_file = (info.vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED);
    let holds_no_frame = no_frame_ref(info.vm_flags);
    let mut va = clip_start;

    while va < clip_end {
        let mut leaf = Leaf::default();
        unsafe { a20_munmap_leaf_probe(mm, va, &mut leaf) };

        if leaf.present == 0 && leaf.swap == 0 {
            // A never-faulted anonymous reservation leaves a per-PTE status
            // mark and no leaf; the teardown has to retire that mark or a
            // later access into the hole maps a page instead of failing.
            if leaf.level == 0 {
                unsafe { a20_munmap_retire_reservation(mm, va) };
            }
            let next_va = if leaf.size != 0 {
                leaf.base + leaf.size
            } else {
                va + PAGE_SIZE
            };
            va = if next_va > va { next_va } else { va + PAGE_SIZE };
            continue;
        }

        // Preserve the cache's dirty state before this mapping loses the only
        // software-visible evidence of it.  Which of the two harvest rules
        // applies is the architecture's business (AArch64 aliases the dirty
        // indication with write permission and ARM32 has none at all).
        unsafe {
            a20_munmap_leaf_harvest(mm, vma, va, clip_start, clip_end, &leaf, i32::from(shared_file))
        };

        // A PMD leaf that straddles either end of the request cannot be
        // dropped wholesale: unmapping it would discard pages outside the
        // request and outside this record, and the cursor would step past
        // them.  Demote first, then re-probe.
        if leaf.level > 0 && (leaf.base < clip_start || leaf.base + leaf.size > clip_end) {
            if unsafe { a20_munmap_demote(mm, va) } < 0 {
                return Err(ENOMEM);
            }
            continue;
        }

        let mut held_pcp: *mut c_void = core::ptr::null_mut();
        if leaf.present != 0
            && unsafe { a20_munmap_leaf_hold(mm, vma, va, leaf.pfn, holds_no_frame, &mut held_pcp) } < 0
        {
            return Err(ENOMEM);
        }

        let mut unmap = Leaf::default();
        if unsafe { a20_munmap_leaf_unmap(mm, va, &mut unmap) } == 0 {
            unsafe {
                a20_munmap_leaf_release(
                    mm,
                    holds_no_frame,
                    unmap.aux,
                    unmap.base,
                    unmap.size,
                    held_pcp,
                )
            };
            va = unmap.base + unmap.size;
        } else {
            if !held_pcp.is_null() {
                unsafe { a20_munmap_page_put(held_pcp) };
            }
            va += PAGE_SIZE;
        }
    }
    Ok(())
}

/// Release, account and cut one covered record.  `info` is the record's state
/// at the moment of the call; the surgery may unlink or split it afterwards.
unsafe fn operate_one(
    mm: *mut c_void,
    vma: *mut c_void,
    info: &VmaInfo,
    clip_start: usize,
    clip_end: usize,
) -> i32 {
    if unsafe { a20_munmap_is_nommu() } != 0 {
        // NOMMU has no leaves to release: the record owns one flat allocation.
        unsafe { a20_munmap_release_flat(mm, vma, clip_start, clip_end) };
    } else if let Err(e) = unsafe { release_leaves(mm, vma, info, clip_start, clip_end) } {
        return e;
    }

    let freed_pages = (clip_end - clip_start) / PAGE_SIZE;
    let locked_bytes = if info.vm_flags & VM_LOCKED != 0 {
        clip_end - clip_start
    } else {
        0
    };
    unsafe { a20_munmap_account(mm, freed_pages, locked_bytes) };

    if clip_start <= info.start && clip_end >= info.end {
        // The whole record goes, so its extent stops being named.
        unsafe { a20_munmap_drop_whole(mm, vma, clip_start, clip_end) };
    } else if clip_start <= info.start {
        unsafe { a20_munmap_cut_head(mm, vma, clip_end) };
    } else if clip_end >= info.end {
        unsafe { a20_munmap_cut_tail(mm, vma, clip_start) };
    } else {
        // The only fallible step left.  Its failure is reported with the pages
        // already released and the two halves still one record, so the address
        // space never describes memory that is gone.
        let r = unsafe { a20_munmap_cut_middle(mm, vma, clip_start, clip_end) };
        if r != 0 {
            return r;
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_munmap_rust(mm: *mut c_void, addr: usize, len: usize) -> i32 {
    if mm.is_null() || unsafe { a20_munmap_has_pgdir(mm) } == 0 {
        return EINVAL;
    }
    if addr & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }
    let len = len.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
    if len == 0 {
        return 0;
    }
    let end = match addr.checked_add(len) {
        Some(end) => end,
        None => return EINVAL,
    };
    if end > unsafe { a20_munmap_user_va_limit() } {
        return EINVAL;
    }
    unsafe { mm_seg_index_invalidate(mm) };

    // Validate the whole range before releasing anything: a partial SysV
    // segment cannot be unmapped, and a sealed record refuses the operation
    // before a single page is handed back.

    let mut buf = [Entry::default(); ENTRIES];
    let n = unsafe { a20_munmap_collect(mm, addr, end, buf.as_mut_ptr(), ENTRIES) };
    if n >= 0 {
        // The bridge contract caps the count at `ENTRIES`; taking the slice by
        // pointer keeps the bound check (and its panic path) out of a module
        // that has no unwinder.
        let n = (n as usize).min(ENTRIES);
        let entries = unsafe { core::slice::from_raw_parts(buf.as_ptr(), n) };
        // Validate every covered record before releasing anything.
        for e in entries {
            if e.vm_flags & VM_SYSV_SHM != 0 && (addr > e.start || end < e.end) {
                return EINVAL;
            }
            if e.vm_flags & VM_SEALED != 0 {
                return EPERM;
            }
        }
        for e in entries {
            let info = VmaInfo {
                start: e.start,
                end: e.end,
                vm_flags: e.vm_flags,
                backing_offset: e.backing_offset,
                file_vnode: e.file_vnode,
            };
            let clip_start = if e.start < addr { addr } else { e.start };
            let clip_end = if e.end > end { end } else { e.end };
            let r = unsafe { operate_one(mm, e.vma, &info, clip_start, clip_end) };
            if r != 0 {
                return r;
            }
        }
        unsafe { mm_seg_index_clear(mm) };
        return 0;
    }

    // More covered records than the collection buffer holds: walk the list
    // directly.  The refusal scan and the operation are the same, just paid at
    // one bridge call per visited record.
    let mut vma = unsafe { a20_munmap_vma_first(mm) };
    while !vma.is_null() {
        let next = unsafe { a20_munmap_vma_next(vma) };
        let mut b = VmaBounds::default();
        unsafe { a20_munmap_vma_bounds(vma, &mut b) };
        if b.start >= end || b.end <= addr {
            vma = next;
            continue;
        }
        if b.vm_flags & VM_SYSV_SHM != 0 && (addr > b.start || end < b.end) {
            return EINVAL;
        }
        if b.vm_flags & VM_SEALED != 0 {
            return EPERM;
        }
        let mut info = VmaInfo::default();
        unsafe { a20_munmap_vma_info(vma, &mut info) };
        let clip_start = if info.start < addr { addr } else { info.start };
        let clip_end = if info.end > end { end } else { info.end };
        let r = unsafe { operate_one(mm, vma, &info, clip_start, clip_end) };
        if r != 0 {
            return r;
        }
        vma = next;
    }

    // Drop the cache's references to the old record pass now.  Keeping the
    // index dirty lets the next lookup rebuild it lazily, while the deferred
    // put holds any last backing release until the TLB shootdown has crossed.
    unsafe { mm_seg_index_clear(mm) };
    0
}

/// Release the references of one run of leaves that `pt_unmap_leaf_run()`
/// already cleared in the final-destroy sweep (`free_vma_pages`).
///
/// This is the same classification rule the unmap transaction applies per
/// leaf, restated for a batch whose page-table work is already done. An
/// annotated cache leaf puts only its mapping pin; an unannotated cache leaf
/// still puts the pin plus the temporary lookup. A plain anonymous frame puts
/// its frame reference; a VMO/PFNMAP leaf puts nothing. Classification remains
/// per page, never a VMA-wide assumption, so mixed private mappings stay correct.
///
/// The caller passes the PFNs and optional direct cache owners of the cleared
/// leaves in order together with `no_frame_ref`. A direct owner releases only
/// the mapping's pin; an unannotated page keeps the old lookup-plus-pin pair.
#[no_mangle]
pub unsafe extern "C" fn a20_vma_free_run_release_rust(
    vma: *mut c_void,
    start_va: usize,
    pfns: *const u32,
    owners: *const *mut c_void,
    count: usize,
    no_frame_ref: i32,
) {
    unsafe extern "C" {
        fn mm_file_cache_mapping_get(vma: *mut c_void, va: usize, pfn: u32)
            -> *mut c_void;
        fn page_cache_put(page: *mut c_void);
        fn frame_put(pfn: u32);
    }
    let mut i = 0usize;
    while i < count {
        let pfn = unsafe { *pfns.add(i) };
        if !owners.is_null() {
            let owner = unsafe { *owners.add(i) };
            if !owner.is_null() {
                unsafe { page_cache_put(owner) };
                i += 1;
                continue;
            }
        }
        let va = start_va + i * PAGE_SIZE;
        let page = unsafe { mm_file_cache_mapping_get(vma, va, pfn) };
        if !page.is_null() {
            // The mapping's retained cache pin plus this lookup's temporary
            // reference: one object, two puts.
            unsafe { page_cache_put(page) };
            unsafe { page_cache_put(page) };
        } else if no_frame_ref == 0 {
            unsafe { frame_put(pfn) };
        }
        i += 1;
    }
}
