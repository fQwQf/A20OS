pub use a20rust_support::vfs::{vfile_t, vnode_t};
use core::ffi::{c_int, c_void};

pub const PAGE_SIZE: usize = 4096;

pub const EINVAL: c_int = 22;
pub const ENOMEM: c_int = 12;
pub const ENOSYS: c_int = 38;
pub const SEEK_SET: c_int = 0;

pub const VFS_FT_REGULAR: c_int = 1;

pub const PFN_NONE: u32 = u32::MAX;

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
    pub fn a20_vnode_type(vn: *mut vnode_t) -> c_int;
    pub fn a20_vnode_size(vn: *mut vnode_t) -> usize;
    pub fn a20_vnode_writepage(vn: *mut vnode_t, index: u64, data: *const c_void, len: usize) -> c_int;
}
