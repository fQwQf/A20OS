//! Bounded lease, thread admission and page-return transactions. No blocking,
//! allocations, callbacks or kernel locks while the admission guard is held.
#![cfg_attr(not(test), no_std)]
use core::cell::UnsafeCell;
use core::sync::atomic::{AtomicBool, AtomicU64, Ordering};

static LEASE: AtomicU64 = AtomicU64::new(0);
static ROOT: AtomicU64 = AtomicU64::new(0);
static BUSY: AtomicBool = AtomicBool::new(false);
struct Threads {
    slots: [(u32, u32); 16],
    epoch: u64,
    limit: u32,
    slices: u32,
}
struct Global(UnsafeCell<Threads>);
unsafe impl Sync for Global {}
static THREADS: Global = Global(UnsafeCell::new(Threads {
    slots: [(0, 0); 16],
    epoch: 0,
    limit: 0,
    slices: 0,
}));
struct Guard;
impl Drop for Guard {
    fn drop(&mut self) {
        BUSY.store(false, Ordering::Release);
    }
}
fn lock() -> Guard {
    while BUSY
        .compare_exchange(false, true, Ordering::Acquire, Ordering::Relaxed)
        .is_err()
    {
        core::hint::spin_loop();
    }
    Guard
}
#[no_mangle]
pub extern "C" fn a20_ssi_state_configure(root: u32, limit: u32, slices: u32) -> i32 {
    let _g = lock();
    if LEASE.load(Ordering::Acquire) != 0 || limit > 16 || slices == 0 {
        return -16;
    }
    // SAFETY: all accesses to Threads hold the admission guard.
    let s = unsafe { &mut *THREADS.0.get() };
    s.slots = [(0, 0); 16];
    s.limit = limit;
    s.slices = slices;
    ROOT.store(root as u64, Ordering::Release);
    0
}
#[no_mangle]
pub extern "C" fn a20_ssi_state_managed(group: u32) -> i32 {
    (group != 0 && ROOT.load(Ordering::Acquire) == group as u64) as i32
}
#[no_mangle]
pub extern "C" fn a20_ssi_state_allowed(group: u32, pid: u32) -> i32 {
    let l = LEASE.load(Ordering::Acquire);
    (l == 0 || (l >> 32) as u32 != group || l as u32 == pid) as i32
}
#[no_mangle]
pub extern "C" fn a20_ssi_state_begin(group: u32, pid: u32) -> u64 {
    let _g = lock();
    if group == pid
        || pid == 0
        || ROOT.load(Ordering::Acquire) != group as u64
        || LEASE.load(Ordering::Acquire) != 0
    {
        return 0;
    }
    let s = unsafe { &mut *THREADS.0.get() };
    let n = match s.slots[..s.limit as usize].iter().position(|p| p.0 == pid) {
        Some(n) => n,
        None => match s.slots[..s.limit as usize].iter().position(|p| p.0 == 0) {
            Some(n) => {
                s.slots[n] = (pid, 0);
                n
            }
            None => return 0,
        },
    };
    if s.slots[n].1 >= s.slices || s.epoch == u64::MAX {
        return 0;
    }
    s.slots[n].1 += 1;
    s.epoch += 1;
    LEASE.store(((group as u64) << 32) | pid as u64, Ordering::Release);
    s.epoch
}
#[no_mangle]
pub extern "C" fn a20_ssi_state_finish(group: u32, pid: u32, epoch: u64) -> i32 {
    let _g = lock();
    let s = unsafe { &mut *THREADS.0.get() };
    if epoch != s.epoch || LEASE.load(Ordering::Acquire) != ((group as u64) << 32 | pid as u64) {
        return -22;
    }
    LEASE.store(0, Ordering::Release);
    0
}
#[no_mangle]
pub extern "C" fn a20_ssi_state_forget(pid: u32) {
    let _g = lock();
    let s = unsafe { &mut *THREADS.0.get() };
    for p in &mut s.slots {
        if p.0 == pid {
            *p = (0, 0);
        }
    }
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Page {
    pub va: u64,
    pub flags: u32,
    pub returned: u32,
}
#[repr(C)]
pub struct Pages {
    pub entries: [Page; 64],
    pub len: u32,
    pub limit: u32,
    pub bytes: u64,
    pub cap: u64,
    pub phase: u32,
    pub reserved: u32,
}
#[no_mangle]
pub unsafe extern "C" fn a20_ssi_pages_init(p: *mut Pages, limit: u32, cap: u64) -> i32 {
    if p.is_null() || limit == 0 || limit > 64 || cap < 4096 {
        return -22;
    }
    unsafe {
        p.write(Pages {
            entries: [Page {
                va: 0,
                flags: 0,
                returned: 0,
            }; 64],
            len: 0,
            limit,
            bytes: 0,
            cap,
            phase: 0,
            reserved: 0,
        });
    }
    0
}
#[no_mangle]
pub unsafe extern "C" fn a20_ssi_pages_charge(p: &mut Pages, bytes: u64) -> i32 {
    match p.bytes.checked_add(bytes) {
        Some(n) if n <= p.cap => {
            p.bytes = n;
            0
        }
        _ => -28,
    }
}
#[no_mangle]
pub unsafe extern "C" fn a20_ssi_pages_grant(p: &mut Pages, va: u64, flags: u32) -> i32 {
    if p.phase != 0 {
        return -22;
    }
    if va == 0 || va & 4095 != 0 || va >= 0x8000_0000_0000 || flags == 0 || flags & !7 != 0 {
        return -22;
    }
    if p.entries[..p.len as usize].iter().any(|e| e.va == va) {
        return -17;
    }
    if p.len >= p.limit {
        return -28;
    }
    p.entries[p.len as usize] = Page {
        va,
        flags,
        returned: 0,
    };
    p.len += 1;
    0
}
#[no_mangle]
pub unsafe extern "C" fn a20_ssi_pages_return(p: &mut Pages, va: u64) -> i32 {
    match p.entries[..p.len as usize].iter().position(|e| e.va == va) {
        Some(n) if p.entries[n].flags & 2 != 0 && p.entries[n].returned == 0 => {
            p.phase = 1;
            p.entries[n].returned = 1;
            n as i32
        }
        _ => -22,
    }
}
#[no_mangle]
pub unsafe extern "C" fn a20_ssi_pages_complete(p: &Pages) -> i32 {
    p.entries[..p.len as usize]
        .iter()
        .all(|e| e.flags & 2 == 0 || e.returned == 1) as i32
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn transactions_and_lease() {
        assert_eq!(a20_ssi_state_configure(10, 2, 2), 0);
        let e = a20_ssi_state_begin(10, 11);
        assert_ne!(e, 0);
        assert_eq!(a20_ssi_state_allowed(10, 10), 0);
        assert_eq!(a20_ssi_state_allowed(10, 11), 1);
        assert_eq!(a20_ssi_state_allowed(12, 10), 1);
        assert_eq!(a20_ssi_state_finish(10, 11, e + 1), -22);
        assert_eq!(a20_ssi_state_begin(10, 12), 0);
        assert_eq!(a20_ssi_state_finish(10, 11, e), 0);
        let e2 = a20_ssi_state_begin(10, 11);
        assert!(e2 > e);
        assert_eq!(a20_ssi_state_finish(10, 11, e2), 0);
        assert_eq!(a20_ssi_state_begin(10, 11), 0);
        a20_ssi_state_forget(11);
        let e3 = a20_ssi_state_begin(10, 13);
        assert_ne!(e3, 0);
        assert_eq!(a20_ssi_state_finish(10, 13, e3), 0);
        assert_eq!(a20_ssi_state_configure(20, 1, 2), 0);
        let e = a20_ssi_state_begin(20, 21);
        assert_ne!(e, 0);
        assert_eq!(a20_ssi_state_configure(30, 1, 2), -16);
        assert_eq!(a20_ssi_state_finish(20, 21, e), 0);
        assert_eq!(a20_ssi_state_begin(20, 22), 0);
        a20_ssi_state_forget(21);
        let e = a20_ssi_state_begin(20, 22);
        assert_ne!(e, 0);
        assert_eq!(a20_ssi_state_finish(20, 22, e), 0);
        assert_eq!(core::mem::size_of::<Pages>(), 1056);
        let mut p = core::mem::MaybeUninit::<Pages>::uninit();
        unsafe {
            assert_eq!(a20_ssi_pages_init(p.as_mut_ptr(), 2, 8192), 0);
            let p = &mut p.assume_init();
            assert_eq!(a20_ssi_pages_grant(p, 4096, 5), 0);
            assert_eq!(a20_ssi_pages_grant(p, 8192, 3), 0);
            assert_eq!(a20_ssi_pages_grant(p, 12288, 3), -28);
            assert_eq!(a20_ssi_pages_return(p, 4096), -22);
            assert_eq!(a20_ssi_pages_complete(p), 0);
            assert_eq!(a20_ssi_pages_return(p, 8192), 1);
            assert_eq!(a20_ssi_pages_grant(p, 12288, 1), -22);
            assert_eq!(a20_ssi_pages_return(p, 8192), -22);
            assert_eq!(a20_ssi_pages_complete(p), 1);
            assert_eq!(a20_ssi_pages_charge(p, 8192), 0);
            assert_eq!(a20_ssi_pages_charge(p, u64::MAX), -28);
        }
        // Contending admissions must never grant two execution owners. State
        // observations are lock-free, and stale completion cannot release it.
        assert_eq!(a20_ssi_state_configure(100, 16, 10000), 0);
        let active = std::sync::Arc::new(std::sync::atomic::AtomicU32::new(0));
        std::thread::scope(|scope| {
            for pid in 101..117 {
                let active = active.clone();
                scope.spawn(move || {
                    for _ in 0..10000 {
                        let e = a20_ssi_state_begin(100, pid);
                        if e == 0 {
                            continue;
                        }
                        assert_eq!(active.fetch_add(1, Ordering::SeqCst), 0);
                        assert_eq!(a20_ssi_state_allowed(100, pid), 1);
                        assert_eq!(a20_ssi_state_allowed(100, 100), 0);
                        assert_eq!(a20_ssi_state_finish(100, pid, e.wrapping_sub(1)), -22);
                        assert_eq!(active.fetch_sub(1, Ordering::SeqCst), 1);
                        assert_eq!(a20_ssi_state_finish(100, pid, e), 0);
                    }
                });
            }
        });
    }
}
