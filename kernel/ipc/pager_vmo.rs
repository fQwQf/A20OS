#![no_std]

//! Rust-owned attach/detach and shutdown transactions for pager-backed VMOs.
//! The pager lock precedes the VMO lock whenever both are held. C callbacks
//! expose only locks, fields, allocation, references, and wake primitives.

mod pager_supply;

use core::ffi::c_void;

const INVALID_ARGUMENT: i32 = -12;
const BUSY: i32 = -9;
const NO_MEMORY: i32 = -6;
const CANCELED: i32 = -20;
const VMO_PAGED: u32 = 2;

unsafe extern "C" {
    fn a20_pager_vmo_lock_pager(pager: *mut c_void, flags: *mut u64) -> i32;
    fn a20_pager_vmo_unlock_pager(pager: *mut c_void, flags: u64);
    fn a20_pager_vmo_lock_vmo(vmo: *mut c_void, flags: *mut u64) -> i32;
    fn a20_pager_vmo_unlock_vmo(vmo: *mut c_void, flags: u64);
    fn a20_pager_vmo_type_locked(vmo: *mut c_void) -> u32;
    fn a20_pager_vmo_get_locked(vmo: *mut c_void) -> *mut c_void;
    fn a20_pager_vmo_set_locked(vmo: *mut c_void, pager: *mut c_void);
    fn a20_pager_vmo_dead_locked(pager: *mut c_void) -> i32;
    fn a20_pager_vmo_set_dead_locked(pager: *mut c_void);
    fn a20_pager_vmo_count_locked(pager: *mut c_void) -> u32;
    fn a20_pager_vmo_capacity_locked(pager: *mut c_void) -> u32;
    fn a20_pager_vmo_slot_locked(pager: *mut c_void, index: u32) -> *mut c_void;
    fn a20_pager_vmo_set_slot_locked(pager: *mut c_void, index: u32, vmo: *mut c_void);
    fn a20_pager_vmo_set_count_locked(pager: *mut c_void, count: u32);
    fn a20_pager_vmo_publish_list_locked(pager: *mut c_void, list: *mut c_void, cap: u32)
        -> *mut c_void;
    fn a20_pager_vmo_alloc_list(cap: u32) -> *mut c_void;
    fn a20_pager_vmo_free_list(list: *mut c_void);
    fn a20_pager_vmo_ref_pager(pager: *mut c_void);
    fn a20_pager_vmo_put_pager(pager: *mut c_void);
    fn a20_pager_vmo_try_ref_vmo(vmo: *mut c_void) -> i32;
    fn a20_pager_vmo_put_vmo(vmo: *mut c_void);
    fn a20_pager_vmo_wake(vmo: *mut c_void);
}

unsafe fn lock_pager(pager: *mut c_void) -> Result<u64, i32> {
    let mut flags = 0;
    let rc = unsafe { a20_pager_vmo_lock_pager(pager, &mut flags) };
    if rc == 0 { Ok(flags) } else { Err(rc) }
}
unsafe fn lock_vmo(vmo: *mut c_void) -> Result<u64, i32> {
    let mut flags = 0;
    let rc = unsafe { a20_pager_vmo_lock_vmo(vmo, &mut flags) };
    if rc == 0 { Ok(flags) } else { Err(rc) }
}

unsafe fn append_locked(pager: *mut c_void, vmo: *mut c_void) {
    let count = unsafe { a20_pager_vmo_count_locked(pager) };
    unsafe {
        a20_pager_vmo_set_slot_locked(pager, count, vmo);
        a20_pager_vmo_set_count_locked(pager, count + 1);
        a20_pager_vmo_ref_pager(pager);
        a20_pager_vmo_set_locked(vmo, pager);
    }
}

/// Attach one PAGED VMO. Capacity growth allocates outside both spinlocks,
/// then revalidates and commits the new list and VMO reference atomically.
#[no_mangle]
pub unsafe extern "C" fn a20_pager_attach_vmo_rust(
    pager: *mut c_void, vmo: *mut c_void,
) -> i32 {
    if pager.is_null() || vmo.is_null() { return INVALID_ARGUMENT; }
    loop {
        let pf = match unsafe { lock_pager(pager) } { Ok(f) => f, Err(e) => return e };
        if unsafe { a20_pager_vmo_dead_locked(pager) } != 0 {
            unsafe { a20_pager_vmo_unlock_pager(pager, pf) };
            return CANCELED;
        }
        let vf = match unsafe { lock_vmo(vmo) } {
            Ok(f) => f,
            Err(e) => { unsafe { a20_pager_vmo_unlock_pager(pager, pf) }; return e; }
        };
        let kind = unsafe { a20_pager_vmo_type_locked(vmo) };
        let attached = unsafe { a20_pager_vmo_get_locked(vmo) };
        if kind != VMO_PAGED {
            unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); }
            return INVALID_ARGUMENT;
        }
        if !attached.is_null() {
            unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); }
            return BUSY;
        }
        let count = unsafe { a20_pager_vmo_count_locked(pager) };
        let cap = unsafe { a20_pager_vmo_capacity_locked(pager) };
        if count < cap {
            unsafe { append_locked(pager, vmo); }
            unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); }
            return 0;
        }
        unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); }

        let new_cap = if cap == 0 { 4 } else { cap.saturating_mul(2) };
        if new_cap <= cap { return NO_MEMORY; }
        let new_list = unsafe { a20_pager_vmo_alloc_list(new_cap) };
        if new_list.is_null() { return NO_MEMORY; }

        let pf = match unsafe { lock_pager(pager) } {
            Ok(f) => f,
            Err(e) => { unsafe { a20_pager_vmo_free_list(new_list) }; return e; }
        };
        if unsafe { a20_pager_vmo_dead_locked(pager) } != 0 {
            unsafe { a20_pager_vmo_unlock_pager(pager, pf); a20_pager_vmo_free_list(new_list); }
            return CANCELED;
        }
        let vf = match unsafe { lock_vmo(vmo) } {
            Ok(f) => f,
            Err(e) => { unsafe { a20_pager_vmo_unlock_pager(pager, pf); a20_pager_vmo_free_list(new_list); } return e; }
        };
        if unsafe { a20_pager_vmo_type_locked(vmo) } != VMO_PAGED {
            unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); a20_pager_vmo_free_list(new_list); }
            return INVALID_ARGUMENT;
        }
        if !unsafe { a20_pager_vmo_get_locked(vmo) }.is_null() {
            unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); a20_pager_vmo_free_list(new_list); }
            return BUSY;
        }
        let current_count = unsafe { a20_pager_vmo_count_locked(pager) };
        let current_cap = unsafe { a20_pager_vmo_capacity_locked(pager) };
        if current_count < current_cap {
            unsafe { append_locked(pager, vmo); }
            unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); a20_pager_vmo_free_list(new_list); }
            return 0;
        }
        if current_cap != cap || new_cap <= current_count {
            unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); a20_pager_vmo_free_list(new_list); }
            continue;
        }
        for i in 0..current_count {
            let old = unsafe { a20_pager_vmo_slot_locked(pager, i) };
            unsafe { (new_list.cast::<*mut c_void>().add(i as usize)).write(old); }
        }
        let old_list = unsafe { a20_pager_vmo_publish_list_locked(pager, new_list, new_cap) };
        unsafe { append_locked(pager, vmo); }
        unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); }
        if !old_list.is_null() { unsafe { a20_pager_vmo_free_list(old_list); } }
        return 0;
    }
}

/// Detach one VMO, clearing the association while locked and doing wake/ref
/// release after unlocking. A temporary pager reference protects recheck.
#[no_mangle]
pub unsafe extern "C" fn a20_pager_detach_vmo_rust(vmo: *mut c_void) {
    if vmo.is_null() { return; }
    let vf = match unsafe { lock_vmo(vmo) } { Ok(f) => f, Err(_) => return };
    let pager = unsafe { a20_pager_vmo_get_locked(vmo) };
    if pager.is_null() {
        unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); }
        return;
    }
    unsafe { a20_pager_vmo_ref_pager(pager); a20_pager_vmo_unlock_vmo(vmo, vf); }
    let pf = match unsafe { lock_pager(pager) } {
        Ok(f) => f,
        Err(_) => { unsafe { a20_pager_vmo_put_pager(pager); } return; }
    };
    let vf = match unsafe { lock_vmo(vmo) } {
        Ok(f) => f,
        Err(_) => { unsafe { a20_pager_vmo_unlock_pager(pager, pf); a20_pager_vmo_put_pager(pager); } return; }
    };
    if unsafe { a20_pager_vmo_get_locked(vmo) } != pager {
        unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); a20_pager_vmo_put_pager(pager); }
        return;
    }
    let count = unsafe { a20_pager_vmo_count_locked(pager) };
    let mut found = None;
    for i in 0..count {
        if unsafe { a20_pager_vmo_slot_locked(pager, i) } == vmo { found = Some(i); break; }
    }
    unsafe { a20_pager_vmo_set_locked(vmo, core::ptr::null_mut()); }
    if let Some(index) = found {
        for i in index..count - 1 {
            let next = unsafe { a20_pager_vmo_slot_locked(pager, i + 1) };
            unsafe { a20_pager_vmo_set_slot_locked(pager, i, next); }
        }
        unsafe {
            a20_pager_vmo_set_slot_locked(pager, count - 1, core::ptr::null_mut());
            a20_pager_vmo_set_count_locked(pager, count - 1);
        }
    }
    unsafe { a20_pager_vmo_unlock_vmo(vmo, vf); a20_pager_vmo_unlock_pager(pager, pf); }
    unsafe {
        a20_pager_vmo_wake(vmo);
        a20_pager_vmo_put_pager(pager); // VMO-held association reference
        a20_pager_vmo_put_pager(pager); // temporary recheck reference
    }
}

/// Called after a request observes peer closure. Mark the pager dead and
/// detach/wake every weakly registered VMO. The pager lock keeps each weak
/// pointer alive until its waiter queue has been woken; the pager itself is
/// pinned across the sweep, and attachment refs are dropped after unlock.
#[no_mangle]
pub unsafe extern "C" fn a20_pager_peer_closed_rust(pager: *mut c_void) {
    if pager.is_null() { return; }
    unsafe { a20_pager_vmo_ref_pager(pager); }
    loop {
        let pf = match unsafe { lock_pager(pager) } { Ok(f) => f, Err(_) => break };
        unsafe { a20_pager_vmo_set_dead_locked(pager); }
        let count = unsafe { a20_pager_vmo_count_locked(pager) };
        if count == 0 {
            unsafe { a20_pager_vmo_unlock_pager(pager, pf); }
            break;
        }
        let vmo = unsafe { a20_pager_vmo_slot_locked(pager, 0) };
        if vmo.is_null() {
            unsafe {
                a20_pager_vmo_set_count_locked(pager, count - 1);
                a20_pager_vmo_unlock_pager(pager, pf);
            }
            continue;
        }
        let vf = match unsafe { lock_vmo(vmo) } {
            Ok(f) => f,
            Err(_) => { unsafe { a20_pager_vmo_unlock_pager(pager, pf); } break; }
        };
        let attached = unsafe { a20_pager_vmo_get_locked(vmo) } == pager;
        let wake_pin = attached && unsafe { a20_pager_vmo_try_ref_vmo(vmo) } != 0;
        if attached { unsafe { a20_pager_vmo_set_locked(vmo, core::ptr::null_mut()); } }
        for i in 0..count - 1 {
            let next = unsafe { a20_pager_vmo_slot_locked(pager, i + 1) };
            unsafe { a20_pager_vmo_set_slot_locked(pager, i, next); }
        }
        unsafe {
            a20_pager_vmo_set_slot_locked(pager, count - 1, core::ptr::null_mut());
            a20_pager_vmo_set_count_locked(pager, count - 1);
            a20_pager_vmo_unlock_vmo(vmo, vf);
        }
        unsafe { a20_pager_vmo_unlock_pager(pager, pf); }
        if attached {
            if wake_pin {
                unsafe { a20_pager_vmo_wake(vmo); a20_pager_vmo_put_vmo(vmo); }
            }
            unsafe { a20_pager_vmo_put_pager(pager); }
        }
    }
    unsafe { a20_pager_vmo_put_pager(pager); }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::sync::atomic::{AtomicBool, AtomicU32, AtomicUsize, Ordering};

    const PAGER: *mut c_void = 1usize as *mut c_void;
    const VMO1: *mut c_void = 2usize as *mut c_void;
    const VMO2: *mut c_void = 3usize as *mut c_void;
    static P_LOCK: AtomicBool = AtomicBool::new(false);
    static V_LOCK: AtomicBool = AtomicBool::new(false);
    static DEAD: AtomicBool = AtomicBool::new(false);
    static FAIL_ALLOC: AtomicBool = AtomicBool::new(false);
    static TYPE1: AtomicU32 = AtomicU32::new(VMO_PAGED);
    static TYPE2: AtomicU32 = AtomicU32::new(VMO_PAGED);
    static REFS: AtomicU32 = AtomicU32::new(1);
    static COUNT: AtomicU32 = AtomicU32::new(0);
    static CAP: AtomicU32 = AtomicU32::new(0);
    static VP1: AtomicUsize = AtomicUsize::new(0);
    static VP2: AtomicUsize = AtomicUsize::new(0);
    static WAKES: AtomicU32 = AtomicU32::new(0);
    static VMO_PUTS: AtomicU32 = AtomicU32::new(0);
    static LIVE1: AtomicU32 = AtomicU32::new(1);
    static LIVE2: AtomicU32 = AtomicU32::new(1);
    static mut LIST: [usize; 16] = [0; 16];

    fn vp(vmo: *mut c_void) -> &'static AtomicUsize {
        if vmo == VMO1 { &VP1 } else { &VP2 }
    }
    fn reset() {
        P_LOCK.store(false, Ordering::SeqCst);
        V_LOCK.store(false, Ordering::SeqCst);
        DEAD.store(false, Ordering::SeqCst);
        FAIL_ALLOC.store(false, Ordering::SeqCst);
        TYPE1.store(VMO_PAGED, Ordering::SeqCst);
        TYPE2.store(VMO_PAGED, Ordering::SeqCst);
        REFS.store(1, Ordering::SeqCst);
        COUNT.store(0, Ordering::SeqCst);
        CAP.store(0, Ordering::SeqCst);
        VP1.store(0, Ordering::SeqCst);
        VP2.store(0, Ordering::SeqCst);
        WAKES.store(0, Ordering::SeqCst);
        VMO_PUTS.store(0, Ordering::SeqCst);
        LIVE1.store(1, Ordering::SeqCst);
        LIVE2.store(1, Ordering::SeqCst);
        unsafe { LIST = [0; 16]; }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_lock_pager(_: *mut c_void, flags: *mut u64) -> i32 {
        assert!(!V_LOCK.load(Ordering::SeqCst));
        assert!(!P_LOCK.swap(true, Ordering::SeqCst));
        unsafe { flags.write(0); }
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_unlock_pager(_: *mut c_void, _: u64) {
        assert!(P_LOCK.swap(false, Ordering::SeqCst));
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_lock_vmo(_: *mut c_void, flags: *mut u64) -> i32 {
        assert!(!V_LOCK.swap(true, Ordering::SeqCst));
        unsafe { flags.write(0); }
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_unlock_vmo(_: *mut c_void, _: u64) {
        assert!(V_LOCK.swap(false, Ordering::SeqCst));
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_type_locked(vmo: *mut c_void) -> u32 {
        if vmo == VMO1 { TYPE1.load(Ordering::SeqCst) } else { TYPE2.load(Ordering::SeqCst) }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_get_locked(vmo: *mut c_void) -> *mut c_void {
        vp(vmo).load(Ordering::SeqCst) as *mut c_void
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_set_locked(vmo: *mut c_void, pager: *mut c_void) {
        vp(vmo).store(pager as usize, Ordering::SeqCst);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_dead_locked(_: *mut c_void) -> i32 {
        DEAD.load(Ordering::SeqCst) as i32
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_set_dead_locked(_: *mut c_void) {
        DEAD.store(true, Ordering::SeqCst);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_count_locked(_: *mut c_void) -> u32 {
        COUNT.load(Ordering::SeqCst)
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_capacity_locked(_: *mut c_void) -> u32 {
        CAP.load(Ordering::SeqCst)
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_slot_locked(_: *mut c_void, i: u32) -> *mut c_void {
        unsafe { LIST[i as usize] as *mut c_void }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_set_slot_locked(_: *mut c_void, i: u32, vmo: *mut c_void) {
        unsafe { LIST[i as usize] = vmo as usize; }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_set_count_locked(_: *mut c_void, n: u32) {
        COUNT.store(n, Ordering::SeqCst);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_publish_list_locked(
        _: *mut c_void, list: *mut c_void, cap: u32,
    ) -> *mut c_void {
        CAP.store(cap, Ordering::SeqCst);
        list
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_alloc_list(cap: u32) -> *mut c_void {
        if FAIL_ALLOC.load(Ordering::SeqCst) || cap > 16 { return core::ptr::null_mut(); }
        core::ptr::addr_of_mut!(LIST).cast::<c_void>()
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_free_list(_: *mut c_void) {}
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_ref_pager(_: *mut c_void) {
        REFS.fetch_add(1, Ordering::SeqCst);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_put_pager(_: *mut c_void) {
        assert!(!P_LOCK.load(Ordering::SeqCst));
        assert!(!V_LOCK.load(Ordering::SeqCst));
        REFS.fetch_sub(1, Ordering::SeqCst);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_try_ref_vmo(vmo: *mut c_void) -> i32 {
        let refs = if vmo == VMO1 { &LIVE1 } else { &LIVE2 };
        let mut old = refs.load(Ordering::SeqCst);
        while old != 0 {
            match refs.compare_exchange_weak(old, old + 1, Ordering::SeqCst, Ordering::SeqCst) {
                Ok(_) => return 1,
                Err(now) => old = now,
            }
        }
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_put_vmo(vmo: *mut c_void) {
        assert!(!P_LOCK.load(Ordering::SeqCst));
        assert!(!V_LOCK.load(Ordering::SeqCst));
        let refs = if vmo == VMO1 { &LIVE1 } else { &LIVE2 };
        refs.fetch_sub(1, Ordering::SeqCst);
        VMO_PUTS.fetch_add(1, Ordering::SeqCst);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_pager_vmo_wake(_: *mut c_void) {
        assert!(!P_LOCK.load(Ordering::SeqCst));
        assert!(!V_LOCK.load(Ordering::SeqCst));
        WAKES.fetch_add(1, Ordering::SeqCst);
    }

    #[test]
    fn attach_validates_allocates_and_keeps_refcounts_balanced() {
        reset();
        unsafe {
            assert_eq!(a20_pager_attach_vmo_rust(core::ptr::null_mut(), VMO1), INVALID_ARGUMENT);
            TYPE1.store(0, Ordering::SeqCst);
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO1), INVALID_ARGUMENT);
            TYPE1.store(VMO_PAGED, Ordering::SeqCst);
            FAIL_ALLOC.store(true, Ordering::SeqCst);
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO1), NO_MEMORY);
            assert_eq!(COUNT.load(Ordering::SeqCst), 0);
            assert_eq!(REFS.load(Ordering::SeqCst), 1);
            FAIL_ALLOC.store(false, Ordering::SeqCst);
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO1), 0);
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO1), BUSY);
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO2), 0);
        }
        assert_eq!(COUNT.load(Ordering::SeqCst), 2);
        assert_eq!(REFS.load(Ordering::SeqCst), 3);
        assert_eq!(VP1.load(Ordering::SeqCst), PAGER as usize);
        assert_eq!(VP2.load(Ordering::SeqCst), PAGER as usize);
    }

    #[test]
    fn detach_and_peer_close_wake_and_drop_refs_after_unlock() {
        reset();
        unsafe {
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO1), 0);
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO2), 0);
            a20_pager_detach_vmo_rust(VMO1);
            assert_eq!(VP1.load(Ordering::SeqCst), 0);
            assert_eq!(COUNT.load(Ordering::SeqCst), 1);
            assert_eq!(WAKES.load(Ordering::SeqCst), 1);
            assert_eq!(REFS.load(Ordering::SeqCst), 2);
            a20_pager_peer_closed_rust(PAGER);
            assert_eq!(DEAD.load(Ordering::SeqCst), true);
            assert_eq!(VP2.load(Ordering::SeqCst), 0);
            assert_eq!(COUNT.load(Ordering::SeqCst), 0);
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO1), CANCELED);
        }
        assert_eq!(WAKES.load(Ordering::SeqCst), 2);
        assert_eq!(VMO_PUTS.load(Ordering::SeqCst), 1);
        assert_eq!(REFS.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn peer_close_does_not_resurrect_a_final_vmo_reference() {
        reset();
        unsafe {
            assert_eq!(a20_pager_attach_vmo_rust(PAGER, VMO1), 0);
            /* Model the final release winning its refcount decrement just
             * before the weak registry sweep reaches this object. */
            LIVE1.store(0, Ordering::SeqCst);
            a20_pager_peer_closed_rust(PAGER);
        }
        assert_eq!(VP1.load(Ordering::SeqCst), 0);
        assert_eq!(COUNT.load(Ordering::SeqCst), 0);
        assert_eq!(WAKES.load(Ordering::SeqCst), 0);
        assert_eq!(VMO_PUTS.load(Ordering::SeqCst), 0);
        assert_eq!(REFS.load(Ordering::SeqCst), 1);
    }
}
