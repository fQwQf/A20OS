#!/usr/bin/env python3
"""Execute the production VMO page transaction with mocked narrow C primitives."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/mm/vmo_page.rs"

HARNESS = r'''
use std::sync::Mutex;

#[derive(Default)]
struct State {
    slot: u32,
    paged: bool,
    charge_fail: bool,
    alloc_fail: bool,
    pager_rc: i32,
    pager_supplies: bool,
    locked: bool,
    charge: usize,
    uncharge: usize,
    alloc: usize,
    free: usize,
    zero: usize,
    publish: usize,
    resize_count: u32,
    resize_size: u64,
    resize_phys_size: u64,
    resize_charged: u64,
    resize_uncharge: usize,
    resize_slots: [u32; 8],
    resize_table_ptr: usize,
    resize_table_fail: bool,
    resize_flush: usize,
    resize_released: usize,
    resize_table_frees: usize,
    destroy_notify: usize,
    destroy_detach: usize,
    destroy_stats: usize,
    destroy_flush: usize,
    destroy_release: usize,
    destroy_invalid_pfn: usize,
    destroy_uncharge: usize,
    destroy_free_pages: usize,
    destroy_free_object: usize,
    destroy_phys_pages: u64,
}
static STATE: Mutex<State> = Mutex::new(State {
    slot:u32::MAX, paged:false, charge_fail:false, alloc_fail:false,
    pager_rc:0, pager_supplies:false, locked:false, charge:0, uncharge:0,
    alloc:0, free:0, zero:0, publish:0,
    resize_count:0, resize_size:0, resize_phys_size:0, resize_charged:0, resize_uncharge:0,
    resize_slots:[u32::MAX;8],
    resize_table_ptr:0, resize_table_fail:false, resize_flush:0,
    resize_released:0, resize_table_frees:0,
    destroy_notify:0, destroy_detach:0, destroy_stats:0, destroy_flush:0,
    destroy_release:0, destroy_invalid_pfn:0, destroy_uncharge:0, destroy_free_pages:0,
    destroy_free_object:0, destroy_phys_pages:0,
});
static CG: u8 = 0;

#[no_mangle]
unsafe extern "C" fn a20_vmo_page_lock(_: *mut core::ffi::c_void, i: u32, f: *mut u64) -> i32 {
    let mut s = STATE.lock().unwrap();
    if i != 0 { return -22; }
    assert!(!s.locked);
    s.locked = true;
    unsafe { f.write(0); }
    0
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_unlock(_: *mut core::ffi::c_void, _: u64) {
    let mut s = STATE.lock().unwrap(); assert!(s.locked); s.locked = false;
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_slot_locked(_: *mut core::ffi::c_void, _: u32) -> u32 { STATE.lock().unwrap().slot }
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_is_paged_locked(_: *mut core::ffi::c_void) -> i32 { STATE.lock().unwrap().paged as i32 }
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_paged_fault(_: *mut core::ffi::c_void, _: u32, _: i32) -> i32 {
    let mut s=STATE.lock().unwrap(); assert!(!s.locked);
    if s.pager_supplies { s.slot=77; }
    s.pager_rc
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_charge(_: *mut core::ffi::c_void) -> i32 {
    let mut s=STATE.lock().unwrap(); assert!(s.locked); s.charge+=1; if s.charge_fail {-12} else {0}
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_uncharge(_: *mut core::ffi::c_void) {
    let mut s=STATE.lock().unwrap(); assert!(s.locked); s.uncharge+=1;
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_alloc() -> u32 {
    let mut s=STATE.lock().unwrap(); assert!(s.locked); s.alloc+=1; if s.alloc_fail {u32::MAX} else {41}
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_free(_: u32) { let mut s=STATE.lock().unwrap(); assert!(s.locked); s.free+=1; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_zero(_: u32) { let mut s=STATE.lock().unwrap(); assert!(s.locked); s.zero+=1; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_page_publish_locked(_: *mut core::ffi::c_void, _: u32, p: u32, _: *mut core::ffi::c_void) {
    let mut s=STATE.lock().unwrap(); assert!(s.locked); assert_eq!(s.slot,u32::MAX); s.slot=p; s.publish+=1;
}

#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_lock(_: *mut core::ffi::c_void, f: *mut u64) -> i32 {
    let mut s=STATE.lock().unwrap(); assert!(!s.locked); s.locked=true; unsafe { f.write(0); } 0
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_unlock(_: *mut core::ffi::c_void, _: u64) {
    let mut s=STATE.lock().unwrap(); assert!(s.locked); s.locked=false;
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_page_count(_: *mut core::ffi::c_void) -> u32 { STATE.lock().unwrap().resize_count }
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_slot(_: *mut core::ffi::c_void, i: u32) -> u32 { STATE.lock().unwrap().resize_slots[i as usize] }
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_alloc_table(_: u32) -> *mut core::ffi::c_void {
    if STATE.lock().unwrap().resize_table_fail { return core::ptr::null_mut(); }
    let table=vec![u32::MAX;8].into_boxed_slice(); Box::into_raw(table) as *mut core::ffi::c_void
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_free_table(t: *mut core::ffi::c_void) {
    if t.is_null() { return; }
    let mut s=STATE.lock().unwrap(); s.resize_table_frees+=1;
    let slice=core::ptr::slice_from_raw_parts_mut(t as *mut u32,8);
    unsafe { drop(Box::from_raw(slice)); }
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_table_set(t: *mut core::ffi::c_void, i:u32, p:u32) {
    unsafe { (t as *mut u32).add(i as usize).write(p); }
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_publish(_: *mut core::ffi::c_void, t:*mut core::ffi::c_void, n:u32, size:u64) -> *mut core::ffi::c_void {
    let mut s=STATE.lock().unwrap(); assert!(s.locked);
    let old=s.resize_table_ptr as *mut core::ffi::c_void;
    if t.is_null() {
        s.resize_count=n; s.resize_size=size;
    } else {
        for i in 0..n as usize { s.resize_slots[i]=unsafe { (t as *const u32).add(i).read() }; }
        for i in n as usize..8 { s.resize_slots[i]=u32::MAX; }
        s.resize_table_ptr=t as usize; s.resize_count=n; s.resize_size=size;
    }
    old
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_flush_tlb() { STATE.lock().unwrap().resize_flush+=1; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_release_page(_: *mut core::ffi::c_void, i:u32) {
    let mut s=STATE.lock().unwrap(); assert!(s.locked); assert!(s.resize_flush>0);
    assert_ne!(s.resize_slots[i as usize],u32::MAX); s.resize_slots[i as usize]=u32::MAX; s.resize_released+=1;
    s.resize_phys_size-=4096;
    if s.resize_charged>0 { s.resize_charged-=1; s.resize_uncharge+=1; }
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_resize_finish_shrink(_: *mut core::ffi::c_void) { assert!(STATE.lock().unwrap().locked); }

#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_notify(_: *mut core::ffi::c_void) { STATE.lock().unwrap().destroy_notify+=1; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_detach_pager(_: *mut core::ffi::c_void) { STATE.lock().unwrap().destroy_detach+=1; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_page_count(_: *mut core::ffi::c_void) -> u32 { STATE.lock().unwrap().resize_count }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_page(_: *mut core::ffi::c_void, i:u32) -> u32 { STATE.lock().unwrap().resize_slots[i as usize] }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_phys_size(_: *mut core::ffi::c_void) -> u64 { STATE.lock().unwrap().resize_phys_size }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_charged_pages(_: *mut core::ffi::c_void) -> u64 { STATE.lock().unwrap().resize_charged }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_stats(p:u64) { let mut s=STATE.lock().unwrap();s.destroy_stats+=1;s.destroy_phys_pages=p/4096; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_flush_tlb() { STATE.lock().unwrap().destroy_flush+=1; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_release_page(_: *mut core::ffi::c_void, i:u32, p:u32) {
    let mut s=STATE.lock().unwrap();assert!(s.destroy_flush>0);assert_eq!(p,s.resize_slots[i as usize]);if p>=1024{s.destroy_invalid_pfn+=1;}s.destroy_release+=1;
}
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_uncharge(_: *mut core::ffi::c_void, n:u64) { let mut s=STATE.lock().unwrap();assert_eq!(n,s.resize_charged);assert_eq!(s.destroy_free_pages,1);s.destroy_uncharge+=1; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_free_pages(_: *mut core::ffi::c_void) { let mut s=STATE.lock().unwrap();assert_eq!(s.destroy_release,s.resize_slots.iter().filter(|p|**p!=u32::MAX).count());s.destroy_free_pages+=1; }
#[no_mangle]
unsafe extern "C" fn a20_vmo_destroy_free_object(_: *mut core::ffi::c_void) { let mut s=STATE.lock().unwrap();assert_eq!(s.destroy_free_pages,1);s.destroy_free_object+=1; }

fn reset() {
    let old_table=STATE.lock().unwrap().resize_table_ptr;
    if old_table != 0 { unsafe { a20_vmo_resize_free_table(old_table as *mut _) }; }
    *STATE.lock().unwrap() = State {
    slot:u32::MAX, paged:false, charge_fail:false, alloc_fail:false,
    pager_rc:0, pager_supplies:false, locked:false, charge:0, uncharge:0,
    alloc:0, free:0, zero:0, publish:0,
    resize_count:0, resize_size:0, resize_phys_size:0, resize_charged:0, resize_uncharge:0,
    resize_slots:[u32::MAX;8],
    resize_table_ptr:0, resize_table_fail:false, resize_flush:0,
    resize_released:0, resize_table_frees:0,
    destroy_notify:0, destroy_detach:0, destroy_stats:0, destroy_flush:0,
    destroy_release:0, destroy_invalid_pfn:0, destroy_uncharge:0, destroy_free_pages:0,
    destroy_free_object:0, destroy_phys_pages:0,
}; }
fn obj() -> *mut core::ffi::c_void { 1usize as *mut _ }

#[test]
fn canonical_page_bypasses_allocation() {
    reset(); STATE.lock().unwrap().slot=19;
    assert_eq!(unsafe { production::test_materialize(obj(),0,core::ptr::null_mut(),false,false) },Ok(19));
    let s=STATE.lock().unwrap(); assert_eq!(s.alloc,0); assert_eq!(s.publish,0); assert!(!s.locked);
}
#[test]
fn charge_failure_does_not_allocate_or_publish() {
    reset(); STATE.lock().unwrap().charge_fail=true;
    assert_eq!(unsafe { production::test_materialize(obj(),0,&CG as *const _ as *mut _,true,true) },Err(-12));
    let s=STATE.lock().unwrap(); assert_eq!(s.charge,1); assert_eq!(s.uncharge,0); assert_eq!(s.alloc,0); assert_eq!(s.publish,0); assert!(!s.locked);
}
#[test]
fn frame_failure_rolls_back_charge_before_unlock() {
    reset(); { let mut s=STATE.lock().unwrap(); s.alloc_fail=true; }
    assert_eq!(unsafe { production::test_materialize(obj(),0,&CG as *const _ as *mut _,true,true) },Err(-12));
    let s=STATE.lock().unwrap(); assert_eq!(s.charge,1); assert_eq!(s.uncharge,1); assert_eq!(s.free,0); assert_eq!(s.publish,0); assert!(!s.locked);
}
#[test]
fn successful_publish_commits_frame_and_charge_once() {
    reset();
    assert_eq!(unsafe { production::test_materialize(obj(),0,&CG as *const _ as *mut _,true,true) },Ok(41));
    let s=STATE.lock().unwrap(); assert_eq!(s.slot,41); assert_eq!(s.charge,1); assert_eq!(s.zero,1); assert_eq!(s.publish,1); assert_eq!(s.free,0); assert_eq!(s.uncharge,0); assert!(!s.locked);
}
#[test]
fn pager_is_called_unlocked_and_existing_page_is_reused() {
    reset(); { let mut s=STATE.lock().unwrap(); s.paged=true; s.pager_supplies=true; s.pager_rc=0; }
    assert_eq!(unsafe { production::test_materialize(obj(),0,core::ptr::null_mut(),false,false) },Ok(77));
    let s=STATE.lock().unwrap(); assert_eq!(s.alloc,0); assert_eq!(s.publish,0); assert!(!s.locked);
}
#[test]
fn pager_eagain_falls_back_only_for_charged_path() {
    reset(); { let mut s=STATE.lock().unwrap(); s.paged=true; s.pager_rc=-11; }
    assert_eq!(unsafe { production::test_materialize(obj(),0,core::ptr::null_mut(),false,false) },Err(-5));
    assert_eq!(STATE.lock().unwrap().alloc,0);
    reset(); { let mut s=STATE.lock().unwrap(); s.paged=true; s.pager_rc=-11; }
    assert_eq!(unsafe { production::test_materialize(obj(),0,core::ptr::null_mut(),true,true) },Ok(41));
    let s=STATE.lock().unwrap(); assert_eq!(s.alloc,1); assert_eq!(s.publish,1); assert!(!s.locked);
}
#[test]
fn resize_grow_reserves_then_commits_table_and_preserves_slots() {
    reset(); { let mut s=STATE.lock().unwrap(); s.resize_count=2; s.resize_size=8192; s.resize_slots[0]=12; }
    assert_eq!(unsafe { production::test_resize(obj(),16384) },0);
    let s=STATE.lock().unwrap(); assert_eq!(s.resize_count,4); assert_eq!(s.resize_size,16384);
    assert_eq!(&s.resize_slots[..4],&[12,u32::MAX,u32::MAX,u32::MAX]); assert!(!s.locked);
}
#[test]
fn resize_grow_allocation_failure_is_atomic() {
    reset(); { let mut s=STATE.lock().unwrap(); s.resize_count=2; s.resize_size=8192; s.resize_slots[0]=12; s.resize_table_fail=true; }
    assert_eq!(unsafe { production::test_resize(obj(),16384) },-1);
    let s=STATE.lock().unwrap(); assert_eq!(s.resize_count,2); assert_eq!(s.resize_size,8192); assert_eq!(s.resize_slots[0],12); assert!(!s.locked);
}
#[test]
fn resize_shrink_flushes_before_releasing_materialized_pages() {
    reset(); { let mut s=STATE.lock().unwrap(); s.resize_count=4; s.resize_size=16384; s.resize_phys_size=8192; s.resize_charged=2; s.resize_slots[2]=22; s.resize_slots[3]=23; }
    assert_eq!(unsafe { production::test_resize(obj(),8192) },0);
    let s=STATE.lock().unwrap(); assert_eq!(s.resize_flush,1); assert_eq!(s.resize_released,2); assert_eq!(s.resize_count,2); assert_eq!(s.resize_size,8192); assert_eq!(s.resize_phys_size,0); assert_eq!(s.resize_charged,0); assert_eq!(s.resize_uncharge,2); assert!(!s.locked);
}
#[test]
fn resize_rejects_rounding_overflow_without_locking() {
    reset(); assert_eq!(unsafe { production::test_resize(obj(),u64::MAX) },-1);
    let s=STATE.lock().unwrap(); assert_eq!(s.resize_count,0); assert!(!s.locked);
}
#[test]
fn destroy_empty_vmo_skips_tlb_and_uncharge() {
    reset();
    unsafe { production::test_destroy(obj()); }
    let s=STATE.lock().unwrap();assert_eq!(s.destroy_notify,1);assert_eq!(s.destroy_detach,1);assert_eq!(s.destroy_stats,1);assert_eq!(s.destroy_phys_pages,0);assert_eq!(s.destroy_flush,0);assert_eq!(s.destroy_release,0);assert_eq!(s.destroy_uncharge,0);assert_eq!(s.destroy_free_pages,1);assert_eq!(s.destroy_free_object,1);
}
#[test]
fn destroy_flushes_before_pages_then_uncharges_before_free_object() {
    reset(); { let mut s=STATE.lock().unwrap();s.resize_count=3;s.resize_slots[0]=11;s.resize_slots[2]=13;s.resize_phys_size=8192;s.resize_charged=2; }
    unsafe { production::test_destroy(obj()); }
    let s=STATE.lock().unwrap();assert_eq!(s.destroy_notify,1);assert_eq!(s.destroy_detach,1);assert_eq!(s.destroy_phys_pages,2);assert_eq!(s.destroy_flush,1);assert_eq!(s.destroy_release,2);assert_eq!(s.destroy_uncharge,1);assert_eq!(s.destroy_free_pages,1);assert_eq!(s.destroy_free_object,1);
}
#[test]
fn destroy_routes_invalid_pfn_to_c_fail_stop_primitive() {
    reset(); { let mut s=STATE.lock().unwrap();s.resize_count=1;s.resize_slots[0]=u32::MAX-1;s.resize_phys_size=4096; }
    unsafe { production::test_destroy(obj()); }
    let s=STATE.lock().unwrap(); assert_eq!(s.destroy_invalid_pfn,1); assert_eq!(s.destroy_free_object,1);
}
'''


def main() -> None:
    rust = SOURCE.read_text().replace("#![no_std]\n", "", 1)
    with tempfile.TemporaryDirectory(prefix="vmo-page-host-") as tmp:
        generated = Path(tmp) / "vmo_page_host.rs"
        binary = Path(tmp) / "vmo_page_host"
        generated.write_text("mod production {\n" + rust + "\npub(super) unsafe fn test_materialize(v:*mut c_void,i:u32,c:*mut c_void,ch:bool,fb:bool)->Result<u32,i32>{ materialize(v,i,c,ch,fb) }\npub(super) unsafe fn test_resize(v:*mut c_void,n:u64)->i64{ resize(v,n) }\npub(super) unsafe fn test_destroy(v:*mut c_void){ destroy(v) }\n}\n" + HARNESS)
        subprocess.run(["rustc", "--edition=2021", "--test", str(generated), "-o", str(binary)], check=True)
        subprocess.run([str(binary), "--test-threads=1"], check=True)


if __name__ == "__main__":
    main()
