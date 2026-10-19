#![no_std]

//! Swap slot allocation, reference counting and release.
//!
//! A swap entry is a reference to one slot.  The slot's `swap_map` word is the
//! number of holders, `UINT32_MAX` marks a bad slot, and zero means free; a
//! free-slot bitmap is an index over that map, never a second source of truth.
//! This module owns the lifecycle of those words:
//!
//!   * allocation takes a charge first, then finds a slot with the hint-first
//!     two-pass word scan, clears its bitmap bit **before** publishing the map
//!     entry (so a concurrent release of that slot cannot see it free), pins the
//!     cgroup owner, advances the hint and accounts the slot;
//!   * duplication increments the count, refusing a bad slot and the last
//!     representable reference instead of wrapping;
//!   * release decrements, and only on the last holder clears the owner,
//!     rewinds the hint, sets the bitmap bit and accounts the slot -- then
//!     uncharges and unpins the owner outside the area lock, because both can
//!     take other locks.
//!
//! The slot 0 is the area header and is never allocatable.  The bitmap and the
//! map are plain u32/u64 arrays, so the scan is the same arithmetic the C
//! reference did; what stays C is the area lock, the area/registry state, the
//! I/O path and the counter lock.

use core::ffi::c_void;

const MAX_SWAPFILES: usize = 16;
const SWAP_MAP_BAD: u32 = u32::MAX;

const SWP_TYPE_BITS: u32 = 4;
#[cfg(target_pointer_width = "64")]
const SWP_OFFSET_BITS: u32 = 40;
#[cfg(target_pointer_width = "32")]
const SWP_OFFSET_BITS: u32 = 20;

const EINVAL: i32 = -22;
const EOVERFLOW: i32 = -75;

/// One area's slot state, read under its lock.
#[repr(C)]
#[derive(Clone, Copy, Default)]
struct Area {
    map: *mut u32,
    bits: *mut u64,
    pages: u64,
    hint: u64,
    active: i32,
    reserved: i32,
}

#[cfg(target_pointer_width = "64")]
const _: [(); 40] = [(); core::mem::size_of::<Area>()];

unsafe extern "C" {
    fn a20_swap_lock(kind: u32) -> u64;
    fn a20_swap_unlock(kind: u32, flags: u64);
    fn a20_swap_area(kind: u32, out: *mut Area);
    fn a20_swap_set_hint(kind: u32, hint: u64);
    fn a20_swap_owner(kind: u32, offset: u64) -> *mut c_void;
    fn a20_swap_set_owner(kind: u32, offset: u64, owner: *mut c_void);
    fn a20_swap_account(kind: u32, alloc: i32);
    fn cg_mem_swap_charge_node(owner: *mut c_void, pages: u32) -> i32;
    fn cg_mem_swap_uncharge_node(owner: *mut c_void, pages: u32);
    fn cg_group_pin(owner: *mut c_void);
    fn cg_group_unpin(owner: *mut c_void);
}

#[inline]
fn swp_type(entry: u64) -> u32 {
    (entry & ((1u64 << SWP_TYPE_BITS) - 1)) as u32
}

#[inline]
fn swp_offset(entry: u64) -> u64 {
    (entry >> SWP_TYPE_BITS) & ((1u64 << SWP_OFFSET_BITS) - 1)
}

#[inline]
fn swp_entry(kind: u32, offset: u64) -> u64 {
    ((offset & ((1u64 << SWP_OFFSET_BITS) - 1)) << SWP_TYPE_BITS) | kind as u64
}

/// Index of the highest set bit.  `word != 0` is a precondition.
#[inline]
fn highest_bit(mut word: u64) -> u64 {
    let mut bit = 0u64;
    if word & 0xFFFF_FFFF_0000_0000 != 0 {
        word >>= 32;
        bit += 32;
    }
    if word & 0x0000_0000_FFFF_0000 != 0 {
        word >>= 16;
        bit += 16;
    }
    if word & 0x0000_0000_0000_FF00 != 0 {
        word >>= 8;
        bit += 8;
    }
    if word & 0x0000_0000_0000_00F0 != 0 {
        word >>= 4;
        bit += 4;
    }
    if word & 0x0000_0000_0000_000C != 0 {
        word >>= 2;
        bit += 2;
    }
    if word & 0x0000_0000_0000_0002 != 0 {
        bit += 1;
    }
    bit
}

/// Find a free slot at or after the area's hint, wrapping once.  Scanning whole
/// words keeps it cheap; the hint keeps it from re-walking the allocated
/// prefix.  Slot 0 is the header and is never offered.
unsafe fn find_free(area: &Area) -> Option<u64> {
    let pages = area.pages;
    if pages == 0 || area.bits.is_null() {
        return None;
    }
    let mut hint = area.hint;
    if hint < 1 {
        hint = 1;
    }
    if hint >= pages {
        hint = 1;
    }

    for pass in 0..2 {
        let (start, end) = if pass == 0 { (hint, pages) } else { (1, hint) };
        if start >= end {
            continue;
        }
        let first_w = start >> 6;
        let last_w = (end + 63) >> 6;
        let mut w = first_w;
        while w < last_w {
            let mut word = unsafe { *area.bits.add(w as usize) };
            if word != 0 {
                if w == first_w && (start & 63) != 0 {
                    word &= !((1u64 << (start & 63)) - 1);
                }
                if (end & 63) != 0 && w == (end >> 6) {
                    word &= (1u64 << (end & 63)) - 1;
                }
                if word != 0 {
                    let bit = highest_bit(word);
                    let idx = (w << 6) + bit;
                    if idx >= start && idx < end && idx < pages && idx != 0 {
                        return Some(idx);
                    }
                }
            }
            w += 1;
        }
    }
    None
}

#[no_mangle]
pub unsafe extern "C" fn get_swap_page_owned(owner: *mut c_void) -> u64 {
    if unsafe { cg_mem_swap_charge_node(owner, 1) } < 0 {
        return 0;
    }
    for kind in 0..MAX_SWAPFILES {
        let flags = unsafe { a20_swap_lock(kind as u32) };
        let mut area = Area::default();
        unsafe { a20_swap_area(kind as u32, &mut area) };
        if area.active != 0 && !area.map.is_null() {
            if let Some(offset) = unsafe { find_free(&area) } {
                // Clear the bit before publishing the map entry, so a
                // concurrent swap_free() for this slot cannot see it free.
                unsafe { *area.bits.add((offset >> 6) as usize) &= !(1u64 << (offset & 63)) };
                unsafe { *area.map.add(offset as usize) = 1 };
                unsafe { cg_group_pin(owner) };
                unsafe { a20_swap_set_owner(kind as u32, offset, owner) };
                unsafe { a20_swap_set_hint(kind as u32, offset.wrapping_add(1)) };
                unsafe { a20_swap_account(kind as u32, 1) };
                unsafe { a20_swap_unlock(kind as u32, flags) };
                return swp_entry(kind as u32, offset);
            }
        }
        unsafe { a20_swap_unlock(kind as u32, flags) };
    }
    unsafe { cg_mem_swap_uncharge_node(owner, 1) };
    0
}

#[no_mangle]
pub unsafe extern "C" fn get_swap_page() -> u64 {
    unsafe { get_swap_page_owned(core::ptr::null_mut()) }
}

#[no_mangle]
pub unsafe extern "C" fn swap_duplicate(entry: u64) -> i32 {
    let kind = swp_type(entry);
    let offset = swp_offset(entry);
    if kind as usize >= MAX_SWAPFILES {
        return EINVAL;
    }
    let flags = unsafe { a20_swap_lock(kind) };
    let mut area = Area::default();
    unsafe { a20_swap_area(kind, &mut area) };
    let mut r = EINVAL;
    if area.active != 0 && offset != 0 && offset < area.pages && !area.map.is_null() {
        let slot = unsafe { area.map.add(offset as usize) };
        let refs = unsafe { *slot };
        if refs != 0 && refs != SWAP_MAP_BAD {
            if refs == SWAP_MAP_BAD - 1 {
                r = EOVERFLOW;
            } else {
                unsafe { *slot = refs + 1 };
                r = 0;
            }
        }
    }
    unsafe { a20_swap_unlock(kind, flags) };
    r
}

#[no_mangle]
pub unsafe extern "C" fn swap_free(entry: u64) {
    let kind = swp_type(entry);
    let offset = swp_offset(entry);
    if kind as usize >= MAX_SWAPFILES {
        return;
    }

    let mut owner: *mut c_void = core::ptr::null_mut();
    let flags = unsafe { a20_swap_lock(kind) };
    let mut area = Area::default();
    unsafe { a20_swap_area(kind, &mut area) };
    if area.active != 0 && offset > 0 && offset < area.pages && !area.map.is_null() {
        let slot = unsafe { area.map.add(offset as usize) };
        let refs = unsafe { *slot };
        if refs != 0 && refs != SWAP_MAP_BAD {
            if refs > 1 {
                unsafe { *slot = refs - 1 };
                unsafe { a20_swap_unlock(kind, flags) };
                return;
            }
            unsafe { *slot = 0 };
            owner = unsafe { a20_swap_owner(kind, offset) };
            unsafe { a20_swap_set_owner(kind, offset, core::ptr::null_mut()) };
            // Rewind the hint so a nearly full area can reach this slot again
            // without a full wrap.
            if area.hint > offset {
                unsafe { a20_swap_set_hint(kind, offset) };
            }
            if !area.bits.is_null() {
                unsafe { *area.bits.add((offset >> 6) as usize) |= 1u64 << (offset & 63) };
            }
            unsafe { a20_swap_account(kind, 0) };
        }
    }
    unsafe { a20_swap_unlock(kind, flags) };
    // Both of these can take other locks: uncharge in the cgroup tree, unpin
    // the owner.  Neither may run under the area lock.
    unsafe { cg_mem_swap_uncharge_node(owner, 1) };
    unsafe { cg_group_unpin(owner) };
}
