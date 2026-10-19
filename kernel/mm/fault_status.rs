#![no_std]

//! The status-led demand fault: serving a first touch from the status mark the
//! mapping left behind, without consulting the VMA.
//!
//! Every branch is a decision:
//!
//!   * **A userfaultfd registration wins.**  The fault has to be parked for the
//!     handler, not satisfied here.  The mark is per entry precisely so this
//!     decision needs no VMA, and an entry that is already backed is not ours
//!     to fill either.
//!   * **The recorded protection round-trips.**  The status carries the prot
//!     bits that `mm_pt_prot_bits()` produced from the same encoding, so
//!     decoding them and asking the architecture for the flag set -- rather
//!     than assembling PTE bits here -- keeps the access check identical to the
//!     VMA path.  riscv64 needs `PTE_U`, x86_64 additionally needs `PTE_LEAF`
//!     and an explicit NX, and the helper encodes the W=>R dependency.
//!   * **Allocate, charge, zero, then map -- and undo in that order.**  A
//!     failed charge gives the frame back, and a lost map gives the charge and
//!     the frame back, so no path leaves a charged frame behind or a frame that
//!     was never charged released.
//!   * **Account after unlocking, then flush locally.**  The RSS and the four
//!     soft-fault counters the VMA paths bump alongside `rss++` are bumped here
//!     too: a fault path that skips them makes every reader of these, and
//!     /proc's reported fault rate, wrong.
//!
//! What stays C is the address-space cursor and the primitives it needs: the
//! query, the absent-only install, the physical frame allocator, the cgroup
//! charge, the architecture's protection encoding and the local TLB flush.

use core::ffi::c_void;

const PFN_NONE: u32 = u32::MAX;

const MM_ST_ANON_VIRT: u8 = 1;
const MM_ST_ANON_MAPPED: u8 = 2;
const MM_ST_PROT_R: u8 = 0x01;
const MM_ST_PROT_W: u8 = 0x02;
const MM_ST_PROT_X: u8 = 0x04;

const PROT_READ: i32 = 1;
const PROT_WRITE: i32 = 2;
const PROT_EXEC: i32 = 4;

const VM_READ: u64 = 1 << 0;
const VM_WRITE: u64 = 1 << 1;
const VM_EXEC: u64 = 1 << 2;

const MM_FAULT_ACCESS_WRITE: i32 = 1;
const MM_FAULT_ACCESS_EXEC: i32 = 2;

/// Opaque storage for the C cursor; the size and alignment are asserted on the
/// C side against `MM_CURSOR_PATH_MAX`.
const CTX_WORDS: usize = 16;

#[repr(C)]
struct Ctx {
    words: [u64; CTX_WORDS],
}

unsafe extern "C" {
    /// Open the cursor over the page and report the entry's facts.  Returns 1
    /// with the cursor open (the caller must close it), or 0 with no lock held
    /// when there is no page table to walk.
    fn a20_fault_status_open(
        ctx: *mut Ctx,
        mm: *mut c_void,
        va: usize,
        cls_out: *mut u8,
        already_out: *mut i32,
        uffd_out: *mut i32,
    ) -> i32;
    fn a20_fault_status_map_if_absent(
        ctx: *mut Ctx,
        va: usize,
        pa: usize,
        allow: usize,
        cls: u8,
    ) -> i32;
    fn a20_fault_status_close(ctx: *mut Ctx);

    // Shared with the status-led COW break.
    fn a20_fault_status_alloc() -> u32;
    fn a20_fault_status_phys(pfn: u32) -> usize;
    fn a20_fault_status_charge(t: *mut c_void) -> i32;
    fn a20_fault_status_uncharge(t: *mut c_void);

    fn a20_fault_status_zero(pfn: u32);
    fn a20_fault_status_rss_add(mm: *mut c_void);
    fn a20_fault_status_bump(t: *mut c_void);
    fn a20_fault_status_flush_local(stval: usize);

    // Existing MM symbols the fault calls directly.
    fn frame_put(pfn: u32);
    fn mm_prot_to_pte_flags(prot: i32) -> usize;
}

/// Serve a demand fault from the status mark.  Returns 1 when the fault is
/// satisfied, 0 to let the caller fall through to the VMA path.
#[no_mangle]
pub unsafe extern "C" fn a20_fault_from_status_rust(
    t: *mut c_void,
    mm: *mut c_void,
    page_va: usize,
    stval: usize,
    access: i32,
) -> i32 {
    let mut ctx = Ctx { words: [0; CTX_WORDS] };
    let mut cls_byte: u8 = 0;
    let mut already: i32 = 0;
    let mut uffd: i32 = 0;
    if unsafe { a20_fault_status_open(&mut ctx, mm, page_va, &mut cls_byte, &mut already, &mut uffd) }
        != 1
    {
        return 0;
    }

    /* Declines past this point close the cursor the caller opened. */
    if already != 0 || (cls_byte >> 4) & 0xF != MM_ST_ANON_VIRT || uffd != 0 {
        unsafe { a20_fault_status_close(&mut ctx) };
        return 0;
    }

    let mut prot = 0;
    if cls_byte & MM_ST_PROT_R != 0 {
        prot |= PROT_READ;
    }
    if cls_byte & MM_ST_PROT_W != 0 {
        prot |= PROT_WRITE;
    }
    if cls_byte & MM_ST_PROT_X != 0 {
        prot |= PROT_EXEC;
    }
    let allow = unsafe { mm_prot_to_pte_flags(prot) };

    let mut vm_flags: u64 = 0;
    if prot & PROT_READ != 0 {
        vm_flags |= VM_READ;
    }
    if prot & PROT_WRITE != 0 {
        vm_flags |= VM_WRITE;
    }
    if prot & PROT_EXEC != 0 {
        vm_flags |= VM_EXEC;
    }
    if a20_fault_status_access_ok(vm_flags, access) == 0 {
        unsafe { a20_fault_status_close(&mut ctx) };
        return 0;
    }

    /* Non-reclaiming, for the same reason the other status-led path insists on
     * it: a reclaiming allocation reaches oom_try_reclaim(), whose victim
     * teardown wants the page-table node locks this path holds. */
    let new_pfn = unsafe { a20_fault_status_alloc() };
    if new_pfn == PFN_NONE {
        unsafe { a20_fault_status_close(&mut ctx) };
        return 0;
    }
    if unsafe { a20_fault_status_charge(t) } != 0 {
        unsafe { frame_put(new_pfn) };
        unsafe { a20_fault_status_close(&mut ctx) };
        return 0;
    }
    unsafe { a20_fault_status_zero(new_pfn) };
    let pa = unsafe { a20_fault_status_phys(new_pfn) };
    if unsafe { a20_fault_status_map_if_absent(&mut ctx, page_va, pa, allow, MM_ST_ANON_MAPPED) } != 0
    {
        unsafe { a20_fault_status_uncharge(t) };
        unsafe { frame_put(new_pfn) };
        unsafe { a20_fault_status_close(&mut ctx) };
        return 0;
    }

    unsafe { a20_fault_status_close(&mut ctx) };
    unsafe { a20_fault_status_rss_add(mm) };
    unsafe { a20_fault_status_bump(t) };
    unsafe { a20_fault_status_flush_local(stval) };
    1
}

/// The access check, kept here so both status-led paths answer the same way.
#[no_mangle]
pub extern "C" fn a20_fault_status_access_ok(vm_flags: u64, access: i32) -> i32 {
    if access == MM_FAULT_ACCESS_WRITE {
        return i32::from(vm_flags & VM_WRITE != 0);
    }
    if access == MM_FAULT_ACCESS_EXEC {
        return i32::from(vm_flags & VM_EXEC != 0);
    }
    i32::from(vm_flags & (VM_READ | VM_WRITE) != 0)
}
