//! Sparse canonical-page index for shared anonymous VMOs.
//!
//! The VMO lock protects lookup/reserve/publish. Its final reference uniquely
//! owns destruction after mapping TLBs have drained. Four radix levels cover
//! every u32 page index; absent branches consume no storage. Reservation may
//! retain empty nodes after ENOMEM, but never publishes a frame or charge.

use core::ffi::c_void;

const NONE: u32 = u32::MAX;
const ENOMEM: i32 = -12;
const EINVAL: i32 = -22;
const EEXIST: i32 = -17;

#[repr(C)]
struct Node {
    // Interior slots own child nodes; leaf slots own PFN + 1 (zero is absent).
    slots: [usize; 256],
}

unsafe extern "C" {
    fn a20_vmo_sparse_alloc_node(bytes: usize) -> *mut c_void;
    fn a20_vmo_sparse_free_node(node: *mut c_void);
    fn a20_vmo_destroy_release_page(vmo: *mut c_void, index: u32, pfn: u32);
}

unsafe fn leaf(mut node: *mut Node, index: u32) -> *mut Node {
    for shift in [24, 16, 8] {
        if node.is_null() { return node; }
        node = unsafe { (*node).slots[((index >> shift) & 255) as usize] as *mut Node };
    }
    node
}

#[no_mangle]
pub unsafe extern "C" fn a20_vmo_sparse_peek(root: *mut c_void, index: u32) -> u32 {
    let node = unsafe { leaf(root.cast(), index) };
    if node.is_null() { return NONE; }
    let encoded = unsafe { (*node).slots[(index & 255) as usize] };
    if encoded == 0 { NONE } else { (encoded - 1) as u32 }
}

/// Caller owns `*root` and holds its VMO lock. Each zeroed allocation is
/// immediately transferred to exactly one owning slot, including on failure.
#[no_mangle]
pub unsafe extern "C" fn a20_vmo_sparse_reserve(root: *mut *mut c_void, index: u32) -> i32 {
    if root.is_null() { return EINVAL; }
    let mut slot = root.cast::<usize>();
    for shift in [24, 16, 8, 0] {
        let mut node = unsafe { *slot as *mut Node };
        if node.is_null() {
            node = unsafe { a20_vmo_sparse_alloc_node(core::mem::size_of::<Node>()).cast() };
            if node.is_null() { return ENOMEM; }
            unsafe { *slot = node as usize };
        }
        if shift != 0 {
            slot = unsafe { &raw mut (*node).slots[((index >> shift) & 255) as usize] };
        }
    }
    0
}

/// No allocation after reserve: a frame transfers once, or stays with the
/// caller on invalid input / an already populated canonical slot.
#[no_mangle]
pub unsafe extern "C" fn a20_vmo_sparse_publish(root: *mut c_void, index: u32, pfn: u32) -> i32 {
    let node = unsafe { leaf(root.cast(), index) };
    if node.is_null() || pfn == NONE { return EINVAL; }
    let slot = unsafe { &mut (*node).slots[(index & 255) as usize] };
    if *slot != 0 { return EEXIST; }
    *slot = pfn as usize + 1;
    0
}

struct TreeOwner {
    node: *mut Node,
    shift: u32,
    prefix: u32,
    vmo: *mut c_void,
}

impl Drop for TreeOwner {
    fn drop(&mut self) {
        if self.node.is_null() { return; }
        for i in 0..256u32 {
            let value = unsafe { (*self.node).slots[i as usize] };
            if value == 0 { continue; }
            let index = self.prefix | (i << self.shift);
            if self.shift != 0 {
                drop(TreeOwner { node: value as *mut Node, shift: self.shift - 8,
                                 prefix: index, vmo: self.vmo });
            } else {
                unsafe { a20_vmo_destroy_release_page(self.vmo, index, (value - 1) as u32) };
            }
        }
        unsafe { a20_vmo_sparse_free_node(self.node.cast()) };
    }
}

/// The caller consumes its root pointer exactly once after the final VMO
/// reference and the TLB barrier; no mapper or fault may still borrow it.
#[no_mangle]
pub unsafe extern "C" fn a20_vmo_sparse_destroy(root: *mut c_void, vmo: *mut c_void) {
    drop(TreeOwner { node: root.cast(), shift: 24, prefix: 0, vmo });
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use self::std::{alloc::{alloc_zeroed, dealloc, Layout}, collections::BTreeMap, sync::Mutex};
    static STATE: Mutex<(isize, usize, BTreeMap<u32, u32>)> = Mutex::new((-1, 0, BTreeMap::new()));
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_sparse_alloc_node(bytes: usize) -> *mut c_void {
        assert_eq!(bytes, core::mem::size_of::<Node>());
        let mut s = STATE.lock().unwrap();
        if s.0 == 0 { return core::ptr::null_mut(); }
        if s.0 > 0 { s.0 -= 1; }
        let p = unsafe { alloc_zeroed(Layout::new::<Node>()) };
        assert!(!p.is_null()); s.1 += 1; p.cast()
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_sparse_free_node(node: *mut c_void) {
        let mut s = STATE.lock().unwrap(); assert!(s.1 > 0); s.1 -= 1;
        unsafe { dealloc(node.cast(), Layout::new::<Node>()) };
    }
    #[no_mangle]
    unsafe extern "C" fn a20_vmo_destroy_release_page(_: *mut c_void, index: u32, pfn: u32) {
        let mut s = STATE.lock().unwrap(); assert_eq!(s.2.remove(&index), Some(pfn));
    }
    #[test]
    fn sparse_ownership_and_every_allocation_failure() {
        let indices = [0, 1, 255, 256, 65535, 65536, 0x01000000, 0xfffffffe, u32::MAX];
        for fail in 0..=4 {
            let mut root = core::ptr::null_mut();
            *STATE.lock().unwrap() = (fail, 0, BTreeMap::new());
            unsafe {
                let rc = a20_vmo_sparse_reserve(&mut root, u32::MAX);
                assert_eq!(rc, if fail < 4 { ENOMEM } else { 0 });
                assert_eq!(a20_vmo_sparse_peek(root, u32::MAX), NONE);
                STATE.lock().unwrap().0 = -1;
                for (i, index) in indices.iter().copied().enumerate() {
                    assert_eq!(a20_vmo_sparse_reserve(&mut root, index), 0);
                    assert_eq!(a20_vmo_sparse_publish(root, index, NONE), EINVAL);
                    assert_eq!(a20_vmo_sparse_publish(root, index, i as u32), 0);
                    STATE.lock().unwrap().2.insert(index, i as u32);
                    assert_eq!(a20_vmo_sparse_peek(root, index), i as u32);
                    assert_eq!(a20_vmo_sparse_publish(root, index, 999), EEXIST);
                }
                assert_eq!(a20_vmo_sparse_peek(root, 123456), NONE);
                assert!(STATE.lock().unwrap().1 < 32);
                a20_vmo_sparse_destroy(root, core::ptr::null_mut());
                assert_eq!(STATE.lock().unwrap().1, 0);
                assert!(STATE.lock().unwrap().2.is_empty());
            }
        }
        // An abandoned reservation, including a partially allocated path,
        // has no frame to free and releases every metadata allocation.
        for fail in 0..=4 {
            let mut root = core::ptr::null_mut();
            *STATE.lock().unwrap() = (fail, 0, BTreeMap::new());
            unsafe {
                a20_vmo_sparse_reserve(&mut root, 0x12345678);
                a20_vmo_sparse_destroy(root, core::ptr::null_mut());
            }
            assert_eq!(STATE.lock().unwrap().1, 0);
        }
    }
}
