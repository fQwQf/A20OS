#![allow(dead_code)]

//! Native pager page-supply transaction. Rust validates the complete request,
//! checks association under the VMO lock, accounts partial progress, and
//! chooses the waiter wake policy. C callbacks are limited to VMO/physical
//! page fields, allocation/copy/statistics, and the wait-queue primitive.

use core::ffi::c_void;

const PAGE_SIZE: u64 = 4096;
const VMO_PAGED: u32 = 2;
const INVALID_ARGUMENT: i64 = -12;
const BAD_HANDLE: i64 = -5;
const RANGE: i64 = -22;
const TYPE_MISMATCH: i64 = -23;
const NO_PAGE: u64 = u32::MAX as u64;

unsafe extern "C" {
    fn a20_pager_supply_meta(vmo: *mut c_void, kind: *mut u32, size: *mut u64);
    fn a20_pager_supply_lock(vmo: *mut c_void, flags: *mut u64) -> i32;
    fn a20_pager_supply_unlock(vmo: *mut c_void, flags: u64);
    fn a20_pager_supply_pager_locked(vmo: *mut c_void) -> *mut c_void;
    fn a20_pager_supply_dst_page(vmo: *mut c_void, index: u32) -> u64;
    fn a20_pager_supply_src_page(src: *mut c_void, index: u32) -> u64;
    fn a20_pager_supply_alloc_page() -> u64;
    fn a20_pager_supply_copy_install(vmo: *mut c_void, index: u32,
                                     src_page: u64, dst_page: u64);
    fn a20_pager_supply_wake(vmo: *mut c_void, first_page: u64, keyed: i32);
}

struct SupplyVmoLock { vmo: *mut c_void, flags: u64 }

impl SupplyVmoLock {
    unsafe fn acquire(vmo: *mut c_void) -> Result<Self, i32> {
        let mut flags = 0;
        let rc = unsafe { a20_pager_supply_lock(vmo, &mut flags) };
        if rc == 0 { Ok(Self { vmo, flags }) } else { Err(rc) }
    }
}

impl Drop for SupplyVmoLock {
    fn drop(&mut self) {
        unsafe { a20_pager_supply_unlock(self.vmo, self.flags); }
    }
}

/// Supply as many consecutive materialized source pages as possible.
/// Existing destination pages count as supplied, matching the Native ABI's
/// idempotent page-supply behavior. Allocation/source exhaustion returns the
/// completed prefix, and only successful progress wakes faulters.
#[no_mangle]
pub unsafe extern "C" fn a20_pager_supply_pages_rust(
    pager: *mut c_void, vmo: *mut c_void, src: *mut c_void,
    vmo_offset: u64, src_offset: u64, len: u64,
) -> i64 {
    if pager.is_null() || vmo.is_null() || src.is_null() { return INVALID_ARGUMENT; }
    let (mut kind, mut vmo_size) = (0, 0);
    unsafe { a20_pager_supply_meta(vmo, &mut kind, &mut vmo_size); }
    if kind != VMO_PAGED { return TYPE_MISMATCH; }
    if vmo_offset % PAGE_SIZE != 0 || src_offset % PAGE_SIZE != 0 ||
       len % PAGE_SIZE != 0 || len == 0 { return INVALID_ARGUMENT; }
    if vmo_offset >= vmo_size || len > vmo_size - vmo_offset { return RANGE; }
    let (mut _src_kind, mut src_size) = (0, 0);
    unsafe { a20_pager_supply_meta(src, &mut _src_kind, &mut src_size); }
    if src_offset >= src_size || len > src_size - src_offset { return RANGE; }

    let _lock = match unsafe { SupplyVmoLock::acquire(vmo) } {
        Ok(lock) => lock,
        Err(rc) => return rc as i64,
    };
    if unsafe { a20_pager_supply_pager_locked(vmo) } != pager {
        return BAD_HANDLE;
    }
    let mut supplied = 0u64;
    let mut off = 0u64;
    while off < len {
        let dst_idx = ((vmo_offset + off) / PAGE_SIZE) as u32;
        let src_idx = ((src_offset + off) / PAGE_SIZE) as u32;
        let dst = unsafe { a20_pager_supply_dst_page(vmo, dst_idx) };
        if dst != NO_PAGE {
            supplied += PAGE_SIZE;
            off += PAGE_SIZE;
            continue;
        }
        let source = unsafe { a20_pager_supply_src_page(src, src_idx) };
        if source == NO_PAGE { break; }
        let allocated = unsafe { a20_pager_supply_alloc_page() };
        if allocated == NO_PAGE { break; }
        unsafe { a20_pager_supply_copy_install(vmo, dst_idx, source, allocated); }
        supplied += PAGE_SIZE;
        off += PAGE_SIZE;
    }
    drop(_lock);
    if supplied != 0 {
        let keyed = (supplied == PAGE_SIZE) as i32;
        unsafe { a20_pager_supply_wake(vmo, vmo_offset / PAGE_SIZE, keyed); }
    }
    supplied as i64
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};

    const PAGER: *mut c_void = 1usize as *mut c_void;
    const VMO: *mut c_void = 2usize as *mut c_void;
    const SRC: *mut c_void = 3usize as *mut c_void;
    static LOCKED: AtomicBool = AtomicBool::new(false);
    static VMO_SIZE: AtomicU64 = AtomicU64::new(0);
    static SRC_SIZE: AtomicU64 = AtomicU64::new(0);
    static ATTACHED: AtomicUsize = AtomicUsize::new(1);
    static ALLOC_COUNT: AtomicU32 = AtomicU32::new(0);
    static FAIL_ALLOC_AT: AtomicU32 = AtomicU32::new(u32::MAX);
    static SOURCE_MISSING_AT: AtomicU32 = AtomicU32::new(u32::MAX);
    static WAKE_COUNT: AtomicU32 = AtomicU32::new(0);
    static WAKE_KEYED: AtomicU32 = AtomicU32::new(0);
    static WAKE_PAGE: AtomicU64 = AtomicU64::new(0);
    static mut DST: [u32; 4] = [u32::MAX; 4];
    static mut SOURCE: [u32; 4] = [20, 21, 22, 23];

    use core::sync::atomic::AtomicUsize;

    fn reset() {
        LOCKED.store(false, Ordering::SeqCst);
        VMO_SIZE.store(4 * PAGE_SIZE, Ordering::SeqCst);
        SRC_SIZE.store(4 * PAGE_SIZE, Ordering::SeqCst);
        ATTACHED.store(PAGER as usize, Ordering::SeqCst);
        ALLOC_COUNT.store(0, Ordering::SeqCst);
        FAIL_ALLOC_AT.store(u32::MAX, Ordering::SeqCst);
        SOURCE_MISSING_AT.store(u32::MAX, Ordering::SeqCst);
        WAKE_COUNT.store(0, Ordering::SeqCst);
        WAKE_KEYED.store(0, Ordering::SeqCst);
        WAKE_PAGE.store(0, Ordering::SeqCst);
        unsafe { DST = [u32::MAX; 4]; SOURCE = [20, 21, 22, 23]; }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_meta(
        vmo: *mut c_void, kind: *mut u32, size: *mut u64,
    ) {
        unsafe {
            kind.write(if vmo == VMO { VMO_PAGED } else { 0 });
            size.write(if vmo == VMO { VMO_SIZE.load(Ordering::SeqCst) }
                       else { SRC_SIZE.load(Ordering::SeqCst) });
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_lock(_: *mut c_void, flags: *mut u64) -> i32 {
        assert!(!LOCKED.swap(true, Ordering::SeqCst));
        unsafe { flags.write(0); }
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_unlock(_: *mut c_void, _: u64) {
        assert!(LOCKED.swap(false, Ordering::SeqCst));
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_pager_locked(_: *mut c_void) -> *mut c_void {
        ATTACHED.load(Ordering::SeqCst) as *mut c_void
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_dst_page(_: *mut c_void, index: u32) -> u64 {
        unsafe { DST[index as usize] as u64 }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_src_page(_: *mut c_void, index: u32) -> u64 {
        if index == SOURCE_MISSING_AT.load(Ordering::SeqCst) { u32::MAX as u64 }
        else { unsafe { SOURCE[index as usize] as u64 } }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_alloc_page() -> u64 {
        let n = ALLOC_COUNT.fetch_add(1, Ordering::SeqCst);
        if n == FAIL_ALLOC_AT.load(Ordering::SeqCst) { u32::MAX as u64 }
        else { 100 + n as u64 }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_copy_install(
        _: *mut c_void, index: u32, _: u64, dst: u64,
    ) { unsafe { DST[index as usize] = dst as u32; } }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_supply_wake(_: *mut c_void, first: u64, keyed: i32) {
        assert!(!LOCKED.load(Ordering::SeqCst));
        WAKE_COUNT.fetch_add(1, Ordering::SeqCst);
        WAKE_KEYED.store(keyed as u32, Ordering::SeqCst);
        WAKE_PAGE.store(first, Ordering::SeqCst);
    }

    #[test]
    fn validates_prefix_progress_and_wake_contract() {
        reset();
        unsafe {
            assert_eq!(a20_pager_supply_pages_rust(PAGER, VMO, SRC, 0, 0, 0), INVALID_ARGUMENT);
            assert_eq!(a20_pager_supply_pages_rust(PAGER, VMO, SRC, 0, 0, 2), INVALID_ARGUMENT);
            ATTACHED.store(9, Ordering::SeqCst);
            assert_eq!(a20_pager_supply_pages_rust(PAGER, VMO, SRC, 0, 0, PAGE_SIZE), BAD_HANDLE);
            ATTACHED.store(PAGER as usize, Ordering::SeqCst);

            SOURCE_MISSING_AT.store(1, Ordering::SeqCst);
            assert_eq!(a20_pager_supply_pages_rust(PAGER, VMO, SRC, 0, 0, 3 * PAGE_SIZE), PAGE_SIZE as i64);
            assert_eq!(DST[0], 100);
            assert_eq!(DST[1], u32::MAX);
            assert_eq!(WAKE_COUNT.load(Ordering::SeqCst), 1);
            assert_eq!(WAKE_KEYED.load(Ordering::SeqCst), 1);
            assert_eq!(WAKE_PAGE.load(Ordering::SeqCst), 0);

            reset();
            DST[0] = 7;
            assert_eq!(a20_pager_supply_pages_rust(PAGER, VMO, SRC, 0, 0, 2 * PAGE_SIZE),
                       (2 * PAGE_SIZE) as i64);
            assert_eq!(DST[0], 7);
            assert_eq!(DST[1], 100);
            assert_eq!(WAKE_KEYED.load(Ordering::SeqCst), 0);
            assert_eq!(WAKE_COUNT.load(Ordering::SeqCst), 1);

            reset();
            FAIL_ALLOC_AT.store(1, Ordering::SeqCst);
            assert_eq!(a20_pager_supply_pages_rust(PAGER, VMO, SRC, PAGE_SIZE, 0,
                       2 * PAGE_SIZE), PAGE_SIZE as i64);
            assert_eq!(DST[1], 100);
            assert_eq!(DST[2], u32::MAX);
        }
        assert!(!LOCKED.load(Ordering::SeqCst));
    }
}
