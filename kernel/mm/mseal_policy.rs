#![no_std]

//! The `mseal(2)` transaction: range sealing and the sealed-range query.
//!
//! Sealing is a one-way property of a mapping.  The whole range must be
//! mapped, must not include a region whose lifetime is kernel-owned, and is
//! sealed exactly as the caller asked -- which is why the records are split at
//! both edges before any of them is marked, so a neighbour stays fully
//! mutable.  Nothing is modified before every one of those checks passes.
//!
//! Rust owns the transaction: the coverage walk, the refusal of SysV/PFNMAP/VMO
//! regions, the split ordering, and the per-record extent that gets both the
//! `VM_SEALED` bit and the fault-around mark.  C keeps the `mm_seg_t` field
//! access, `mm_seg_find`/`mm_split_vma_at`, and the per-PTE safe-range
//! sidecar.

use core::ffi::c_void;

const PAGE_SIZE: usize = 4096;

const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
const EPERM: i32 = -1;

const VM_VMO: u64 = 1 << 14;
const VM_ANON: u64 = 1 << 4;
const VM_SHARED: u64 = 1 << 3;
const VM_SYSV_SHM: u64 = 1 << 16;
const VM_PFNMAP: u64 = 1 << 17;
const VM_SEALED: u64 = 1 << 18;

/// One mapping record, as the walk reports it.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Rec {
    vma: *mut c_void,
    start: usize,
    end: usize,
    vm_flags: u64,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); core::mem::size_of::<Rec>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 24] = [(); core::mem::size_of::<Rec>()];

unsafe extern "C" {
    fn a20_mseal_has_pgdir(mm: *const c_void) -> i32;
    fn a20_mseal_user_va_limit() -> usize;
    /// The first record that ends after `va`, or 0 when there is none.  It
    /// deliberately skips holes: the coverage check has to see that a hole is
    /// a hole, not stop at it.
    fn a20_mseal_record_from(mm: *const c_void, va: usize, out: *mut Rec) -> i32;
    fn a20_mseal_split_at(mm: *mut c_void, addr: usize) -> i32;
    /// Set the seal on `[a, b)` of one record and mark that range
    /// `MM_SAFE_NO_FA` so a fault cannot install speculative neighbours.
    fn a20_mseal_mark(mm: *mut c_void, vma: *mut c_void, a: usize, b: usize);
}

/// Any record overlapping `[addr, addr+len)` sealed?  Caller holds `mm->lock`.
#[no_mangle]
pub unsafe extern "C" fn mm_mseal_range_is_sealed_locked(
    mm: *const c_void,
    addr: usize,
    len: usize,
) -> i32 {
    if mm.is_null() {
        return 0;
    }
    let end = addr.wrapping_add(len);
    let mut va = addr;
    while va < end {
        let mut r = Rec::default();
        if unsafe { a20_mseal_record_from(mm, va, &mut r) } == 0 {
            break;
        }
        if r.start >= end {
            break;
        }
        if r.vm_flags & VM_SEALED != 0 {
            return 1;
        }
        va = r.end;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn mm_mseal_locked(mm: *mut c_void, addr: usize, len: usize) -> i32 {
    if mm.is_null() || unsafe { a20_mseal_has_pgdir(mm) } == 0 {
        return EINVAL;
    }
    if addr & (PAGE_SIZE - 1) != 0 {
        return EINVAL;
    }
    if len == 0 {
        return 0;
    }
    let len = len.wrapping_add(PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
    let end = match addr.checked_add(len) {
        Some(end) => end,
        None => return ENOMEM,
    };
    if end > unsafe { a20_mseal_user_va_limit() } {
        return ENOMEM;
    }

    // One pass proves both that every page is mapped and that nothing in the
    // range is kernel-owned.  A hole is refused before any record is touched,
    // which is what makes the operation all-or-nothing.
    let mut covered = addr;
    while covered < end {
        let mut r = Rec::default();
        if unsafe { a20_mseal_record_from(mm, covered, &mut r) } == 0 {
            return ENOMEM;
        }
        if r.start > covered {
            return ENOMEM; // a hole: the range is not fully mapped
        }
        let shared_anon = r.vm_flags & (VM_ANON | VM_SHARED) == (VM_ANON | VM_SHARED);
        if r.vm_flags & (VM_SYSV_SHM | VM_PFNMAP) != 0 ||
            (r.vm_flags & VM_VMO != 0 && !shared_anon) {
            return EPERM;
        }
        if r.end <= covered {
            return ENOMEM; // the walk did not advance; refuse rather than spin
        }
        covered = if r.end < end { r.end } else { end };
    }

    // Split at both edges first, so what gets sealed is exactly the range the
    // caller named and its neighbours stay mutable.
    if unsafe { a20_mseal_split_at(mm, addr) } < 0 {
        return ENOMEM;
    }
    if unsafe { a20_mseal_split_at(mm, end) } < 0 {
        return ENOMEM;
    }

    let mut va = addr;
    while va < end {
        let mut r = Rec::default();
        if unsafe { a20_mseal_record_from(mm, va, &mut r) } == 0 {
            break;
        }
        if r.start >= end {
            break;
        }
        let a = if r.start > addr { r.start } else { addr };
        let b = if r.end < end { r.end } else { end };
        unsafe { a20_mseal_mark(mm, r.vma, a, b) };
        va = r.end;
    }
    0
}
