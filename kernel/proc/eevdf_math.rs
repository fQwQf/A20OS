#![cfg_attr(not(test), no_std)]

//! Scalar EEVDF calculations shared with the C scheduler adapter.
//!
//! This module owns scalar arithmetic plus the intrusive EEVDF treap's
//! structural transitions. C owns task lifetime, runqueues, locks, time
//! sampling, and PI-boost state; it passes stable node pointers while holding
//! the owning runqueue lock.

use core::ffi::c_void;

#[repr(C)]
pub struct EevdfNode {
    left: *mut EevdfNode,
    right: *mut EevdfNode,
    parent: *mut EevdfNode,
    min_vruntime: u64,
    heap_prio: u32,
    owner: *mut c_void,
    deadline_key: u64,
    vruntime_key: u64,
}

// Keep this repr(C) layout synchronized with the assertions adjacent to
// eevdf_node_t in proc.h. These checks compile for each Rust target selected
// by the kernel build, including ILP32 and LP64.
#[cfg(target_pointer_width = "64")]
const _: () = {
    assert!(core::mem::offset_of!(EevdfNode, min_vruntime) == 24);
    assert!(core::mem::offset_of!(EevdfNode, heap_prio) == 32);
    assert!(core::mem::offset_of!(EevdfNode, owner) == 40);
    assert!(core::mem::offset_of!(EevdfNode, deadline_key) == 48);
    assert!(core::mem::offset_of!(EevdfNode, vruntime_key) == 56);
    assert!(core::mem::size_of::<EevdfNode>() == 64);
    assert!(core::mem::align_of::<EevdfNode>() == 8);
};

#[cfg(target_pointer_width = "32")]
const _: () = {
    assert!(core::mem::offset_of!(EevdfNode, min_vruntime) == 16);
    assert!(core::mem::offset_of!(EevdfNode, heap_prio) == 24);
    assert!(core::mem::offset_of!(EevdfNode, owner) == 28);
    assert!(core::mem::offset_of!(EevdfNode, deadline_key) == 32);
    assert!(core::mem::offset_of!(EevdfNode, vruntime_key) == 40);
    assert!(core::mem::size_of::<EevdfNode>() == 48);
    assert!(core::mem::align_of::<EevdfNode>() == 8);
};

#[inline]
fn key_less(a: *const EevdfNode, b: *const EevdfNode) -> bool {
    // SAFETY: callers only pass live nodes belonging to a runqueue protected
    // by its lock; queued key fields are immutable until removal.
    unsafe {
        let a = &*a;
        let b = &*b;
        a.deadline_key < b.deadline_key
            || (a.deadline_key == b.deadline_key
                && (a.vruntime_key < b.vruntime_key
                    || (a.vruntime_key == b.vruntime_key
                        && (a.owner as usize) < (b.owner as usize))))
    }
}

#[inline]
unsafe fn augment(node: *mut EevdfNode) {
    // SAFETY: node and any non-null children are live nodes in the locked tree.
    unsafe {
        let mut min = (*node).vruntime_key;
        if !(*node).left.is_null() && (*(*node).left).min_vruntime < min {
            min = (*(*node).left).min_vruntime;
        }
        if !(*node).right.is_null() && (*(*node).right).min_vruntime < min {
            min = (*(*node).right).min_vruntime;
        }
        (*node).min_vruntime = min;
    }
}

unsafe fn augment_up(mut node: *mut EevdfNode) {
    while !node.is_null() {
        // SAFETY: the caller holds the queue lock and node is in its tree.
        unsafe {
            augment(node);
            node = (*node).parent;
        }
    }
}

unsafe fn rotate_left(root: *mut *mut EevdfNode, x: *mut EevdfNode) {
    // SAFETY: caller guarantees x has a right child and root is a valid tree root.
    unsafe {
        let y = (*x).right;
        (*x).right = (*y).left;
        if !(*y).left.is_null() {
            (*(*y).left).parent = x;
        }
        (*y).parent = (*x).parent;
        if (*y).parent.is_null() {
            *root = y;
        } else if (*(*y).parent).left == x {
            (*(*y).parent).left = y;
        } else {
            (*(*y).parent).right = y;
        }
        (*y).left = x;
        (*x).parent = y;
        augment(x);
        augment(y);
    }
}

unsafe fn rotate_right(root: *mut *mut EevdfNode, x: *mut EevdfNode) {
    // SAFETY: caller guarantees x has a left child and root is a valid tree root.
    unsafe {
        let y = (*x).left;
        (*x).left = (*y).right;
        if !(*y).right.is_null() {
            (*(*y).right).parent = x;
        }
        (*y).parent = (*x).parent;
        if (*y).parent.is_null() {
            *root = y;
        } else if (*(*y).parent).left == x {
            (*(*y).parent).left = y;
        } else {
            (*(*y).parent).right = y;
        }
        (*y).right = x;
        (*x).parent = y;
        augment(x);
        augment(y);
    }
}

unsafe fn extreme(mut node: *mut EevdfNode, leftmost: bool) -> *mut c_void {
    if node.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: node is a live root in a locked tree.
    unsafe {
        loop {
            let next = if leftmost { (*node).left } else { (*node).right };
            if next.is_null() {
                return (*node).owner;
            }
            node = next;
        }
    }
}

/// Insert an unlinked node and restore BST, heap, and subtree-min invariants.
///
/// # Safety
/// `root` points to a valid root slot, `node` is a live writable node not in
/// any tree, and `owner` is a non-null stable identity. The caller holds the
/// queue lock and keeps owner/node alive until removal.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_eevdf_tree_insert(
    root: *mut *mut EevdfNode,
    node: *mut EevdfNode,
    owner: *mut c_void,
    deadline: u64,
    vruntime: u64,
    heap_prio: u32,
) -> u32 {
    if root.is_null() || node.is_null() || owner.is_null() {
        return 0;
    }
    // SAFETY: validity/lifetime and exclusive queue access are the caller's contract.
    unsafe {
        if !(*node).owner.is_null()
            || !(*node).left.is_null()
            || !(*node).right.is_null()
            || !(*node).parent.is_null()
        {
            return 0;
        }
        (*node).owner = owner;
        (*node).deadline_key = deadline;
        (*node).vruntime_key = vruntime;
        (*node).heap_prio = heap_prio;
        (*node).min_vruntime = vruntime;

        let mut parent = core::ptr::null_mut();
        let mut link = *root;
        while !link.is_null() {
            parent = link;
            link = if key_less(node, parent) {
                (*parent).left
            } else {
                (*parent).right
            };
        }
        (*node).parent = parent;
        if parent.is_null() {
            *root = node;
        } else if key_less(node, parent) {
            (*parent).left = node;
        } else {
            (*parent).right = node;
        }
        augment_up(node);

        while !(*node).parent.is_null()
            && (*(*node).parent).heap_prio > (*node).heap_prio
        {
            let p = (*node).parent;
            if (*p).left == node {
                rotate_right(root, p);
            } else {
                rotate_left(root, p);
            }
        }
    }
    1
}

/// Remove exactly `node`, returning zero if it is not in this tree.
///
/// # Safety
/// `root` and `node` must be valid writable pointers, and the caller must hold
/// the tree's lock. The node and owner remain alive throughout this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_eevdf_tree_remove(
    root: *mut *mut EevdfNode,
    node: *mut EevdfNode,
) -> u32 {
    if root.is_null() || node.is_null() {
        return 0;
    }
    // SAFETY: caller guarantees root/node validity and exclusive locked access.
    unsafe {
        if (*node).owner.is_null() {
            return 0;
        }
        let mut found = *root;
        while !found.is_null() && found != node {
            found = if key_less(node, found) {
                (*found).left
            } else {
                (*found).right
            };
        }
        if found != node {
            return 0;
        }

        while !(*node).left.is_null() && !(*node).right.is_null() {
            if (*(*node).left).heap_prio < (*(*node).right).heap_prio {
                rotate_right(root, node);
            } else {
                rotate_left(root, node);
            }
        }
        let child = if !(*node).left.is_null() {
            (*node).left
        } else {
            (*node).right
        };
        let parent = (*node).parent;
        if !child.is_null() {
            (*child).parent = parent;
        }
        if parent.is_null() {
            *root = child;
        } else if (*parent).left == node {
            (*parent).left = child;
        } else {
            (*parent).right = child;
        }
        (*node).left = core::ptr::null_mut();
        (*node).right = core::ptr::null_mut();
        (*node).parent = core::ptr::null_mut();
        (*node).owner = core::ptr::null_mut();
        (*node).min_vruntime = (*node).vruntime_key;
        augment_up(parent);
    }
    1
}

/// Return the owner of the smallest BST key, or null for an empty tree.
///
/// # Safety
/// `root` must be a valid tree root protected against concurrent mutation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_eevdf_tree_first(root: *mut EevdfNode) -> *mut c_void {
    // SAFETY: forwarded from the caller contract.
    unsafe { extreme(root, true) }
}

/// Return the owner of the largest BST key, or null for an empty tree.
///
/// # Safety
/// `root` must be a valid tree root protected against concurrent mutation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_eevdf_tree_last(root: *mut EevdfNode) -> *mut c_void {
    // SAFETY: forwarded from the caller contract.
    unsafe { extreme(root, false) }
}

/// Select the first in-order task whose vruntime is at most `vtime`.
///
/// # Safety
/// `root` must be a valid tree protected against concurrent mutation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_eevdf_tree_pick(
    mut root: *mut EevdfNode,
    vtime: u64,
) -> *mut c_void {
    // SAFETY: caller holds the runqueue lock; only tree fields are inspected.
    unsafe {
        if root.is_null() || (*root).min_vruntime > vtime {
            return core::ptr::null_mut();
        }
        loop {
            if !(*root).left.is_null() && (*(*root).left).min_vruntime <= vtime {
                root = (*root).left;
                continue;
            }
            if (*root).vruntime_key <= vtime {
                return (*root).owner;
            }
            if !(*root).right.is_null() && (*(*root).right).min_vruntime <= vtime {
                root = (*root).right;
                continue;
            }
            return core::ptr::null_mut();
        }
    }
}

/// Membership probe used by scheduler invariant checks.
///
/// # Safety
/// Both pointers refer to live nodes and the caller excludes tree mutation.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_eevdf_tree_contains(
    mut root: *mut EevdfNode,
    target: *const EevdfNode,
) -> u32 {
    if root.is_null() || target.is_null() {
        return 0;
    }
    // SAFETY: caller guarantees both node pointers remain live during lookup.
    unsafe {
        while !root.is_null() {
            if root == target.cast_mut() {
                return 1;
            }
            root = if key_less(target, root) {
                (*root).left
            } else {
                (*root).right
            };
        }
    }
    0
}

pub const NICE0_LOAD: u64 = 1024;
pub const DEFAULT_SLICE_DIVISOR: u64 = 100;

#[inline]
pub fn effective_weight(base: u32, boost: u32) -> u64 {
    let base = if base == 0 { NICE0_LOAD } else { base as u64 };
    core::cmp::max(base, boost as u64)
}

#[inline]
pub fn base_slice_ticks(configured_ms: i32, ticks_per_sec: u64) -> u64 {
    if configured_ms > 0 {
        (configured_ms as u64).wrapping_mul(ticks_per_sec / 1000)
    } else {
        ticks_per_sec / DEFAULT_SLICE_DIVISOR
    }
}

#[inline]
pub fn virtual_slice(configured_ms: i32, ticks_per_sec: u64, weight: u64) -> u64 {
    base_slice_ticks(configured_ms, ticks_per_sec)
        .wrapping_mul(NICE0_LOAD) / core::cmp::max(weight, 1)
}

#[inline]
pub fn advance_vruntime(vruntime: u64, elapsed: u64, weight: u64) -> u64 {
    vruntime.wrapping_add(
        elapsed.wrapping_mul(NICE0_LOAD) / core::cmp::max(weight, 1),
    )
}

#[inline]
pub fn deadline(vruntime: u64, slice: u64) -> u64 {
    vruntime.wrapping_add(slice)
}

#[inline]
pub fn advance_vtime(vtime: u64, elapsed: u64, total_weight: u64) -> u64 {
    if total_weight == 0 {
        vtime
    } else {
        vtime.wrapping_add(
            elapsed.wrapping_mul(NICE0_LOAD) / total_weight,
        )
    }
}

#[inline]
pub fn eligible(vruntime: u64, vtime: u64) -> bool {
    vruntime <= vtime
}

#[inline]
pub fn clamp_sleeper_vruntime(vruntime: u64, vtime: u64, max_lag: u64) -> u64 {
    if vruntime.wrapping_add(max_lag) < vtime {
        vtime.wrapping_sub(max_lag)
    } else {
        vruntime
    }
}

/// Preserve signed virtual lag when moving between independent CPU clocks.
#[unsafe(no_mangle)]
pub extern "C" fn a20_eevdf_rebase(vruntime: u64, source: u64, destination: u64) -> u64 {
    if vruntime >= source {
        destination.saturating_add(vruntime - source)
    } else {
        destination.saturating_sub(source - vruntime)
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn a20_eevdf_effective_weight(base: u32, boost: u32) -> u64 {
    effective_weight(base, boost)
}

#[unsafe(no_mangle)]
pub extern "C" fn a20_eevdf_virtual_slice(configured_ms: i32,
                                            ticks_per_sec: u64,
                                            weight: u64) -> u64 {
    virtual_slice(configured_ms, ticks_per_sec, weight)
}

#[unsafe(no_mangle)]
pub extern "C" fn a20_eevdf_advance_vruntime(vruntime: u64, elapsed: u64,
                                               weight: u64) -> u64 {
    advance_vruntime(vruntime, elapsed, weight)
}

#[unsafe(no_mangle)]
pub extern "C" fn a20_eevdf_deadline(vruntime: u64, slice: u64) -> u64 {
    deadline(vruntime, slice)
}

#[unsafe(no_mangle)]
pub extern "C" fn a20_eevdf_advance_vtime(vtime: u64, elapsed: u64,
                                             total_weight: u64) -> u64 {
    advance_vtime(vtime, elapsed, total_weight)
}

#[unsafe(no_mangle)]
pub extern "C" fn a20_eevdf_eligible(vruntime: u64, vtime: u64) -> u32 {
    eligible(vruntime, vtime) as u32
}

#[unsafe(no_mangle)]
pub extern "C" fn a20_eevdf_clamp_sleeper(vruntime: u64, vtime: u64,
                                              max_lag: u64) -> u64 {
    clamp_sleeper_vruntime(vruntime, vtime, max_lag)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn c_weight(base: u32, boost: u32) -> u64 {
        let base = if base != 0 { base as u64 } else { 1024 };
        if boost as u64 > base { boost as u64 } else { base }
    }

    fn c_vslice(ms: i32, hz: u64, weight: u64) -> u64 {
        let slice = if ms > 0 {
            (ms as u64).wrapping_mul(hz / 1000)
        } else {
            hz / 100
        };
        slice.wrapping_mul(1024) / weight
    }

    fn c_advance(v: u64, dt: u64, weight: u64) -> u64 {
        v.wrapping_add(dt.wrapping_mul(1024) / weight)
    }

    fn c_clamp(vruntime: u64, vtime: u64, lag: u64) -> u64 {
        if vruntime.wrapping_add(lag) < vtime {
            vtime.wrapping_sub(lag)
        } else {
            vruntime
        }
    }

    #[test]
    fn weights_and_pi_boost_match_c() {
        for base in [0, 1, 256, 1024, u32::MAX] {
            for boost in [0, 1, 512, 2048, u32::MAX] {
                assert_eq!(effective_weight(base, boost), c_weight(base, boost));
            }
        }
    }

    #[test]
    fn virtual_arithmetic_matches_c_boundaries() {
        let values = [0, 1, 1023, 1024, u64::MAX - 1, u64::MAX];
        for &v in &values {
            for &dt in &values {
                for weight in [1, 256, 1024, u32::MAX as u64] {
                    assert_eq!(advance_vruntime(v, dt, weight), c_advance(v, dt, weight));
                    assert_eq!(virtual_slice(10, 10_000_000, weight), c_vslice(10, 10_000_000, weight));
                }
            }
        }
        for configured_ms in [-1, 0, 1, 9, 1000, i32::MAX] {
            for hz in [0, 999, 1000, 10_000_000, u64::MAX] {
                for weight in [1, 1024, u32::MAX as u64] {
                    assert_eq!(virtual_slice(configured_ms, hz, weight),
                               c_vslice(configured_ms, hz, weight));
                }
            }
        }
    }

    #[test]
    fn virtual_time_and_eligibility_match_c() {
        for v in [0, 1, 100, u64::MAX] {
            for dt in [0, 1, 100, u64::MAX] {
                for weight in [0, 1, 1024, u64::MAX] {
                    let expected = if weight == 0 { v } else { c_advance(v, dt, weight) };
                    assert_eq!(advance_vtime(v, dt, weight), expected);
                }
            }
        }
        assert!(eligible(10, 10));
        assert!(!eligible(11, 10));
    }

    #[test]
    fn sleeper_clamp_and_deadline_match_c() {
        for v in [0, 1, u64::MAX - 1, u64::MAX] {
            for now in [0, 1, 100, u64::MAX] {
                for lag in [0, 1, 100, u64::MAX] {
                    assert_eq!(clamp_sleeper_vruntime(v, now, lag), c_clamp(v, now, lag));
                    assert_eq!(deadline(v, lag), v.wrapping_add(lag));
                }
            }
        }
    }

}

#[cfg(test)]
mod tree_tests {
    use super::*;
    use std::boxed::Box;
    use std::vec;
    use std::vec::Vec;

    fn node() -> Box<EevdfNode> {
        Box::new(EevdfNode {
            left: core::ptr::null_mut(),
            right: core::ptr::null_mut(),
            parent: core::ptr::null_mut(),
            min_vruntime: 0,
            heap_prio: 0,
            owner: core::ptr::null_mut(),
            deadline_key: 0,
            vruntime_key: 0,
        })
    }

    unsafe fn check_subtree(
        root: *mut EevdfNode,
        parent: *mut EevdfNode,
        lower: *mut EevdfNode,
        upper: *mut EevdfNode,
    ) -> Option<u64> {
        if root.is_null() {
            return None;
        }
        // SAFETY: test owns all nodes and checks each after every mutation.
        unsafe {
            let n = &*root;
            assert_eq!(n.parent, parent);
            if !lower.is_null() {
                assert!(key_less(lower, root));
            }
            if !upper.is_null() {
                assert!(key_less(root, upper));
            }
            if !n.left.is_null() {
                assert!(n.heap_prio <= (*n.left).heap_prio);
            }
            if !n.right.is_null() {
                assert!(n.heap_prio <= (*n.right).heap_prio);
            }
            let left = check_subtree(n.left, root, lower, root);
            let right = check_subtree(n.right, root, root, upper);
            let expected = [Some(n.vruntime_key), left, right]
                .into_iter()
                .flatten()
                .min()
                .unwrap();
            assert_eq!(n.min_vruntime, expected);
            Some(expected)
        }
    }

    unsafe fn ordered(root: *mut EevdfNode, out: &mut Vec<*mut EevdfNode>) {
        if root.is_null() {
            return;
        }
        // SAFETY: recursive traversal of the test-owned valid tree.
        unsafe {
            ordered((*root).left, out);
            out.push(root);
            ordered((*root).right, out);
        }
    }

    #[test]
    fn randomized_insert_remove_pick_and_augments() {
        const COUNT: usize = 127;
        let mut nodes = (0..COUNT).map(|_| node()).collect::<Vec<_>>();
        let mut root = core::ptr::null_mut();
        let mut order = (0..COUNT).collect::<Vec<_>>();
        let mut seed = 0x9e37_79b9_u32;
        for i in (1..COUNT).rev() {
            seed = seed.wrapping_mul(1664525).wrapping_add(1013904223);
            order.swap(i, (seed as usize) % (i + 1));
        }
        let mut active = vec![false; COUNT];
        for (step, &idx) in order.iter().enumerate() {
            let ptr = &mut *nodes[idx] as *mut EevdfNode;
            let deadline = ((idx * 17 + step * 3) % 43) as u64;
            let vruntime = ((idx * 29 + step * 11) % 61) as u64;
            let priority = ((idx * 13 + 7) % 31 + 1) as u32;
            // SAFETY: nodes remain boxed/stable and the test is single threaded.
            assert_eq!(unsafe {
                a20_eevdf_tree_insert(&mut root, ptr, ptr.cast(), deadline, vruntime, priority)
            }, 1);
            active[idx] = true;
            assert_eq!(unsafe {
                a20_eevdf_tree_insert(&mut root, ptr, ptr.cast(), deadline, vruntime, priority)
            }, 0);
            let mut other_root = core::ptr::null_mut();
            assert_eq!(unsafe {
                a20_eevdf_tree_insert(
                    &mut other_root,
                    ptr,
                    ptr.cast(),
                    deadline,
                    vruntime,
                    priority,
                )
            }, 0, "one intrusive node cannot belong to two queues");
            // SAFETY: validates all parent, heap, BST, and augmentation invariants.
            unsafe { check_subtree(root, core::ptr::null_mut(), core::ptr::null_mut(), core::ptr::null_mut()); }

            let mut sorted = active.iter().enumerate().filter_map(|(i, yes)| {
                if *yes { Some(i) } else { None }
            }).collect::<Vec<_>>();
            sorted.sort_by_key(|&i| {
                let n = &nodes[i];
                (n.deadline_key, n.vruntime_key, (&**n as *const EevdfNode) as usize)
            });
            let mut walk = Vec::new();
            // SAFETY: traverses the valid locked-equivalent test tree.
            unsafe { ordered(root, &mut walk); }
            assert_eq!(walk.len(), sorted.len());
            for (actual, expected) in walk.iter().zip(sorted.iter()) {
                assert_eq!(*actual, &mut *nodes[*expected] as *mut EevdfNode);
            }
            let first = sorted.first().map(|&i| (&mut *nodes[i] as *mut EevdfNode).cast()).unwrap_or(core::ptr::null_mut());
            let last = sorted.last().map(|&i| (&mut *nodes[i] as *mut EevdfNode).cast()).unwrap_or(core::ptr::null_mut());
            assert_eq!(unsafe { a20_eevdf_tree_first(root) }, first);
            assert_eq!(unsafe { a20_eevdf_tree_last(root) }, last);
            for vtime in [0, 10, 30, 60, 100] {
                let expected = sorted.iter().find_map(|&i| {
                    (nodes[i].vruntime_key <= vtime)
                        .then(|| (&mut *nodes[i] as *mut EevdfNode).cast())
                }).unwrap_or(core::ptr::null_mut());
                assert_eq!(unsafe { a20_eevdf_tree_pick(root, vtime) }, expected);
            }
        }

        for idx in order.into_iter().rev() {
            let ptr = &mut *nodes[idx] as *mut EevdfNode;
            assert_eq!(unsafe { a20_eevdf_tree_contains(root, ptr) }, 1);
            assert_eq!(unsafe { a20_eevdf_tree_remove(&mut root, ptr) }, 1);
            active[idx] = false;
            assert!(ptr.is_null() || unsafe { (*ptr).owner.is_null() });
            assert_eq!(unsafe { a20_eevdf_tree_remove(&mut root, ptr) }, 0);
            // SAFETY: all remaining nodes are still owned by this test.
            unsafe { check_subtree(root, core::ptr::null_mut(), core::ptr::null_mut(), core::ptr::null_mut()); }
            if !root.is_null() {
                let mut sorted = active.iter().enumerate().filter_map(|(i, yes)| {
                    if *yes { Some(i) } else { None }
                }).collect::<Vec<_>>();
                sorted.sort_by_key(|&i| {
                    let n = &nodes[i];
                    (n.deadline_key, n.vruntime_key, (&**n as *const EevdfNode) as usize)
                });
                assert_eq!(unsafe { a20_eevdf_tree_first(root) }, (&mut *nodes[sorted[0]] as *mut EevdfNode).cast());
                assert_eq!(unsafe { a20_eevdf_tree_last(root) }, (&mut *nodes[*sorted.last().unwrap()] as *mut EevdfNode).cast());
            } else {
                assert!(unsafe { a20_eevdf_tree_first(root) }.is_null());
                assert!(unsafe { a20_eevdf_tree_last(root) }.is_null());
                assert!(unsafe { a20_eevdf_tree_pick(root, u64::MAX) }.is_null());
            }
        }
    }

    #[test]
    fn ancestor_min_regression_old_local_update_would_miss_eligible_task() {
        let mut root = node();
        let mut eligible_left = node();
        let root_ptr = &mut *root as *mut EevdfNode;
        let child_ptr = &mut *eligible_left as *mut EevdfNode;
        root.deadline_key = 10;
        root.vruntime_key = 50;
        root.min_vruntime = 50;
        root.heap_prio = 1;
        root.owner = root_ptr.cast();
        eligible_left.deadline_key = 5;
        eligible_left.vruntime_key = 5;
        eligible_left.min_vruntime = 5;
        eligible_left.heap_prio = 2;
        eligible_left.owner = child_ptr.cast();
        eligible_left.parent = root_ptr;
        root.left = child_ptr;

        // The former C insertion recalculated only the inserted node. Its
        // ancestor still claimed min_vruntime=50, pruning this eligible node.
        assert!(unsafe { a20_eevdf_tree_pick(root_ptr, 10) }.is_null());
        // The production insertion/removal invariant propagates to ancestors.
        // SAFETY: these two boxed nodes form a valid tree for the test.
        unsafe { augment_up(root_ptr); }
        assert_eq!(unsafe { a20_eevdf_tree_pick(root_ptr, 10) }, child_ptr.cast());
    }
}
