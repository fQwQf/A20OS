#![no_std]

//! The protection-change transaction.
//!
//! `mm_mprotect_locked` decides and orders everything a protection change can
//! fail at or get wrong:
//!
//!   * the argument contract: the known-prot mask, the pointer-signature hint
//!     bits (a hint is not an access bit, so it is dropped before the W^X
//!     policy or the page table can read it as one), alignment and rounding;
//!   * the range contract: full coverage, the Native capability ceiling, and
//!     the mseal refusal -- all before a single PTE or record is touched;
//!   * the split of the address range at both edges, so exactly what the
//!     caller named changes;
//!   * every fallible huge-leaf demotion, finished *before* any access bit
//!     moves: an allocation failure may change table shape, never leave the
//!     first half of the range with new permissions and the rest with old;
//!   * the per-leaf decision: preserve the dirty evidence a shared file
//!     mapping loses with write permission, keep a private file leaf that is
//!     still the canonical page-cache frame read-only and COW, keep a private
//!     anonymous frame that is still shared after fork COW, flush the
//!     instruction cache when execute is granted, and re-probe when a COW
//!     fault changed the leaf under the snapshot;
//!   * the record update and the re-merge.
//!
//! What stays C is the architecture and layout surface: `mm_seg_t` field
//! access, the page-table walk and leaf install (including the owning node
//! lock), the arch PTE bit definitions and encoders, the physical-frame
//! reference peek, the instruction-cache flush, and the page-cache lookup.
//! Those are hardware primitives; the ordering and the policy above are not.

use core::ffi::{c_char, c_void};

const PAGE_SIZE: usize = 4096;

const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
const EACCES: i32 = -13;
const EPERM: i32 = -1;

const PROT_READ: i32 = 1;
const PROT_WRITE: i32 = 2;
const PROT_EXEC: i32 = 4;
const PROT_HINT_MASK: i32 = 0x10 | 0x20;
const PROT_KNOWN_MASK: i32 = PROT_READ | PROT_WRITE | PROT_EXEC | PROT_HINT_MASK;

const VM_READ: u64 = 1 << 0;
const VM_WRITE: u64 = 1 << 1;
const VM_EXEC: u64 = 1 << 2;
const VM_SHARED: u64 = 1 << 3;
const VM_FILE: u64 = 1 << 13;
const VM_VMO: u64 = 1 << 14;
const VM_NATIVE_CAP: u64 = 1 << 19;
const VM_SEALED: u64 = 1 << 18;

/// One mapping record, as the walk reports it.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Rec {
    vma: *mut c_void,
    start: usize,
    end: usize,
    vm_flags: u64,
    pte_flags: usize,
    vmar_cap: u32,
    reserved: u32,
}

/// One probed leaf.  `pte` is opaque and round-trips back to the bridges.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Leaf {
    pte: usize,
    base: usize,
    size: usize,
    pa: usize,
    pfn: u32,
    level: i32,
    present: i32,
    reserved: i32,
}

/// The architecture's PTE bit definitions, handed to the decision code once
/// per call.  They are data about the encoding, not policy.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Bits {
    w: usize,
    d: usize,
    x: usize,
    cow: usize,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 48] = [(); core::mem::size_of::<Rec>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 48] = [(); core::mem::size_of::<Leaf>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); core::mem::size_of::<Bits>()];

unsafe extern "C" {
    // Existing MM symbols called directly.
    fn mm_seg_index_invalidate(mm: *mut c_void);
    fn mm_pte_flags_apply_prot(pte: usize, apply: usize) -> usize;
    fn mm_demote_huge_page(mm: *mut c_void, va: usize) -> i32;
    fn mm_file_cache_mapping_get(vma: *mut c_void, va: usize, pfn: u32) -> *mut c_void;
    fn page_cache_put(page: *mut c_void);

    // Narrow C bridges declared beside the adapters in mm/mprotect.c.
    fn a20_mprotect_has_pgdir(mm: *const c_void) -> i32;
    fn a20_mprotect_user_va_limit() -> usize;
    fn a20_mprotect_is_nommu() -> i32;
    fn a20_mprotect_record_from(mm: *const c_void, va: usize, out: *mut Rec) -> i32;
    fn a20_mprotect_split_at(mm: *mut c_void, addr: usize) -> i32;
    fn a20_mprotect_set_vma(vma: *mut c_void, pte_flags: usize, vm_flags: u64);
    fn a20_mprotect_merge(mm: *mut c_void, vma: *mut c_void, next_va: *mut usize) -> *mut c_void;
    fn a20_mprotect_bits(out: *mut Bits);
    fn a20_mprotect_probe(mm: *const c_void, va: usize, out: *mut Leaf);
    fn a20_mprotect_install(
        mm: *mut c_void,
        va: usize,
        observed: usize,
        pa: usize,
        flags: usize,
    ) -> i32;
    fn a20_mprotect_refresh_absent(mm: *mut c_void, va: usize, ptef: usize);
    fn a20_mprotect_harvest(
        mm: *mut c_void,
        vma: *mut c_void,
        va: usize,
        observed: usize,
        removing_write: i32,
    );
    fn a20_mprotect_frame_refs(pfn: u32) -> u32;
    fn a20_mprotect_flush_icache(pfn: u32);
    fn mm_wx_filter_prot(prot: i32, ctx: *const c_char) -> i32;
    fn mm_prot_to_pte_flags(prot: i32) -> usize;
}

#[inline]
fn round_up_len(len: usize) -> usize {
    len.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1)
}

#[inline]
fn prot_vm_flags(prot: i32) -> u64 {
    let mut vmf = 0u64;
    if prot & PROT_READ != 0 {
        vmf |= VM_READ;
    }
    if prot & PROT_WRITE != 0 {
        vmf |= VM_WRITE;
    }
    if prot & PROT_EXEC != 0 {
        vmf |= VM_EXEC;
    }
    vmf
}

/// The per-leaf permission the transaction installs.
unsafe fn leaf_flags(
    mm: *mut c_void,
    rec: &Rec,
    leaf: &Leaf,
    observed: usize,
    ptef: usize,
    bits: &Bits,
    removing_write: bool,
) -> usize {
    unsafe { a20_mprotect_harvest(mm, rec.vma, leaf.base, observed, i32::from(removing_write)) };
    let mut flags = unsafe { mm_pte_flags_apply_prot(observed, ptef) };

    // A private file leaf may still be the canonical page-cache frame, which
    // must stay read-only: the first store copies it in the COW path.
    if ptef & bits.w != 0 && rec.vm_flags & VM_FILE != 0 && rec.vm_flags & VM_SHARED == 0 {
        let page = unsafe { mm_file_cache_mapping_get(rec.vma, leaf.base, leaf.pfn) };
        if !page.is_null() {
            flags &= !(bits.w | bits.d);
            flags |= bits.cow;
            unsafe { page_cache_put(page) };
        }
    }

    // A private anonymous frame still shared after fork must keep COW even
    // when this call is re-granting write after a read-only interval cleared
    // the old COW bit.  A temporary pin may cause a spare copy, which is safe;
    // granting write to a shared frame is not.
    if ptef & bits.w != 0 && rec.vm_flags & (VM_SHARED | VM_VMO) == 0 {
        if unsafe { a20_mprotect_frame_refs(leaf.pfn) } > 1 {
            flags &= !(bits.w | bits.d);
            flags |= bits.cow;
        }
    }

    // Newly executable code has to be visible to the instruction stream.
    if flags & bits.x != 0 && observed & bits.x == 0 {
        unsafe { a20_mprotect_flush_icache(leaf.pfn) };
    }
    flags
}

/// Update one covered record's leaves, then its own flags, then re-merge it.
unsafe fn protect_record(
    mm: *mut c_void,
    rec: &Rec,
    addr: usize,
    end: usize,
    ptef: usize,
    vm_prot: u64,
    bits: &Bits,
) -> i32 {
    let removing_write = vm_prot & VM_WRITE == 0;
    let mut va = if rec.start < addr { addr } else { rec.start };
    let rend = if rec.end > end { end } else { rec.end };

    while va < rend {
        let mut leaf = Leaf::default();
        unsafe { a20_mprotect_probe(mm, va, &mut leaf) };
        if leaf.present == 0 {
            unsafe { a20_mprotect_refresh_absent(mm, va, ptef) };
            va += PAGE_SIZE;
            continue;
        }
        if leaf.level > 0 {
            // A large leaf is rewritten one entry at a time, which is only
            // sound once its size is a single page.  The pre-pass demoted the
            // ones that are known fallible; this is the defensive repeat.
            if unsafe { mm_demote_huge_page(mm, va) } < 0 {
                return ENOMEM;
            }
            continue;
        }
        let observed = leaf.pte;
        let flags = unsafe { leaf_flags(mm, rec, &leaf, observed, ptef, bits, removing_write) };
        let rc = unsafe { a20_mprotect_install(mm, va, observed, leaf.pa, flags) };
        if rc < 0 {
            return rc;
        }
        if rc == 0 {
            // A COW fault replaced the leaf under the snapshot: recompute for
            // the same address rather than skipping it.
            continue;
        }
        va = leaf.base + leaf.size;
    }

    let new_pte = unsafe { mm_pte_flags_apply_prot(rec.pte_flags, ptef) };
    unsafe {
        a20_mprotect_set_vma(
            rec.vma,
            new_pte,
            (rec.vm_flags & !(VM_READ | VM_WRITE | VM_EXEC)) | vm_prot,
        )
    };
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_mprotect_locked_rust(
    mm: *mut c_void,
    addr: usize,
    len: usize,
    prot: i32,
) -> i32 {
    if mm.is_null() || unsafe { a20_mprotect_has_pgdir(mm) } == 0 {
        return EINVAL;
    }
    if prot & !PROT_KNOWN_MASK != 0 {
        return EINVAL;
    }
    let prot = prot & !PROT_HINT_MASK;
    if addr & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }
    let len = round_up_len(len);
    if len == 0 {
        return 0;
    }

    let prot = unsafe { mm_wx_filter_prot(prot, c"mprotect".as_ptr()) };
    if prot < 0 {
        return prot;
    }
    let ptef = unsafe { mm_prot_to_pte_flags(prot) };
    let vm_prot = prot_vm_flags(prot);
    let end = match addr.checked_add(len) {
        Some(end) => end,
        None => return ENOMEM,
    };
    if end > unsafe { a20_mprotect_user_va_limit() } {
        return ENOMEM;
    }
    unsafe { mm_seg_index_invalidate(mm) };

    // Coverage first: a hole means nothing is changed.
    let mut covered = addr;
    while covered < end {
        let mut r = Rec::default();
        if unsafe { a20_mprotect_record_from(mm, covered, &mut r) } == 0 || r.start > covered {
            return ENOMEM;
        }
        if r.end <= covered {
            return ENOMEM;
        }
        covered = if r.end < end { r.end } else { end };
    }

    // The Native handle ceiling and mseal are both refused before any change.
    let mut va = addr;
    while va < end {
        let mut r = Rec::default();
        if unsafe { a20_mprotect_record_from(mm, va, &mut r) } == 0 {
            break;
        }
        if r.start >= end {
            break;
        }
        if r.vm_flags & VM_NATIVE_CAP != 0 && (prot as u32 & !r.vmar_cap) != 0 {
            return EACCES;
        }
        if r.vm_flags & VM_SEALED != 0 {
            return EPERM;
        }
        va = r.end;
    }

    if unsafe { a20_mprotect_is_nommu() } != 0 {
        // NOMMU has no page tables: only the record's own permission bits
        // change, and no split is needed because there is one flat mapping
        // per record already.
        let mut va = addr;
        while va < end {
            let mut r = Rec::default();
            if unsafe { a20_mprotect_record_from(mm, va, &mut r) } == 0 || r.start >= end {
                break;
            }
            let new_pte = unsafe { mm_pte_flags_apply_prot(r.pte_flags, ptef) };
            unsafe {
                a20_mprotect_set_vma(
                    r.vma,
                    new_pte,
                    (r.vm_flags & !(VM_READ | VM_WRITE | VM_EXEC)) | vm_prot,
                )
            };
            va = r.end;
        }
        return 0;
    }

    // Split at both edges, so the sealed/protected extent is exactly the
    // requested one and neighbours are untouched.  These splits also make the
    // per-record edge checks below provably no-ops, which is why there is no
    // second split inside the loop.
    if unsafe { a20_mprotect_split_at(mm, addr) } < 0 {
        return ENOMEM;
    }
    if unsafe { a20_mprotect_split_at(mm, end) } < 0 {
        return ENOMEM;
    }

    // Finish every fallible demotion before changing any access bit.
    let mut va = addr;
    while va < end {
        let mut leaf = Leaf::default();
        unsafe { a20_mprotect_probe(mm, va, &mut leaf) };
        if leaf.present != 0 && leaf.level > 0 {
            if unsafe { mm_demote_huge_page(mm, va) } < 0 {
                return ENOMEM;
            }
            continue;
        }
        let step = if leaf.size != 0 { leaf.size } else { PAGE_SIZE };
        let step = if step < PAGE_SIZE { PAGE_SIZE } else { step };
        va += step;
    }

    let mut bits = Bits::default();
    unsafe { a20_mprotect_bits(&mut bits) };

    let mut va = addr;
    while va < end {
        let mut r = Rec::default();
        if unsafe { a20_mprotect_record_from(mm, va, &mut r) } == 0 {
            break;
        }
        if r.start >= end {
            break;
        }
        let rc = unsafe { protect_record(mm, &r, addr, end, ptef, vm_prot, &bits) };
        if rc != 0 {
            return rc;
        }
        let mut next_va = 0usize;
        unsafe { a20_mprotect_merge(mm, r.vma, &mut next_va) };
        // The merge may have absorbed the next record (identical flags, so it
        // needed nothing) or the previous one; resume after the survivor.
        va = if next_va > va { next_va } else { r.end };
    }
    0
}

