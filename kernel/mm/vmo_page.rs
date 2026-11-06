#![no_std]

//! Rust-owned transaction for materializing a canonical VMO page.
//!
//! Locking, frame allocation and cgroup accounting remain narrow C callbacks.
//! This module owns the ordering: recheck the slot under the VMO lock, reserve
//! the optional charge, acquire and zero a frame, then publish once. Any early
//! return drops the transaction and releases resources it acquired.

use core::ffi::c_void;
use core::ptr;

#[cfg(not(test))]
#[path = "vmo_sparse.rs"]
mod sparse;

const PFN_NONE: u32 = u32::MAX;
const EIO: i32 = -5;
const ENOMEM: i32 = -12;
const EINVAL: i32 = -22;
const EAGAIN: i32 = -11;
const PAGE_SIZE: u64 = 4096;

unsafe extern "C" {
    fn a20_vmo_page_lock(vmo: *mut c_void, index: u32, flags: *mut u64) -> i32;
    fn a20_vmo_page_unlock(vmo: *mut c_void, flags: u64);
    fn a20_vmo_page_slot_locked(vmo: *mut c_void, index: u32) -> u32;
    fn a20_vmo_page_is_paged_locked(vmo: *mut c_void) -> i32;
    fn a20_vmo_page_prepare_locked(vmo: *mut c_void, index: u32, cgroup: *mut c_void) -> i32;
    fn a20_vmo_page_paged_fault(vmo: *mut c_void, index: u32, write: i32) -> i32;
    fn a20_vmo_page_charge(vmo: *mut c_void, cgroup: *mut c_void) -> i32;
    fn a20_vmo_page_uncharge(vmo: *mut c_void, cgroup: *mut c_void);
    fn a20_vmo_page_alloc() -> u32;
    fn a20_vmo_page_free(pfn: u32);
    fn a20_vmo_page_zero(pfn: u32);
    fn a20_vmo_page_publish_locked(
        vmo: *mut c_void,
        index: u32,
        pfn: u32,
        cgroup: *mut c_void,
    );
    fn a20_vmo_resize_lock(vmo: *mut c_void, flags: *mut u64) -> i32;
    fn a20_vmo_resize_unlock(vmo: *mut c_void, flags: u64);
    fn a20_vmo_resize_page_count(vmo: *mut c_void) -> u32;
    fn a20_vmo_resize_slot(vmo: *mut c_void, index: u32) -> u32;
    fn a20_vmo_resize_alloc_table(count: u32) -> *mut c_void;
    fn a20_vmo_resize_free_table(table: *mut c_void);
    fn a20_vmo_resize_table_set(table: *mut c_void, index: u32, pfn: u32);
    fn a20_vmo_resize_publish(vmo: *mut c_void, table: *mut c_void, count: u32, size: u64) -> *mut c_void;
    fn a20_vmo_resize_flush_tlb();
    fn a20_vmo_resize_release_page(vmo: *mut c_void, index: u32);
    fn a20_vmo_resize_finish_shrink(vmo: *mut c_void);
    fn a20_vmo_destroy_notify(vmo: *mut c_void);
    fn a20_vmo_destroy_detach_pager(vmo: *mut c_void);
    fn a20_vmo_destroy_page_count(vmo: *mut c_void) -> u32;
    fn a20_vmo_destroy_page(vmo: *mut c_void, index: u32) -> u32;
    fn a20_vmo_destroy_phys_size(vmo: *mut c_void) -> u64;
    fn a20_vmo_destroy_charged_pages(vmo: *mut c_void) -> u64;
    fn a20_vmo_destroy_stats(phys_size: u64);
    fn a20_vmo_destroy_flush_tlb();
    fn a20_vmo_destroy_release_page(vmo: *mut c_void, index: u32, pfn: u32);
    fn a20_vmo_destroy_uncharge(vmo: *mut c_void, pages: u64);
    fn a20_vmo_destroy_free_pages(vmo: *mut c_void);
    fn a20_vmo_destroy_free_object(vmo: *mut c_void);
}

struct VmoLock {
    vmo: *mut c_void,
    flags: u64,
}

impl VmoLock {
    unsafe fn acquire(vmo: *mut c_void, index: u32) -> Result<Self, i32> {
        if vmo.is_null() {
            return Err(EINVAL);
        }
        let mut flags = 0u64;
        // SAFETY: the C adapter validates the object/index before transferring
        // its irq-save lock release obligation to this guard.
        let rc = unsafe { a20_vmo_page_lock(vmo, index, &mut flags) };
        if rc != 0 {
            return Err(rc);
        }
        Ok(Self { vmo, flags })
    }

    fn slot(&self, index: u32) -> u32 {
        // SAFETY: this guard owns the VMO lock and acquire() validated index.
        unsafe { a20_vmo_page_slot_locked(self.vmo, index) }
    }

    fn is_paged(&self) -> bool {
        // SAFETY: this guard owns the VMO lock.
        unsafe { a20_vmo_page_is_paged_locked(self.vmo) != 0 }
    }
}

impl Drop for VmoLock {
    fn drop(&mut self) {
        // SAFETY: acquire() succeeded and this guard has not been released.
        unsafe { a20_vmo_page_unlock(self.vmo, self.flags) }
    }
}

/// Resources are acquired while the VMO lock is held. Declaring this guard
/// after VmoLock makes it roll back before the lock is released on early exit.
struct PageTxn {
    vmo: *mut c_void,
    pfn: u32,
    cgroup: *mut c_void,
    charged: bool,
    committed: bool,
}

impl PageTxn {
    fn new(vmo: *mut c_void, cgroup: *mut c_void) -> Self {
        Self { vmo, pfn: PFN_NONE, cgroup, charged: false, committed: false }
    }
}

impl Drop for PageTxn {
    fn drop(&mut self) {
        if self.committed {
            return;
        }
        // SAFETY: each release corresponds to a successful reserve recorded
        // immediately after the callback succeeds.
        unsafe {
            if self.pfn != PFN_NONE {
                a20_vmo_page_free(self.pfn);
            }
            if self.charged {
                a20_vmo_page_uncharge(self.vmo, self.cgroup);
            }
        }
    }
}

unsafe fn materialize(
    vmo: *mut c_void,
    index: u32,
    cgroup: *mut c_void,
    charged: bool,
    fallback_without_pager: bool,
) -> Result<u32, i32> {
    let first = unsafe { VmoLock::acquire(vmo, index) }?;
    let existing = first.slot(index);
    if existing != PFN_NONE {
        return Ok(existing);
    }
    let paged = first.is_paged();
    drop(first);

    if paged {
        // The pager may park; never call it while the VMO lock is held.
        let page_rc = unsafe { a20_vmo_page_paged_fault(vmo, index, 1) };
        if page_rc == 0 {
            let after_fault = unsafe { VmoLock::acquire(vmo, index) }?;
            let existing = after_fault.slot(index);
            if existing != PFN_NONE {
                return Ok(existing);
            }
            drop(after_fault);
            if !fallback_without_pager {
                return Err(EIO);
            }
        } else if page_rc != EAGAIN || !fallback_without_pager {
            return Err(EIO);
        }
    }

    let lock = unsafe { VmoLock::acquire(vmo, index) }?;
    // Recheck after dropping the lock for the pager probe. This prevents two
    // concurrent first-touch paths from replacing and leaking a canonical page.
    let existing = lock.slot(index);
    if existing != PFN_NONE {
        return Ok(existing);
    }

    // Reserve sparse metadata / the accounting owner before acquiring a
    // charge or frame. Publish below remains a non-failing ownership transfer.
    let prepared = unsafe { a20_vmo_page_prepare_locked(vmo, index, cgroup) };
    if prepared != 0 { return Err(prepared); }
    let mut txn = PageTxn::new(vmo, cgroup);
    if charged && !cgroup.is_null() {
        // SAFETY: the cgroup pointer is borrowed from the fault caller for this
        // synchronous call; the matching release is owned by PageTxn.
        let rc = unsafe { a20_vmo_page_charge(vmo, cgroup) };
        if rc != 0 {
            return Err(ENOMEM);
        }
        txn.charged = true;
    }

    // SAFETY: this narrow primitive returns a frame owned by the transaction.
    txn.pfn = unsafe { a20_vmo_page_alloc() };
    if txn.pfn == PFN_NONE {
        return Err(ENOMEM);
    }
    // SAFETY: the newly allocated frame is private to this transaction.
    unsafe { a20_vmo_page_zero(txn.pfn) };
    // This callback is the non-failing publish point and updates the canonical
    // slot, accounting counters, and optional cgroup owner under the VMO lock.
    unsafe { a20_vmo_page_publish_locked(vmo, index, txn.pfn, cgroup) };
    txn.committed = true;
    Ok(txn.pfn)
}

#[no_mangle]
pub unsafe extern "C" fn a20_vmo_get_page_rust(vmo: *mut c_void, index: u32) -> u32 {
    match unsafe { materialize(vmo, index, ptr::null_mut(), false, false) } {
        Ok(pfn) => pfn,
        Err(_) => PFN_NONE,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_vmo_get_page_charged_rust(
    vmo: *mut c_void,
    index: u32,
    cgroup: *mut c_void,
    out: *mut u32,
) -> i32 {
    if out.is_null() {
        return EINVAL;
    }
    match unsafe { materialize(vmo, index, cgroup, true, true) } {
        Ok(pfn) => {
            // SAFETY: the C wrapper validates out before calling this function.
            unsafe { out.write(pfn) };
            0
        }
        Err(error) => error,
    }
}

struct ResizeLock {
    vmo: *mut c_void,
    flags: u64,
}

impl ResizeLock {
    unsafe fn acquire(vmo: *mut c_void) -> Result<Self, i64> {
        if vmo.is_null() { return Err(-1); }
        let mut flags = 0;
        let rc = unsafe { a20_vmo_resize_lock(vmo, &mut flags) };
        if rc != 0 { return Err(-1); }
        Ok(Self { vmo, flags })
    }
}

impl Drop for ResizeLock {
    fn drop(&mut self) {
        unsafe { a20_vmo_resize_unlock(self.vmo, self.flags) }
    }
}

struct PageTableTxn {
    table: *mut c_void,
    committed: bool,
}

impl Drop for PageTableTxn {
    fn drop(&mut self) {
        if !self.committed && !self.table.is_null() {
            unsafe { a20_vmo_resize_free_table(self.table) }
        }
    }
}

unsafe fn resize(vmo: *mut c_void, new_size: u64) -> i64 {
    let Some(rounded) = new_size.checked_add(PAGE_SIZE - 1) else { return -1; };
    let new_count64 = rounded / PAGE_SIZE;
    if new_count64 > u32::MAX as u64 || new_count64 > (usize::MAX / 4) as u64 {
        return -1;
    }
    let new_count = new_count64 as u32;
    let lock = match unsafe { ResizeLock::acquire(vmo) } { Ok(lock) => lock, Err(rc) => return rc };
    let old_count = unsafe { a20_vmo_resize_page_count(vmo) };

    if new_count <= old_count {
        let mut has_pages = false;
        for index in new_count..old_count {
            if unsafe { a20_vmo_resize_slot(vmo, index) } != PFN_NONE {
                has_pages = true;
                break;
            }
        }
        // Remove translations before returning any backing frames to the buddy.
        if has_pages { unsafe { a20_vmo_resize_flush_tlb() }; }
        for index in new_count..old_count {
            if unsafe { a20_vmo_resize_slot(vmo, index) } != PFN_NONE {
                unsafe { a20_vmo_resize_release_page(vmo, index) };
            }
        }
        unsafe { a20_vmo_resize_finish_shrink(vmo) };
        // Publish the new logical extent after all removed pages are released.
        // The callback keeps these scalar/list updates under the same lock.
        unsafe { a20_vmo_resize_publish(vmo, ptr::null_mut(), new_count, new_size) };
        drop(lock);
        return 0;
    }

    let table = unsafe { a20_vmo_resize_alloc_table(new_count) };
    if table.is_null() { return -1; }
    let mut txn = PageTableTxn { table, committed: false };
    for index in 0..old_count {
        let pfn = unsafe { a20_vmo_resize_slot(vmo, index) };
        unsafe { a20_vmo_resize_table_set(table, index, pfn) };
    }
    for index in old_count..new_count {
        unsafe { a20_vmo_resize_table_set(table, index, PFN_NONE) };
    }
    let old_table = unsafe { a20_vmo_resize_publish(vmo, table, new_count, new_size) };
    txn.committed = true;
    drop(lock);
    if !old_table.is_null() { unsafe { a20_vmo_resize_free_table(old_table) }; }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_vmo_resize_rust(vmo: *mut c_void, new_size: u64) -> i64 {
    unsafe { resize(vmo, new_size) }
}

unsafe fn destroy(vmo: *mut c_void) {
    if vmo.is_null() { return; }
    unsafe { a20_vmo_destroy_notify(vmo) };
    unsafe { a20_vmo_destroy_detach_pager(vmo) };
    let count = unsafe { a20_vmo_destroy_page_count(vmo) };
    let phys_size = unsafe { a20_vmo_destroy_phys_size(vmo) };
    let charged_pages = unsafe { a20_vmo_destroy_charged_pages(vmo) };
    unsafe { a20_vmo_destroy_stats(phys_size) };

    // Sparse VMOs expose no dense iteration range, but phys_size still
    // proves that their owner must cross the TLB barrier before tree drop.
    let mut has_pages = phys_size != 0;
    for index in 0..count {
        if unsafe { a20_vmo_destroy_page(vmo, index) } != PFN_NONE {
            has_pages = true;
            break;
        }
    }
    // Clear stale translations before any VMO-owned frame can be reused.
    if has_pages { unsafe { a20_vmo_destroy_flush_tlb() }; }
    for index in 0..count {
        let pfn = unsafe { a20_vmo_destroy_page(vmo, index) };
        if pfn != PFN_NONE {
            unsafe { a20_vmo_destroy_release_page(vmo, index, pfn) };
        }
    }
    unsafe { a20_vmo_destroy_free_pages(vmo) };
    if charged_pages != 0 {
        unsafe { a20_vmo_destroy_uncharge(vmo, charged_pages) };
    }
    unsafe { a20_vmo_destroy_free_object(vmo) };
}

#[no_mangle]
pub unsafe extern "C" fn a20_vmo_destroy_rust(vmo: *mut c_void) {
    unsafe { destroy(vmo) }
}
