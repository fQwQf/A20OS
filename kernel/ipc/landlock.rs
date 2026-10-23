#![no_std]

//! Bounded path policy. C serializes policy access under its leaf spinlock;
//! references are independent atomics so fd/task pins can change concurrently.
use core::cell::UnsafeCell;
use core::ffi::c_void;
use core::sync::atomic::{AtomicI32, Ordering};

const RULES: usize = 64;
const PREFIX: usize = 256;
const EINVAL: i32 = 22;
const E2BIG: i32 = 7;
const EACCES: i32 = 13;
const ENOMEM: i32 = 12;
const ENAMETOOLONG: i32 = 36;
const ACCESS_ALL: u64 = (1 << 16) - 1;

struct Policy {
    handled: u64,
    sealed: bool,
    count: usize,
    paths: [[u8; PREFIX]; RULES],
    access: [u64; RULES],
}
struct Ruleset {
    refs: AtomicI32,
    policy: UnsafeCell<Policy>,
}
unsafe extern "C" {
    fn a20_landlock_alloc(size: usize) -> *mut c_void;
    fn a20_landlock_free(p: *mut c_void);
}

#[no_mangle]
pub unsafe extern "C" fn a20_landlock_create(handled: u64, out: *mut *mut c_void) -> i32 {
    if handled == 0 || handled & !ACCESS_ALL != 0 { return -EINVAL; }
    let rs = a20_landlock_alloc(core::mem::size_of::<Ruleset>()) as *mut Ruleset;
    if rs.is_null() { return -ENOMEM; }
    // Initialize in place: the bounded rule array must not consume a kernel
    // stack. All-zero Policy is valid, and no observer can see this allocation.
    core::ptr::write_bytes(rs, 0, 1);
    core::ptr::addr_of_mut!((*rs).refs).write(AtomicI32::new(1));
    (*(*rs).policy.get()).handled = handled;
    *out = rs.cast();
    0
}

// Callers already hold a vfile/task reference while obtaining another pin.
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_get(p: *mut c_void) -> bool {
    if p.is_null() { return false; }
    (*(p as *mut Ruleset)).refs.fetch_add(1, Ordering::Relaxed);
    true
}
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_put(p: *mut c_void) {
    if !p.is_null() && (*(p as *mut Ruleset)).refs.fetch_sub(1, Ordering::AcqRel) == 1 {
        a20_landlock_free(p);
    }
}

// The following three entry points require g_landlock_policy_lock in C.
// They allocate nothing, sleep nowhere and call no filesystem/task operations.
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_add(p: *mut c_void, path: *const u8, access: u64) -> i32 {
    let policy = &mut *(*(p as *mut Ruleset)).policy.get();
    if policy.sealed || access == 0 || access & !policy.handled != 0 || *path != b'/' {
        return -EINVAL;
    }
    let mut len = 0;
    while len < PREFIX && *path.add(len) != 0 { len += 1; }
    if len == PREFIX { return -ENAMETOOLONG; }
    if policy.count == RULES { return -E2BIG; }
    let slot = policy.count;
    core::ptr::copy_nonoverlapping(path, policy.paths[slot].as_mut_ptr(), len + 1);
    policy.access[slot] = access;
    policy.count += 1;
    0
}
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_restrict(p: *mut c_void) -> i32 {
    // Freezing is idempotent: independently confined tasks may share one fd.
    (*(*(p as *mut Ruleset)).policy.get()).sealed = true;
    0
}
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_match(p: *mut c_void, path: *const u8, needed: u64) -> i32 {
    if p.is_null() { return 0; }
    let policy = &*(*(p as *mut Ruleset)).policy.get();
    let needed = needed & policy.handled;
    if needed == 0 { return 0; }
    let mut allowed = 0;
    for n in 0..policy.count {
        let rule = &policy.paths[n];
        let mut i = 0;
        while rule[i] != 0 && rule[i] == *path.add(i) { i += 1; }
        if rule[i] == 0 && (i == 1 || *path.add(i) == 0 || *path.add(i) == b'/') {
            allowed |= policy.access[n];
        }
    }
    if needed & !allowed == 0 { 0 } else { -EACCES }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::alloc::{alloc_zeroed, dealloc, Layout};
    #[no_mangle]
    unsafe extern "C" fn a20_landlock_alloc(size: usize) -> *mut c_void {
        alloc_zeroed(Layout::from_size_align(size, core::mem::align_of::<Ruleset>()).unwrap()).cast()
    }
    #[no_mangle]
    unsafe extern "C" fn a20_landlock_free(p: *mut c_void) {
        dealloc(p.cast(), Layout::new::<Ruleset>());
    }
    #[test]
    fn union_boundary_seal_and_reference_lifetime() {
        unsafe {
            let mut rs = core::ptr::null_mut();
            assert_eq!(a20_landlock_create(6, &mut rs), 0);
            assert_eq!(a20_landlock_add(rs, b"/srv\0".as_ptr(), 4), 0);
            assert_eq!(a20_landlock_add(rs, b"/srv/data\0".as_ptr(), 2), 0);
            assert_eq!(a20_landlock_match(rs, b"/srv/data/file\0".as_ptr(), 6), 0);
            assert_eq!(a20_landlock_match(rs, b"/srv/database\0".as_ptr(), 6), -EACCES);
            assert_eq!(a20_landlock_match(rs, b"/srv2\0".as_ptr(), 4), -EACCES);
            assert_eq!(a20_landlock_restrict(rs), 0);
            assert!(a20_landlock_get(rs));
            assert_eq!(a20_landlock_restrict(rs), 0);
            assert_eq!(a20_landlock_add(rs, b"/\0".as_ptr(), 6), -EINVAL);
            a20_landlock_put(rs);
            assert_eq!(a20_landlock_match(rs, b"/srv/data\0".as_ptr(), 6), 0);
            a20_landlock_put(rs);
        }
    }
    #[test]
    fn long_prefix_cannot_silently_broaden_authority() {
        unsafe {
            let mut rs = core::ptr::null_mut();
            assert_eq!(a20_landlock_create(4, &mut rs), 0);
            let mut path = [b'a'; PREFIX + 1];
            path[0] = b'/'; path[PREFIX] = 0;
            assert_eq!(a20_landlock_add(rs, path.as_ptr(), 4), -ENAMETOOLONG);
            assert_eq!(a20_landlock_match(rs, b"/aaaa\0".as_ptr(), 4), -EACCES);
            a20_landlock_put(rs);
        }
    }
}
