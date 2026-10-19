#![no_std]

//! High-level mapping-creation policy for `mmap`/`mmap_file`/`mmap_vmo`.
//!
//! This module owns the whole address-selection and record-publication
//! transaction: length/protection validation, the W^X decision, the Linux
//! `MAP_FIXED`/`MAP_FIXED_NOREPLACE`/hint semantics, address selection, the
//! `vm_flags`/`pte_flags`/capability policy for the new mapping, the mlock
//! admission decision, and the ordering of every fallible step together with
//! the exact rollback of each partial acquisition.
//!
//! NOMMU is a different *address* rule, not a different transaction, so it is
//! the same code path with one branch: `a20_mmap_is_nommu()` selects freshly
//! allocated, page-aligned flat memory instead of a gap in an address space,
//! and the VMO path answers `-EOPNOTSUPP` because a VMO mapping is defined by
//! its page-table leaves.  Everything else -- validation, W^X, the fixed
//! contracts, the capability policy, the mlock admission and the rollback --
//! is shared, which is the point of keeping one transaction.
//!
//! What stays C is deliberately narrow and mechanical: the `mm_seg_t` layout
//! (filled through one `repr(C)` spec), the sorted-list insert, the interval
//! search/overlap primitives, the flat allocation, the page-table reservation
//! call, the backing reference primitives
//! (`vnode_get`/`vnode_put`/`vmar_acquire`/`vmo_ref`), and the arch constants
//! this file cannot see (`USER_VA_LIMIT`, `MMAP_BASE_ADDR`).  Those are
//! ABI/layout/hardware bridges, not policy.
//!
//! The caller already holds `mm->lock` and the TLB-invalidate window is open;
//! nothing here takes a lock or blocks.

use core::ffi::{c_char, c_void};

const PAGE_SIZE: usize = 4096;

const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
const EEXIST: i32 = -17;
const EOPNOTSUPP: i32 = -95;

const PROT_READ: i32 = 1;
const PROT_WRITE: i32 = 2;
const PROT_EXEC: i32 = 4;

const MAP_SHARED: i32 = 0x01;
const MAP_FIXED: i32 = 0x10;
const MAP_HUGETLB: i32 = 0x40000;
const MAP_FIXED_NOREPLACE: i32 = 0x100000;

const VM_READ: u64 = 1 << 0;
const VM_WRITE: u64 = 1 << 1;
const VM_EXEC: u64 = 1 << 2;
const VM_SHARED: u64 = 1 << 3;
const VM_ANON: u64 = 1 << 4;
const VM_HUGEPAGE: u64 = 1 << 11;
const VM_FILE: u64 = 1 << 13;
const VM_VMO: u64 = 1 << 14;
const VM_LOCKED: u64 = 1 << 15;
const VM_NATIVE_CAP: u64 = 1 << 19;

/// The `mm_seg_t` fields the creation transaction sets, in the order the C
/// bridge copies them.  The record is filled only once every fallible step
/// except publication has succeeded, so a refused mapping never exists in a
/// half-described state that another holder could observe.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct SegSpec {
    start: usize,
    end: usize,
    vm_flags: u64,
    pte_flags: usize,
    vmar_cap: u32,
    reserved: u32,
    file: *mut c_void,
    backing_offset: u64,
    file_vnode: *mut c_void,
    vmo: *mut c_void,
    vmar_owner: *mut c_void,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 80] = [(); core::mem::size_of::<SegSpec>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 48] = [(); core::mem::size_of::<SegSpec>()];

unsafe extern "C" {
    // Existing MM primitives: interval search, list insert, page-table
    // reservation, and the backing-object reference helpers.
    fn mm_seg_new() -> *mut c_void;
    fn mm_insert_vma(mm: *mut c_void, vma: *mut c_void);
    fn mm_find_gap(mm: *mut c_void, hint: usize, len: usize) -> usize;
    fn mm_range_overlaps(
        mm: *mut c_void,
        start: usize,
        len: usize,
        ignore: *const c_void,
    ) -> i32;
    fn mm_munmap_locked(mm: *mut c_void, addr: usize, len: usize) -> i32;
    fn mm_wx_filter_prot(prot: i32, ctx: *const c_char) -> i32;
    fn mm_prot_to_pte_flags(prot: i32) -> usize;
    fn mm_pte_flags_to_prot(pte_flags: usize) -> i32;
    fn kfree(ptr: *mut c_void);
    fn vnode_get(vn: *mut c_void);
    fn vnode_put(vn: *mut c_void);
    fn vmar_acquire(vmar: *mut c_void);
    fn vmo_ref(vmo: *mut c_void);

    // Narrow C bridges declared beside the adapters in mm/mmap.c.
    fn a20_mmap_seg_fill(vma: *mut c_void, spec: *const SegSpec, nommu_raw: *mut c_void);
    fn a20_mmap_mmap_base(mm: *mut c_void) -> usize;
    fn a20_mmap_def_locked(mm: *mut c_void) -> i32;
    fn a20_mmap_locked_vm(mm: *mut c_void) -> usize;
    fn a20_mmap_locked_vm_add(mm: *mut c_void, len: usize);
    fn a20_mmap_total_vm_add(mm: *mut c_void, pages: usize);
    fn a20_mmap_memlock_limit() -> u64;
    fn a20_mmap_has_cap_admin() -> i32;
    fn a20_mmap_file_vnode(file: *mut c_void) -> *mut c_void;
    fn a20_mmap_vmo_size(vmo: *mut c_void) -> u64;
    fn a20_mmap_vnode_shared_map_inc(vn: *mut c_void);
    fn a20_mmap_provision_anon(mm: *mut c_void, start: usize, end: usize, ptef: usize);
    fn a20_mmap_user_va_limit() -> usize;
    fn a20_mmap_base_addr() -> usize;
    fn a20_mmap_is_nommu() -> i32;
    fn a20_mmap_nommu_alloc(len: usize, addr_out: *mut usize) -> *mut c_void;
}

/// The address a mapping will be published at, plus the flat allocation it
/// owns on NOMMU (null on an MMU profile).
struct Placed {
    addr: usize,
    nommu_raw: *mut c_void,
}

#[inline]
fn errno(e: i32) -> usize {
    e as isize as usize
}

/// `ROUND_UP(len, PAGE_SIZE)` with the C version's wrap behavior: a length so
/// large it wraps to zero is rejected as `-EINVAL`, exactly as `len == 0` is.
#[inline]
fn round_up_len(len: usize) -> Result<usize, i32> {
    let Some(rounded) = len.checked_add(PAGE_SIZE - 1) else {
        return Err(EINVAL);
    };
    let rounded = rounded & !(PAGE_SIZE - 1);
    if rounded == 0 {
        return Err(EINVAL);
    }
    if rounded > unsafe { a20_mmap_user_va_limit() } {
        return Err(ENOMEM);
    }
    Ok(rounded)
}

/// `vmf |= VM_READ/WRITE/EXEC` for the PROT bits the mapping asked for.
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

/// The Linux address contracts, in the order the ABI guarantees them:
///   - `MAP_FIXED_NOREPLACE` upgrades to `MAP_FIXED` only after proving the
///     range is free, and answers `-EEXIST` otherwise;
///   - `MAP_FIXED` releases the previous mapping through the ordinary unmap
///     transaction and propagates its failure;
///   - a plain hint that is out of range or wholly collides is *dropped*, not
///     failed -- V8's `GetRandomMmapAddr` generates hints up to 2^46 and
///     relies on the kernel falling back to a legal address.
///
/// The returned address is the requested one, or zero when the caller must
/// pick a fresh one.
unsafe fn apply_fixed_and_hint(
    mm: *mut c_void,
    addr: usize,
    len: usize,
    flags: i32,
) -> Result<usize, i32> {
    let mut addr = addr;
    let mut flags = flags;
    let limit = unsafe { a20_mmap_user_va_limit() };

    if flags & MAP_FIXED_NOREPLACE != 0 && addr != 0 {
        if unsafe { mm_range_overlaps(mm, addr, len, core::ptr::null()) } != 0 {
            return Err(EEXIST);
        }
        flags |= MAP_FIXED;
    }

    if flags & MAP_FIXED != 0 && addr != 0 {
        let mr = unsafe { mm_munmap_locked(mm, addr, len) };
        if mr < 0 {
            return Err(mr);
        }
    } else if addr != 0 {
        let usable = match addr.checked_add(len) {
            Some(end) => end <= limit,
            None => false,
        };
        if !usable || unsafe { mm_range_overlaps(mm, addr, len, core::ptr::null()) } != 0 {
            addr = 0;
        }
    }
    Ok(addr)
}

/// Turn the (possibly zero) requested address into the address the mapping is
/// published at, and own whatever flat allocation that required.
unsafe fn finish_address(mm: *mut c_void, addr: usize, len: usize) -> Result<Placed, i32> {
    // NOMMU has no address space to place anything in: a named address is only
    // ever compared against this process's own VMAs, so honouring it would
    // alias another owner's memory.  Every mapping gets freshly allocated,
    // page-aligned, zeroed memory instead -- MAP_FIXED included.
    if unsafe { a20_mmap_is_nommu() } != 0 {
        let mut aligned = 0usize;
        let raw = unsafe { a20_mmap_nommu_alloc(len, &mut aligned) };
        if raw.is_null() {
            return Err(ENOMEM);
        }
        return Ok(Placed {
            addr: aligned,
            nommu_raw: raw,
        });
    }

    let mut addr = addr;
    if addr == 0 {
        let base = unsafe { a20_mmap_mmap_base(mm) };
        let hint = if base != 0 {
            base
        } else {
            unsafe { a20_mmap_base_addr() }
        };
        addr = unsafe { mm_find_gap(mm, hint, len) };
    }
    if addr == 0 {
        return Err(ENOMEM);
    }
    let limit = unsafe { a20_mmap_user_va_limit() };
    if addr.checked_add(len).map_or(true, |end| end > limit) {
        return Err(ENOMEM);
    }
    Ok(Placed {
        addr,
        nommu_raw: core::ptr::null_mut(),
    })
}

/// Release everything a refused mapping acquired.  The record is not on any
/// list yet, so it is freed directly rather than put; the flat allocation, when
/// there is one, is freed here too.
unsafe fn abandon(vma: *mut c_void, placed: &Placed) {
    if !vma.is_null() {
        unsafe { kfree(vma) };
    }
    if !placed.nommu_raw.is_null() {
        unsafe { kfree(placed.nommu_raw) };
    }
}

/// The mlock admission decision, shared by the anonymous and file paths.
///
/// `Ok(bits)` means the mapping is admitted and `mm->locked_vm` has already
/// been charged for `len`; `Err` means nothing was charged and the caller must
/// abandon what it has already acquired.  Charging and admission are one step
/// so there is no window in which the accounting and the decision disagree.
unsafe fn mlock_admission(mm: *mut c_void, len: usize) -> Result<u64, i32> {
    if unsafe { a20_mmap_def_locked(mm) } == 0 {
        return Ok(0);
    }
    // Both sides are `uint64_t` in C: `mm->locked_vm` (size_t) promotes to the
    // 64-bit `rlimit(RLIMIT_MEMLOCK)` ceiling, so the comparison is done in
    // 64 bits on every architecture.
    let over_limit = (unsafe { a20_mmap_locked_vm(mm) } as u64)
        .checked_add(len as u64)
        .map_or(true, |total| total > unsafe { a20_mmap_memlock_limit() });
    if over_limit && unsafe { a20_mmap_has_cap_admin() } == 0 {
        return Err(ENOMEM);
    }
    unsafe { a20_mmap_locked_vm_add(mm, len) };
    Ok(VM_LOCKED)
}

/// Anonymous `mmap`: reserve a range, publish its record, and (for private
/// anonymous mappings) record the per-PTE reservation the status-driven fault
/// path reads.
#[no_mangle]
pub unsafe extern "C" fn a20_mmap_anon_rust(
    mm: *mut c_void,
    addr: usize,
    len: usize,
    prot: i32,
    flags: i32,
    native_cap: i32,
) -> usize {
    if mm.is_null() {
        return errno(EINVAL);
    }
    if flags & (MAP_FIXED | MAP_FIXED_NOREPLACE) != 0 && addr & (PAGE_SIZE - 1) != 0 {
        return errno(EINVAL);
    }
    let len = match round_up_len(len) {
        Ok(len) => len,
        Err(e) => return errno(e),
    };
    // W^X is applied before anything is allocated, so a refused mapping costs
    // nothing.
    let prot = unsafe { mm_wx_filter_prot(prot, c"mmap".as_ptr()) };
    if prot < 0 {
        return errno(prot);
    }

    let ptef = unsafe { mm_prot_to_pte_flags(prot) };
    let mut vmf = VM_ANON | prot_vm_flags(prot);
    if flags & MAP_SHARED != 0 {
        vmf |= VM_SHARED;
    }
    if flags & MAP_HUGETLB != 0 {
        vmf |= VM_HUGEPAGE;
    }
    if native_cap >= 0 {
        vmf |= VM_NATIVE_CAP;
    }

    let addr = match unsafe { apply_fixed_and_hint(mm, addr, len, flags) } {
        Ok(addr) => addr,
        Err(e) => return errno(e),
    };
    let placed = match unsafe { finish_address(mm, addr, len) } {
        Ok(placed) => placed,
        Err(e) => return errno(e),
    };

    let vma = unsafe { mm_seg_new() };
    if vma.is_null() {
        unsafe { abandon(core::ptr::null_mut(), &placed) };
        return errno(ENOMEM);
    }
    match unsafe { mlock_admission(mm, len) } {
        Ok(bits) => vmf |= bits,
        Err(e) => {
            unsafe { abandon(vma, &placed) };
            return errno(e);
        }
    }

    let spec = SegSpec {
        start: placed.addr,
        end: placed.addr + len,
        vm_flags: vmf,
        pte_flags: ptef,
        vmar_cap: if native_cap >= 0 {
            native_cap as u32
        } else {
            (unsafe { mm_pte_flags_to_prot(ptef) }) as u32
        },
        reserved: 0,
        file: core::ptr::null_mut(),
        backing_offset: 0,
        file_vnode: core::ptr::null_mut(),
        vmo: core::ptr::null_mut(),
        vmar_owner: core::ptr::null_mut(),
    };
    unsafe { a20_mmap_seg_fill(vma, &spec, placed.nommu_raw) };

    unsafe { mm_insert_vma(mm, vma) };
    unsafe { a20_mmap_total_vm_add(mm, len / PAGE_SIZE) };
    if vmf & VM_ANON != 0 && vmf & VM_SHARED == 0 {
        unsafe { a20_mmap_provision_anon(mm, placed.addr, placed.addr + len, ptef) };
    }
    placed.addr
}

/// File-backed `mmap`.  `file` arrives referenced and the record takes that
/// reference over on success; on every failure path the caller keeps it and
/// releases it after dropping `mm->lock`, so nothing here puts it.
#[no_mangle]
pub unsafe extern "C" fn a20_mmap_file_rust(
    mm: *mut c_void,
    addr: usize,
    len: usize,
    prot: i32,
    flags: i32,
    file: *mut c_void,
    file_offset: u64,
    native_cap: i32,
    owner: *mut c_void,
) -> usize {
    if file.is_null() || file_offset & (PAGE_SIZE as u64 - 1) != 0 {
        return errno(EINVAL);
    }
    if flags & (MAP_FIXED | MAP_FIXED_NOREPLACE) != 0 && addr & (PAGE_SIZE - 1) != 0 {
        return errno(EINVAL);
    }
    let len = match round_up_len(len) {
        Ok(len) => len,
        Err(e) => return errno(e),
    };
    let prot = unsafe { mm_wx_filter_prot(prot, c"mmap_file".as_ptr()) };
    if prot < 0 {
        return errno(prot);
    }

    let addr = match unsafe { apply_fixed_and_hint(mm, addr, len, flags) } {
        Ok(addr) => addr,
        Err(e) => return errno(e),
    };
    let placed = match unsafe { finish_address(mm, addr, len) } {
        Ok(placed) => placed,
        Err(e) => return errno(e),
    };

    let mut vmf = VM_FILE | prot_vm_flags(prot);
    if flags & MAP_SHARED != 0 {
        vmf |= VM_SHARED;
    }
    if flags & MAP_HUGETLB != 0 {
        vmf |= VM_HUGEPAGE;
    }
    if native_cap >= 0 {
        vmf |= VM_NATIVE_CAP;
    }

    let vma = unsafe { mm_seg_new() };
    if vma.is_null() {
        unsafe { abandon(core::ptr::null_mut(), &placed) };
        return errno(ENOMEM);
    }
    // Every file VMA retains its vnode: MAP_SHARED needs it for dirty-page
    // writeback, and read-only MAP_PRIVATE uses it to tell a direct page-cache
    // leaf from an anonymous COW leaf during teardown.
    let vnode = unsafe { a20_mmap_file_vnode(file) };
    if !vnode.is_null() {
        unsafe { vnode_get(vnode) };
    }
    match unsafe { mlock_admission(mm, len) } {
        Ok(bits) => vmf |= bits,
        Err(e) => {
            if !vnode.is_null() {
                unsafe { vnode_put(vnode) };
            }
            unsafe { abandon(vma, &placed) };
            return errno(e);
        }
    }

    let spec = SegSpec {
        start: placed.addr,
        end: placed.addr + len,
        vm_flags: vmf,
        pte_flags: unsafe { mm_prot_to_pte_flags(prot) },
        vmar_cap: if native_cap >= 0 { native_cap as u32 } else { prot as u32 },
        reserved: 0,
        file,
        backing_offset: file_offset,
        file_vnode: vnode,
        vmo: core::ptr::null_mut(),
        vmar_owner: owner,
    };
    unsafe { a20_mmap_seg_fill(vma, &spec, placed.nommu_raw) };

    if vmf & VM_SHARED != 0 && !vnode.is_null() {
        unsafe { a20_mmap_vnode_shared_map_inc(vnode) };
    }
    if !owner.is_null() {
        unsafe { vmar_acquire(owner) };
    }
    unsafe { mm_insert_vma(mm, vma) };
    unsafe { a20_mmap_total_vm_add(mm, len / PAGE_SIZE) };
    placed.addr
}

/// VMO-backed `mmap`.  Unlike the file path there is no mlock admission block
/// here; that asymmetry is the existing contract and is preserved.  A VMO
/// mapping is defined by its page-table leaves, so NOMMU answers
/// `-EOPNOTSUPP` after the fixed-address contracts have been honoured.
#[no_mangle]
pub unsafe extern "C" fn a20_mmap_vmo_rust(
    mm: *mut c_void,
    addr: usize,
    len: usize,
    prot: i32,
    flags: i32,
    vmo: *mut c_void,
    vmo_offset: u64,
    native_cap: i32,
    owner: *mut c_void,
) -> usize {
    if mm.is_null() || vmo.is_null() {
        return errno(EINVAL);
    }
    if flags & (MAP_FIXED | MAP_FIXED_NOREPLACE) != 0 && addr & (PAGE_SIZE - 1) != 0 {
        return errno(EINVAL);
    }
    let len = match round_up_len(len) {
        Ok(len) => len,
        Err(e) => return errno(e),
    };
    let vmo_size = unsafe { a20_mmap_vmo_size(vmo) };
    if vmo_offset >= vmo_size || (len as u64) > vmo_size - vmo_offset {
        return errno(EINVAL);
    }
    if vmo_offset & (PAGE_SIZE as u64 - 1) != 0 {
        return errno(EINVAL);
    }
    let prot = unsafe { mm_wx_filter_prot(prot, c"mmap_vmo".as_ptr()) };
    if prot < 0 {
        return errno(prot);
    }

    let addr = match unsafe { apply_fixed_and_hint(mm, addr, len, flags) } {
        Ok(addr) => addr,
        Err(e) => return errno(e),
    };
    if unsafe { a20_mmap_is_nommu() } != 0 {
        return errno(EOPNOTSUPP);
    }
    let placed = match unsafe { finish_address(mm, addr, len) } {
        Ok(placed) => placed,
        Err(e) => return errno(e),
    };

    let mut vmf = VM_VMO | prot_vm_flags(prot);
    if flags & MAP_SHARED != 0 {
        vmf |= VM_SHARED;
    }
    if native_cap >= 0 {
        vmf |= VM_NATIVE_CAP;
    }

    let vma = unsafe { mm_seg_new() };
    if vma.is_null() {
        unsafe { abandon(core::ptr::null_mut(), &placed) };
        return errno(ENOMEM);
    }
    let spec = SegSpec {
        start: placed.addr,
        end: placed.addr + len,
        vm_flags: vmf,
        pte_flags: unsafe { mm_prot_to_pte_flags(prot) },
        vmar_cap: if native_cap >= 0 { native_cap as u32 } else { prot as u32 },
        reserved: 0,
        file: core::ptr::null_mut(),
        backing_offset: vmo_offset,
        file_vnode: core::ptr::null_mut(),
        vmo,
        vmar_owner: owner,
    };
    unsafe { a20_mmap_seg_fill(vma, &spec, core::ptr::null_mut()) };
    unsafe { vmo_ref(vmo) };
    if !owner.is_null() {
        unsafe { vmar_acquire(owner) };
    }
    unsafe { mm_insert_vma(mm, vma) };
    unsafe { a20_mmap_total_vm_add(mm, len / PAGE_SIZE) };
    placed.addr
}
