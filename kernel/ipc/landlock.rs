#![no_std]
use core::ffi::c_void;
use core::sync::atomic::{AtomicI32, Ordering};
const RULES: usize = 64;
const PREFIX: usize = 256;
const EINVAL: i32 = 22;
const E2BIG: i32 = 7;
const EACCES: i32 = 13;
const ENOMEM: i32 = 12;
const ACCESS_ALL: u64 = (1 << 16) - 1;
#[repr(C)]
struct Ruleset { refs: i32, next: *mut c_void, fd: i32, used: i32, restricted: i32, handled: u64, paths: [[u8; PREFIX]; RULES], access: [u64; RULES], nr_rules: i32 }
unsafe extern "C" { fn a20_landlock_alloc(size: usize) -> *mut c_void; fn a20_landlock_free(p: *mut c_void); }
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_create(handled: u64, out: *mut *mut c_void) -> i32 {
    if out.is_null() || handled & !ACCESS_ALL != 0 { return -EINVAL; }
    let p = a20_landlock_alloc(core::mem::size_of::<Ruleset>()) as *mut Ruleset;
    if p.is_null() { return -ENOMEM; }
    core::ptr::write_bytes(p, 0, 1); (*p).refs = 1; (*p).used = 1; (*p).handled = handled; *out = p as *mut c_void; 0
}
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_get(p: *mut c_void) -> bool {
    if p.is_null() { return false; }
    let rs = p as *mut Ruleset;
    AtomicI32::from_ptr(&mut (*rs).refs).fetch_update(Ordering::Acquire, Ordering::Relaxed, |v| if v > 0 { Some(v + 1) } else { None }).is_ok()
}
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_put(p: *mut c_void) {
    if p.is_null() { return; }
    let rs = p as *mut Ruleset;
    if AtomicI32::from_ptr(&mut (*rs).refs).fetch_sub(1, Ordering::AcqRel) == 1 { a20_landlock_free(p); }
}
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_add(p: *mut c_void, path: *const u8, access: u64) -> i32 {
    if p.is_null() || path.is_null() || access & !ACCESS_ALL != 0 { return -EINVAL; }
    let rs = &mut *(p as *mut Ruleset);
    if rs.restricted != 0 || access & !rs.handled != 0 { return -EINVAL; }
    if rs.nr_rules as usize >= RULES { return -E2BIG; }
    let slot = rs.nr_rules as usize; let dst = &mut rs.paths[slot];
    let mut i = 0; while i + 1 < PREFIX { let b = *path.add(i); dst[i] = b; if b == 0 { break; } i += 1; }
    dst[PREFIX - 1] = 0; rs.access[slot] = access; rs.nr_rules += 1; 0
}
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_restrict(p: *mut c_void) -> i32 {
    if p.is_null() { return -EINVAL; }
    let rs = &mut *(p as *mut Ruleset); if rs.restricted != 0 { return -EINVAL; }
    rs.restricted = 1; 0
}
#[no_mangle]
pub unsafe extern "C" fn a20_landlock_match(p: *mut c_void, path: *const u8, needed: u64) -> i32 {
    if p.is_null() || path.is_null() { return 0; }
    let rs = &*(p as *mut Ruleset); let needed = needed & rs.handled; if needed == 0 { return 0; } let mut n = 0;
    while n < rs.nr_rules as usize {
        let rule = &rs.paths[n]; let mut i = 0;
        while i < PREFIX && rule[i] != 0 && *path.add(i) != 0 && rule[i] == *path.add(i) { i += 1; }
        let rule_end = i < PREFIX && rule[i] == 0;
        let path_end = *path.add(i) == 0;
        if rule_end && (i == 1 || path_end || *path.add(i) == b'/') {
            return if needed & !rs.access[n] != 0 { -EACCES } else { 0 };
        }
        n += 1;
    }
    -EACCES
}
#[cfg(test)]
mod tests {
    use super::*;
    #[test] fn rules_match_component_boundary_and_access() {
        let mut rs: Ruleset = unsafe { core::mem::zeroed() }; rs.nr_rules = 1; rs.handled = 3;
        rs.paths[0][..10].copy_from_slice(b"/srv/data\0"); rs.access[0] = 1;
        let exact=b"/srv/data\0"; let child=b"/srv/data/file\0"; let sibling=b"/srv/database/file\0";
        unsafe { assert_eq!(a20_landlock_match(&mut rs as *mut _ as *mut c_void, exact.as_ptr(), 1), 0); assert_eq!(a20_landlock_match(&mut rs as *mut _ as *mut c_void, child.as_ptr(), 1), 0); assert_eq!(a20_landlock_match(&mut rs as *mut _ as *mut c_void, sibling.as_ptr(), 1), -EACCES); assert_eq!(a20_landlock_match(&mut rs as *mut _ as *mut c_void, child.as_ptr(), 2), -EACCES); assert_eq!(a20_landlock_add(&mut rs as *mut _ as *mut c_void, exact.as_ptr(), 1), 0); assert_eq!(a20_landlock_restrict(&mut rs as *mut _ as *mut c_void), 0); assert_eq!(a20_landlock_add(&mut rs as *mut _ as *mut c_void, child.as_ptr(), 1), -EINVAL); assert_eq!(a20_landlock_match(&mut rs as *mut _ as *mut c_void, sibling.as_ptr(), 4), 0); }
    }
}
