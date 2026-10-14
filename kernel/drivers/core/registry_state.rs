//! Driver-core registry membership and capacity transactions.
//!
//! The C driver core keeps its existing lock and callback ordering. While
//! those locks are held, this module owns the slot-table mutations: duplicate
//! detection, capacity growth, publication, and swap-last removal.

#![no_std]

#[cfg(test)]
extern crate std;

use core::ffi::c_void;
use core::ptr;

const EEXIST: i32 = -17;
const EINVAL: i32 = -22;
const ENOMEM: i32 = -12;
const EOVERFLOW: i32 = -75;
const ENOENT: i32 = -2;

pub type GrowSlots = unsafe extern "C" fn(*mut c_void, usize) -> *mut c_void;

fn capacity_bytes(capacity: i32) -> Result<usize, i32> {
    if capacity < 0 {
        return Err(EINVAL);
    }
    let slots = capacity as usize;
    let max_slots = (isize::MAX as usize) / core::mem::size_of::<*mut c_void>();
    if slots > max_slots {
        return Err(EOVERFLOW);
    }
    Ok(slots * core::mem::size_of::<*mut c_void>())
}

fn next_capacity(capacity: i32) -> Result<(i32, usize), i32> {
    let next = if capacity == 0 {
        1
    } else {
        capacity.checked_mul(2).ok_or(EOVERFLOW)?
    };
    if next <= capacity {
        return Err(EOVERFLOW);
    }
    Ok((next, capacity_bytes(next)?))
}

/// Insert a non-null object into a pointer registry. `grow` must have krealloc
/// semantics: on failure it leaves the old allocation valid. The caller
/// serializes this operation with the driver-core operation and registry locks.
#[no_mangle]
pub unsafe extern "C" fn a20_driver_registry_insert(
    slots: *mut *mut c_void,
    count: *mut i32,
    capacity: *mut i32,
    item: *mut c_void,
    grow: Option<GrowSlots>,
) -> i32 {
    if slots.is_null() || count.is_null() || capacity.is_null() || item.is_null() {
        return EINVAL;
    }
    let len = unsafe { *count };
    let cap = unsafe { *capacity };
    if len < 0 || len > cap {
        return EINVAL;
    }
    if let Err(error) = capacity_bytes(cap) {
        return error;
    }
    if cap != 0 && unsafe { *slots }.is_null() {
        return EINVAL;
    }
    let table: *mut c_void = unsafe { *slots };
    let table_slots: *mut *mut c_void = table.cast();
    for i in 0..len as usize {
        if unsafe { table_slots.add(i).read() } == item {
            return EEXIST;
        }
    }

    let mut new_table = table;
    if len == cap {
        let (next, bytes) = match next_capacity(cap) {
            Ok(growth) => growth,
            Err(error) => return error,
        };
        let Some(grow_fn) = grow else { return ENOMEM };
        let allocated = unsafe { grow_fn(table, bytes) };
        if allocated.is_null() {
            return ENOMEM;
        }
        new_table = allocated;
        unsafe { *slots = new_table };
        unsafe { *capacity = next };
    }

    unsafe { new_table.cast::<*mut c_void>().add(len as usize).write(item) };
    unsafe { *count = len + 1 };
    0
}

/// Remove a member by swapping the last entry into its slot and clearing the
/// vacated pointer. This matches the existing registry's unordered semantics.
#[no_mangle]
pub unsafe extern "C" fn a20_driver_registry_remove(
    slots: *mut *mut c_void,
    count: *mut i32,
    item: *mut c_void,
) -> i32 {
    if slots.is_null() || count.is_null() || item.is_null() {
        return EINVAL;
    }
    let table: *mut c_void = unsafe { *slots };
    let len = unsafe { *count };
    if len <= 0 || table.is_null() {
        return ENOENT;
    }
    for i in 0..len as usize {
        if unsafe { table.cast::<*mut c_void>().add(i).read() } == item {
            let last = len as usize - 1;
            let entries = table.cast::<*mut c_void>();
            let replacement = unsafe { entries.add(last).read() };
            unsafe { entries.add(i).write(replacement) };
            unsafe { entries.add(last).write(ptr::null_mut()) };
            unsafe { *count = len - 1 };
            return 0;
        }
    }
    ENOENT
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::prelude::v1::*;
    use std::alloc::{alloc, dealloc, Layout};
    use std::cell::Cell;

    std::thread_local! {
        static OLD_CAPACITY: Cell<usize> = const { Cell::new(0) };
    }

    unsafe extern "C" fn grow(old: *mut c_void, size: usize) -> *mut c_void {
        let layout = Layout::from_size_align(size, core::mem::align_of::<*mut c_void>()).unwrap();
        let new = unsafe { alloc(layout) };
        if new.is_null() { return new.cast(); }
        if !old.is_null() {
            let old_bytes = OLD_CAPACITY.with(|capacity| capacity.get())
                * core::mem::size_of::<*mut c_void>();
            unsafe { ptr::copy_nonoverlapping(old.cast::<u8>(), new, old_bytes) };
            let previous = Layout::from_size_align(old_bytes, core::mem::align_of::<*mut c_void>()).unwrap();
            unsafe { dealloc(old.cast(), previous) };
        }
        OLD_CAPACITY.with(|capacity| capacity.set(size / core::mem::size_of::<*mut c_void>()));
        new.cast()
    }

    unsafe extern "C" fn fail_grow(_: *mut c_void, _: usize) -> *mut c_void {
        ptr::null_mut()
    }

    #[test]
    fn duplicate_and_growth_failure_preserve_membership() {
        let mut raw = ptr::null_mut();
        let mut count = 0;
        let mut capacity = 0;
        let a = 1usize as *mut c_void;
        let b = 2usize as *mut c_void;
        assert_eq!(unsafe { a20_driver_registry_insert(&mut raw, &mut count, &mut capacity, a, Some(grow)) }, 0);
        assert_eq!((count, capacity), (1, 1));
        assert_eq!(unsafe { a20_driver_registry_insert(&mut raw, &mut count, &mut capacity, a, Some(grow)) }, EEXIST);
        let before = unsafe { raw.cast::<*mut c_void>().read() };
        assert_eq!(unsafe { a20_driver_registry_insert(&mut raw, &mut count, &mut capacity, b, Some(fail_grow)) }, ENOMEM);
        assert_eq!((count, capacity), (1, 1));
        assert_eq!(unsafe { raw.cast::<*mut c_void>().read() }, before);
        assert_eq!(unsafe { a20_driver_registry_insert(&mut raw, &mut count, &mut capacity, b, Some(grow)) }, 0);
        assert_eq!((count, capacity), (2, 2));
        unsafe { dealloc(raw.cast(), Layout::from_size_align(2 * core::mem::size_of::<*mut c_void>(), core::mem::align_of::<*mut c_void>()).unwrap()) };
    }

    #[test]
    fn remove_middle_last_and_nonmember_clear_slots() {
        let mut values = [1usize as *mut c_void, 2usize as *mut c_void, 3usize as *mut c_void];
        let mut raw = values.as_mut_ptr().cast::<c_void>();
        let mut count = 3;
        assert_eq!(unsafe { a20_driver_registry_remove(&mut raw, &mut count, 2usize as *mut c_void) }, 0);
        assert_eq!(count, 2);
        assert_eq!(values, [1usize as *mut c_void, 3usize as *mut c_void, ptr::null_mut()]);
        assert_eq!(unsafe { a20_driver_registry_remove(&mut raw, &mut count, 9usize as *mut c_void) }, ENOENT);
        assert_eq!(count, 2);
        assert_eq!(unsafe { a20_driver_registry_remove(&mut raw, &mut count, 3usize as *mut c_void) }, 0);
        assert_eq!(values, [1usize as *mut c_void, ptr::null_mut(), ptr::null_mut()]);
    }

    #[test]
    fn invalid_state_and_capacity_overflow_fail_without_mutation() {
        let item = 1usize as *mut c_void;
        let max_slots = (isize::MAX as usize) / core::mem::size_of::<*mut c_void>();
        assert_eq!(
            capacity_bytes(i32::MAX),
            if i32::MAX as usize <= max_slots {
                Ok(i32::MAX as usize * core::mem::size_of::<*mut c_void>())
            } else {
                Err(EOVERFLOW)
            }
        );
        assert_eq!(next_capacity(i32::MAX), Err(EOVERFLOW));
        assert_eq!(next_capacity(0), Ok((1, core::mem::size_of::<*mut c_void>())));

        let mut raw = ptr::null_mut();
        let mut count = -1;
        let mut capacity = 0;
        assert_eq!(unsafe { a20_driver_registry_insert(&mut raw, &mut count, &mut capacity, item, Some(grow)) }, EINVAL);
        assert_eq!((raw as usize, count, capacity), (0, -1, 0));
        assert_eq!(unsafe { a20_driver_registry_insert(ptr::null_mut(), &mut count, &mut capacity, item, Some(grow)) }, EINVAL);
        assert_eq!(unsafe { a20_driver_registry_remove(ptr::null_mut(), &mut count, item) }, EINVAL);
    }
}
