use core::ffi::{c_int, c_void};

pub const PAGE_SIZE: usize = 4096;
pub const MAX_ORDER: c_int = 11;
pub const PFN_NONE: u32 = u32::MAX;
pub const FRAME_F_ALLOC: u8 = 0x01;

pub const SLAB_NR_CACHES: usize = 7;
pub const SLAB_SIZES: [usize; SLAB_NR_CACHES] = [32, 64, 128, 256, 512, 1024, 2048];
pub const SLAB_HDR_SIZE: usize = 64;
pub const SLAB_MAGIC: u32 = 0x534C4142;
pub const BIG_MAGIC: u32 = 0x42494741;
pub const SLAB_SPARE_CAP: usize = 2;
pub const SLAB_BITMAP_WORDS: usize = 2;
pub const SLAB_BITMAP_BITS: usize = SLAB_BITMAP_WORDS * 64;

#[repr(C)]
pub struct slab_stats_t {
    pub total_pages: usize,
    pub active_pages: usize,
    pub spare_pages: usize,
    pub allocated_objects: usize,
    pub allocated_bytes: usize,
    pub total_bytes: usize,
    pub reclaimable_bytes: usize,
}

extern "C" {
    pub fn pfa_alloc_page() -> u32;
    pub fn a20_pfa_free_page(pfn: u32);
    pub fn a20_pfa_alloc(order: c_int) -> u32;
    pub fn a20_pfa_free(pfn: u32, order: c_int);

    pub fn a20_pfn_to_virt(pfn: u32) -> *mut c_void;
    pub fn a20_virt_to_pfn(va: *const c_void) -> u32;
    pub fn a20_pfn_valid(pfn: u32) -> c_int;

    pub fn a20_pfa_meta_flags(pfn: u32) -> u8;
    pub fn a20_pfa_meta_refcount(pfn: u32) -> u16;

    pub fn a20_oom_try_reclaim();

    pub fn memset(dst: *mut c_void, value: c_int, n: usize) -> *mut c_void;
    pub fn memcpy(dst: *mut c_void, src: *const c_void, n: usize) -> *mut c_void;
    pub fn panic(fmt: *const core::ffi::c_char, ...) -> !;
}
