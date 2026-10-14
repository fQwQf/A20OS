#![no_std]

//! Architecture-independent interval and coalescing policy for VMA lists.
//!
//! Callers hold the address-space mutation lock while walking the list. The C
//! adapter only snapshots the stable linked-list/backing fields; this module
//! owns the gap, overlap, and adjacent-merge decisions.

use core::ffi::c_void;
use core::ptr;

const VM_FILE: u64 = 1 << 13;
const VM_VMO: u64 = 1 << 14;
const VM_SYSV_SHM: u64 = 1 << 16;

#[repr(C)]
#[derive(Clone, Copy)]
struct RegionSnapshot {
    start: usize,
    end: usize,
    next: *const c_void,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
struct MergeSnapshot {
    start: usize,
    end: usize,
    backing_offset: u64,
    vm_flags: u64,
    pte_flags: usize,
    vmar_cap: u32,
    reserved: u32,
    vmar_owner: *const c_void,
    file: *const c_void,
    vmo: *const c_void,
    prev: *const c_void,
    next: *const c_void,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 24] = [(); core::mem::size_of::<RegionSnapshot>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 12] = [(); core::mem::size_of::<RegionSnapshot>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 88] = [(); core::mem::size_of::<MergeSnapshot>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 56] = [(); core::mem::size_of::<MergeSnapshot>()];

unsafe extern "C" {
    fn a20_vma_region_read(node: *const c_void, out: *mut RegionSnapshot) -> i32;
    fn a20_vma_merge_snapshot(node: *const c_void, out: *mut MergeSnapshot) -> i32;
    fn a20_vma_split_alloc(test_split: u32) -> *mut c_void;
    fn a20_vma_split_clone(head: *mut c_void, tail: *mut c_void, split: usize) -> i32;
    // Returns 1 when a reference was acquired, 0 when this mapping has no
    // resource of that kind, or a negative errno on failure.
    fn a20_vma_split_ref(tail: *mut c_void, kind: u32) -> i32;
    fn a20_vma_split_unref(tail: *mut c_void, kind: u32);
    fn a20_vma_split_free(tail: *mut c_void);
    fn a20_vma_split_retire(mm: *mut c_void, head: *mut c_void);
    fn a20_vma_split_publish(
        mm: *mut c_void,
        head: *mut c_void,
        tail: *mut c_void,
        split: usize,
    );
    fn a20_vma_split_annotate(mm: *mut c_void, head: *mut c_void, tail: *mut c_void);
    fn a20_vma_merge_commit(mm: *mut c_void, survivor: *mut c_void, absorbed: *mut c_void);
}

struct SplitReservation {
    tail: *mut c_void,
    acquired: u8,
    committed: bool,
}

impl Drop for SplitReservation {
    fn drop(&mut self) {
        if self.tail.is_null() || self.committed {
            return;
        }
        for kind in (1..=3).rev() {
            if self.acquired & (1 << kind) != 0 {
                unsafe { a20_vma_split_unref(self.tail, kind) };
            }
        }
        unsafe { a20_vma_split_free(self.tail) };
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_vma_merge_transaction_rust(
    mm: *mut c_void,
    node: *mut c_void,
    left_first: i32,
) -> *mut c_void {
    if mm.is_null() || node.is_null() {
        return ptr::null_mut();
    }
    let read = |entry: *const c_void| -> Option<MergeSnapshot> {
        let mut snapshot = MergeSnapshot::default();
        if entry.is_null() || unsafe { a20_vma_merge_snapshot(entry, &mut snapshot) } != 0 {
            None
        } else {
            Some(snapshot)
        }
    };
    let mut survivor = node;
    let merge_left = |survivor: &mut *mut c_void| {
        let Some(current) = read(*survivor) else { return };
        let Some(left) = read(current.prev) else { return };
        if mergeable(&left, &current) {
            unsafe { a20_vma_merge_commit(mm, current.prev.cast_mut(), *survivor) };
            *survivor = current.prev.cast_mut();
        }
    };
    let merge_right = |survivor: &mut *mut c_void| {
        let Some(current) = read(*survivor) else { return };
        let Some(right) = read(current.next) else { return };
        if mergeable(&current, &right) {
            unsafe { a20_vma_merge_commit(mm, *survivor, current.next.cast_mut()) };
        }
    };
    if left_first != 0 {
        merge_left(&mut survivor);
        merge_right(&mut survivor);
    } else {
        merge_right(&mut survivor);
        merge_left(&mut survivor);
    }
    survivor
}

/// Own the fallible split transaction: validate its geometry, reserve and
/// retain the tail before publication, then perform the infallible retire,
/// link/extent update, and re-annotation phases in order.
#[no_mangle]
pub unsafe extern "C" fn a20_vma_split_transaction_rust(
    mm: *mut c_void,
    head: *mut c_void,
    split: usize,
    test_split: u32,
    tail_out: *mut *mut c_void,
) -> i32 {
    if mm.is_null() || head.is_null() || tail_out.is_null() {
        return -22;
    }
    unsafe { *tail_out = ptr::null_mut() };
    let mut snapshot = MergeSnapshot::default();
    if unsafe { a20_vma_merge_snapshot(head, &mut snapshot) } != 0 {
        return -22;
    }
    if split <= snapshot.start || split >= snapshot.end {
        return 0;
    }
    if snapshot.vm_flags & (VM_FILE | VM_VMO) != 0
        && snapshot.backing_offset.checked_add((split - snapshot.start) as u64).is_none()
    {
        return -22;
    }

    let tail = unsafe { a20_vma_split_alloc(test_split) };
    if tail.is_null() {
        return -12;
    }
    let mut reservation = SplitReservation { tail, acquired: 0, committed: false };
    let result = unsafe { a20_vma_split_clone(head, tail, split) };
    if result != 0 {
        return result;
    }
    for kind in 1..=3 {
        let result = unsafe { a20_vma_split_ref(tail, kind) };
        if result < 0 {
            return result;
        }
        if result > 0 {
            reservation.acquired |= 1 << kind;
        }
    }

    unsafe {
        a20_vma_split_retire(mm, head);
        a20_vma_split_publish(mm, head, tail, split);
        a20_vma_split_annotate(mm, head, tail);
        *tail_out = tail;
    }
    reservation.committed = true;
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_vma_find_gap_rust(
    mut node: *const c_void,
    hint: usize,
    length: usize,
) -> usize {
    let mut cursor = hint;
    while !node.is_null() {
        let mut region = RegionSnapshot { start: 0, end: 0, next: ptr::null() };
        if unsafe { a20_vma_region_read(node, &mut region) } != 0 {
            return cursor;
        }
        if region.start >= cursor && region.start - cursor >= length {
            return cursor;
        }
        if region.end > cursor {
            cursor = region.end;
        }
        node = region.next;
    }
    cursor
}

#[no_mangle]
pub unsafe extern "C" fn a20_vma_range_overlaps_rust(
    mut node: *const c_void,
    start: usize,
    length: usize,
    ignore: *const c_void,
) -> i32 {
    let Some(end) = start.checked_add(length) else { return 1 };
    while !node.is_null() {
        let mut region = RegionSnapshot { start: 0, end: 0, next: ptr::null() };
        if unsafe { a20_vma_region_read(node, &mut region) } != 0 {
            return 1;
        }
        if node != ignore && region.start < end && region.end > start {
            return 1;
        }
        if region.start >= end {
            break;
        }
        node = region.next;
    }
    0
}

fn mergeable(a: &MergeSnapshot, b: &MergeSnapshot) -> bool {
    if a.end < a.start
        || b.end < b.start
        || a.end != b.start
        || ((a.vm_flags | b.vm_flags) & VM_SYSV_SHM) != 0
        || a.vm_flags != b.vm_flags
        || a.pte_flags != b.pte_flags
        || a.vmar_cap != b.vmar_cap
        || a.vmar_owner != b.vmar_owner
    {
        return false;
    }

    if (a.vm_flags | b.vm_flags) & VM_FILE != 0 {
        let Some(next_offset) = a.backing_offset.checked_add((a.end - a.start) as u64) else {
            return false;
        };
        return a.file == b.file && next_offset == b.backing_offset;
    }
    if (a.vm_flags | b.vm_flags) & VM_VMO != 0 {
        let Some(next_offset) = a.backing_offset.checked_add((a.end - a.start) as u64) else {
            return false;
        };
        return a.vm_flags & VM_VMO != 0
            && b.vm_flags & VM_VMO != 0
            && a.vmo == b.vmo
            && next_offset == b.backing_offset;
    }
    true
}

#[no_mangle]
pub unsafe extern "C" fn a20_vma_can_merge_rust(
    left: *const c_void,
    right: *const c_void,
) -> i32 {
    if left.is_null() || right.is_null() {
        return 0;
    }
    let mut a = MergeSnapshot::default();
    let mut b = MergeSnapshot::default();
    if unsafe { a20_vma_merge_snapshot(left, &mut a) } != 0
        || unsafe { a20_vma_merge_snapshot(right, &mut b) } != 0
    {
        return 0;
    }
    i32::from(mergeable(&a, &b))
}

#[cfg(test)]
mod tests {
    use super::*;

    static mut SPLIT_EVENTS: [u8; 8] = [0; 8];
    static mut SPLIT_EVENT_COUNT: usize = 0;
    static mut SPLIT_FAIL_KIND: u32 = 0;
    static mut SPLIT_REFS: [u32; 4] = [0; 4];
    static mut MERGE_EVENTS: [u8; 2] = [0; 2];
    static mut MERGE_EVENT_COUNT: usize = 0;

    #[repr(C)]
    struct TestNode {
        start: usize,
        end: usize,
        next: *const c_void,
        attrs: MergeSnapshot,
    }

    unsafe extern "C" fn test_read(node: *const c_void, out: *mut RegionSnapshot) -> i32 {
        if node.is_null() || out.is_null() { return -1; }
        let node = unsafe { &*node.cast::<TestNode>() };
        unsafe { *out = RegionSnapshot { start: node.start, end: node.end, next: node.next } };
        0
    }

    unsafe extern "C" fn test_merge_read(node: *const c_void, out: *mut MergeSnapshot) -> i32 {
        if node.is_null() || out.is_null() { return -1; }
        let node = unsafe { &*node.cast::<TestNode>() };
        unsafe { *out = node.attrs };
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_region_read(
        node: *const c_void,
        out: *mut RegionSnapshot,
    ) -> i32 { unsafe { test_read(node, out) } }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_merge_snapshot(
        node: *const c_void,
        out: *mut MergeSnapshot,
    ) -> i32 { unsafe { test_merge_read(node, out) } }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_split_alloc(_test_split: u32) -> *mut c_void {
        9usize as *mut c_void
    }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_split_clone(
        _head: *mut c_void,
        _tail: *mut c_void,
        _split: usize,
    ) -> i32 { 0 }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_split_ref(_tail: *mut c_void, kind: u32) -> i32 {
        unsafe {
            if SPLIT_FAIL_KIND == kind { return -12; }
            SPLIT_REFS[kind as usize] += 1;
        }
        1
    }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_split_unref(_tail: *mut c_void, kind: u32) {
        unsafe {
            SPLIT_REFS[kind as usize] -= 1;
            SPLIT_EVENTS[SPLIT_EVENT_COUNT] = kind as u8;
            SPLIT_EVENT_COUNT += 1;
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_split_free(_tail: *mut c_void) {
        unsafe {
            SPLIT_EVENTS[SPLIT_EVENT_COUNT] = 9;
            SPLIT_EVENT_COUNT += 1;
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_split_retire(_mm: *mut c_void, _head: *mut c_void) {
        unsafe {
            SPLIT_EVENTS[SPLIT_EVENT_COUNT] = 1;
            SPLIT_EVENT_COUNT += 1;
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_split_publish(
        _mm: *mut c_void,
        _head: *mut c_void,
        _tail: *mut c_void,
        _split: usize,
    ) {
        unsafe {
            SPLIT_EVENTS[SPLIT_EVENT_COUNT] = 2;
            SPLIT_EVENT_COUNT += 1;
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_split_annotate(
        _mm: *mut c_void,
        _head: *mut c_void,
        _tail: *mut c_void,
    ) {
        unsafe {
            SPLIT_EVENTS[SPLIT_EVENT_COUNT] = 3;
            SPLIT_EVENT_COUNT += 1;
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_vma_merge_commit(
        _mm: *mut c_void,
        survivor: *mut c_void,
        absorbed: *mut c_void,
    ) {
        let survivor = unsafe { &mut *survivor.cast::<TestNode>() };
        let absorbed = unsafe { &*absorbed.cast::<TestNode>() };
        unsafe {
            MERGE_EVENTS[MERGE_EVENT_COUNT] = if survivor.attrs.start == 20 { 1 } else { 2 };
            MERGE_EVENT_COUNT += 1;
        }
        survivor.attrs.end = absorbed.attrs.end;
        survivor.attrs.next = absorbed.attrs.next;
    }

    fn attrs(start: usize, end: usize) -> MergeSnapshot {
        MergeSnapshot { start, end, ..MergeSnapshot::default() }
    }

    #[test]
    fn interval_walk_matches_sorted_list_gap_and_overlap_contract() {
        let third = TestNode { start: 40, end: 60, next: ptr::null(), attrs: attrs(40, 60) };
        let second = TestNode { start: 20, end: 30, next: (&third as *const TestNode).cast(), attrs: attrs(20, 30) };
        let first = TestNode { start: 10, end: 15, next: (&second as *const TestNode).cast(), attrs: attrs(10, 15) };
        let head = (&first as *const TestNode).cast();
        assert_eq!(unsafe { a20_vma_find_gap_rust(head, 0, 8) }, 0);
        assert_eq!(unsafe { a20_vma_find_gap_rust(head, 12, 5) }, 15);
        assert_eq!(unsafe { a20_vma_find_gap_rust(head, 30, 8) }, 30);
        assert_eq!(unsafe { a20_vma_find_gap_rust(head, 55, 10) }, 60);
        assert_eq!(unsafe { a20_vma_range_overlaps_rust(head, 15, 5, ptr::null()) }, 0);
        assert_eq!(unsafe { a20_vma_range_overlaps_rust(head, 14, 1, ptr::null()) }, 1);
        assert_eq!(unsafe { a20_vma_range_overlaps_rust(head, 20, 1, (&second as *const TestNode).cast()) }, 0);
        assert_eq!(unsafe { a20_vma_range_overlaps_rust(head, usize::MAX - 3, 8, ptr::null()) }, 1);
    }

    #[test]
    fn merge_policy_preserves_backing_and_capability_identity() {
        let left = MergeSnapshot { start: 0x1000, end: 0x2000, backing_offset: 0x4000,
            vm_flags: VM_FILE, pte_flags: 0x25, vmar_cap: 7,
            vmar_owner: 1usize as *const c_void, file: 2usize as *const c_void, ..MergeSnapshot::default() };
        let right = MergeSnapshot { start: 0x2000, end: 0x3000, backing_offset: 0x5000,
            vm_flags: VM_FILE, pte_flags: 0x25, vmar_cap: 7,
            vmar_owner: 1usize as *const c_void, file: 2usize as *const c_void, ..MergeSnapshot::default() };
        assert!(mergeable(&left, &right));
        assert!(!mergeable(&left, &MergeSnapshot { backing_offset: 0x6000, ..right }));
        assert!(!mergeable(&left, &MergeSnapshot { file: 3usize as *const c_void, ..right }));
        assert!(!mergeable(&left, &MergeSnapshot { pte_flags: 0x27, ..right }));
        assert!(!mergeable(&left, &MergeSnapshot { vmar_cap: 3, ..right }));
        assert!(!mergeable(&left, &MergeSnapshot { vm_flags: VM_FILE | VM_SYSV_SHM, ..right }));
        let anonymous = MergeSnapshot { vm_flags: 0, backing_offset: u64::MAX, ..left };
        assert!(mergeable(&anonymous, &MergeSnapshot { start: anonymous.end, ..anonymous }));
    }

    #[test]
    fn vmo_merge_requires_same_object_and_contiguous_offset() {
        let left = MergeSnapshot { start: 0x1000, end: 0x2000, backing_offset: 0,
            vm_flags: VM_VMO, vmo: 7usize as *const c_void, ..MergeSnapshot::default() };
        let right = MergeSnapshot { start: 0x2000, end: 0x3000, backing_offset: 0x1000,
            vm_flags: VM_VMO, vmo: 7usize as *const c_void, ..MergeSnapshot::default() };
        assert!(mergeable(&left, &right));
        assert!(!mergeable(&left, &MergeSnapshot { vmo: 8usize as *const c_void, ..right }));
        assert!(!mergeable(&left, &MergeSnapshot { backing_offset: 0, ..right }));
    }

    #[test]
    fn split_transaction_reserves_before_publish_and_keeps_phase_order() {
        let mut head = TestNode {
            start: 0x1000,
            end: 0x3000,
            next: ptr::null(),
            attrs: attrs(0x1000, 0x3000),
        };
        let mm = 1usize as *mut c_void;
        let head_ptr = (&mut head as *mut TestNode).cast();
        let mut tail = ptr::null_mut();
        unsafe {
            SPLIT_EVENT_COUNT = 0;
            SPLIT_FAIL_KIND = 3;
            SPLIT_REFS = [0; 4];
        }
        assert_eq!(unsafe {
            a20_vma_split_transaction_rust(mm, head_ptr, 0x2000, 0, &mut tail)
        }, -12);
        assert!(tail.is_null());
        assert_eq!(unsafe { SPLIT_EVENT_COUNT }, 3);
        assert_eq!(unsafe { [SPLIT_EVENTS[0], SPLIT_EVENTS[1], SPLIT_EVENTS[2]] }, [2, 1, 9]);
        assert_eq!(unsafe { SPLIT_REFS }, [0; 4]);

        unsafe { SPLIT_FAIL_KIND = 0; SPLIT_EVENT_COUNT = 0; }
        assert_eq!(unsafe {
            a20_vma_split_transaction_rust(mm, head_ptr, 0x2000, 0, &mut tail)
        }, 0);
        assert_eq!(tail, 9usize as *mut c_void);
        assert_eq!(unsafe { SPLIT_REFS }, [0, 1, 1, 1]);
        assert_eq!(unsafe { SPLIT_EVENT_COUNT }, 3);
        assert_eq!(unsafe { [SPLIT_EVENTS[0], SPLIT_EVENTS[1], SPLIT_EVENTS[2]] }, [1, 2, 3]);
    }

    #[test]
    fn merge_transaction_can_match_insert_right_then_left_order() {
        let mut left = TestNode { start: 10, end: 20, next: ptr::null(), attrs: attrs(10, 20) };
        let mut middle = TestNode { start: 20, end: 30, next: ptr::null(), attrs: attrs(20, 30) };
        let right = TestNode { start: 30, end: 40, next: ptr::null(), attrs: attrs(30, 40) };
        left.attrs.next = (&middle as *const TestNode).cast();
        middle.attrs.prev = (&left as *const TestNode).cast();
        middle.attrs.next = (&right as *const TestNode).cast();
        unsafe { MERGE_EVENT_COUNT = 0; }
        let result = unsafe {
            a20_vma_merge_transaction_rust(
                1usize as *mut c_void,
                (&mut middle as *mut TestNode).cast(),
                0,
            )
        };
        assert_eq!(result, (&mut left as *mut TestNode).cast());
        assert_eq!(left.attrs.end, 40);
        assert_eq!(left.attrs.next, ptr::null());
        assert_eq!(unsafe { MERGE_EVENT_COUNT }, 2);
        assert_eq!(unsafe { MERGE_EVENTS }, [1, 2]);
    }
}
