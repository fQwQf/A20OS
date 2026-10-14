//! Shared ABI layout for the C-owned Native handle-table Entry.
//!
//! This type is included by the slot-transition and table-state rustc units so
//! both validate and use one Rust definition of the production C entry layout.

use core::ffi::c_void;
use core::mem::{align_of, offset_of, size_of};

#[repr(C)]
#[derive(Clone, Copy)]
pub struct Entry {
    pub(crate) object: *mut c_void,
    pub(crate) object_type: u16,
    pub(crate) pad: u16,
    pub(crate) rights: u64,
    pub(crate) expiry_tick: u64,
    pub(crate) remaining_ops: u32,
    pub(crate) temporal_flags: u32,
    pub(crate) security_label: u8,
    pub(crate) state: u8,
    pub(crate) pad2: [u8; 6],
}

const _: () = {
    assert!(size_of::<Entry>() == if usize::BITS == 64 { 48 } else { 40 });
    assert!(offset_of!(Entry, object) == 0);
    assert!(offset_of!(Entry, object_type) == size_of::<usize>());
    assert!(offset_of!(Entry, rights) == if usize::BITS == 64 { 16 } else { 8 });
    assert!(offset_of!(Entry, expiry_tick) == if usize::BITS == 64 { 24 } else { 16 });
    assert!(offset_of!(Entry, remaining_ops) == if usize::BITS == 64 { 32 } else { 24 });
    assert!(offset_of!(Entry, temporal_flags) == if usize::BITS == 64 { 36 } else { 28 });
    assert!(offset_of!(Entry, security_label) == if usize::BITS == 64 { 40 } else { 32 });
    #[cfg(target_arch = "arm")]
    assert!(align_of::<Entry>() == 8);
    #[cfg(not(target_arch = "arm"))]
    assert!(align_of::<Entry>() == 8);
};
