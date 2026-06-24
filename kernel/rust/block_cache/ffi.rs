//! FFI bindings used by the Rust block cache implementation.

use core::ffi::{c_int, c_void};

pub const BCACHE_BLOCK_SIZE: usize = 512;
pub const BCACHE_MAX_BLOCKS: usize = 1024;
pub const BCACHE_HASH_BUCKETS: usize = 1024;
pub const PCACHE_PAGE_SIZE: usize = 4096;
pub const PCACHE_MAX_PAGES: usize = 1024;
pub const PCACHE_HASH_BUCKETS: usize = 128;

#[repr(C)]
pub struct block_dev_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct bcache_stats_t {
    pub caches: usize,
    pub block_pool_bytes: usize,
    pub page_pool_bytes: usize,
    pub valid_blocks: usize,
    pub dirty_blocks: usize,
    pub valid_pages: usize,
    pub dirty_pages: usize,
}

unsafe extern "C" {
    pub fn kmalloc(size: usize) -> *mut c_void;
    pub fn kfree(ptr: *mut c_void);
    pub fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    pub fn memset(dst: *mut c_void, value: c_int, n: usize) -> *mut c_void;

    pub fn a20_block_dev_read(
        dev: *mut block_dev_t,
        lba: u64,
        buf: *mut c_void,
        count: usize,
    ) -> c_int;
    pub fn a20_block_dev_write(
        dev: *mut block_dev_t,
        lba: u64,
        buf: *const c_void,
        count: usize,
    ) -> c_int;
}
