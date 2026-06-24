//! FFI bindings used by the Rust page cache implementation.

use core::ffi::{c_int, c_long, c_void};

pub const PAGE_SIZE: usize = 4096;

pub const EINVAL: c_int = 22;
pub const ENOMEM: c_int = 12;
pub const ENOSYS: c_int = 38;
pub const SEEK_SET: c_int = 0;

pub const VFS_FT_REGULAR: c_int = 1;

pub const PFN_NONE: u32 = u32::MAX;

#[repr(C)]
pub struct vnode_t {
    _ino: u64,
    pub type_: c_int,
    _mode: u32,
    _uid: u32,
    _gid: u32,
    pub size: usize,
    _ref_count: c_int,
    _pad: c_int,
    _parent: *mut vnode_t,
    _mnt: *mut c_void,
    _fs_data: *mut c_void,
    _ops: *mut c_void,
}

#[repr(C)]
pub struct vfile_t {
    pub vnode: *mut vnode_t,
    _flags: c_int,
    pub offset: usize,
    _offset_lock: [u8; 32],
    _ref_count: c_int,
    _owner_type: c_int,
    _owner_pid: c_int,
    _owner_signal: c_int,
    _seals: c_int,
    _lease: c_int,
    _rw_hint: u64,
    _path: [u8; 256],
    pub ops: *mut vfile_ops_t,
    _priv: *mut c_void,
}

#[repr(C)]
pub struct vfile_ops_t {
    pub read: Option<unsafe extern "C" fn(*mut vfile_t, *mut u8, usize) -> c_int>,
    _write: Option<unsafe extern "C" fn(*mut vfile_t, *const u8, usize) -> c_int>,
    pub lseek: Option<unsafe extern "C" fn(*mut vfile_t, c_long, c_int) -> c_long>,
    _readdir: *mut c_void,
    _ioctl: *mut c_void,
    _close: *mut c_void,
}

#[repr(C)]
pub struct page_cache_stats_t {
    pub capacity: usize,
    pub valid: usize,
    pub dirty: usize,
    pub pinned: usize,
    pub bytes: usize,
}

extern "C" {
    pub fn vnode_get(vn: *mut vnode_t);
    pub fn vnode_put(vn: *mut vnode_t);

    pub fn pfa_alloc_page() -> u32;

    pub fn a20_pfn_to_virt(pfn: u32) -> *mut c_void;
    pub fn a20_pfn_valid(pfn: u32) -> c_int;
    pub fn a20_frame_refcount(pfn: u32) -> u32;
    pub fn a20_vnode_writepage(vn: *mut vnode_t, index: u64, data: *const c_void, len: usize) -> c_int;
}
