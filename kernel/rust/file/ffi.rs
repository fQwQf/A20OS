use core::ffi::c_int;

pub use a20rust_support::vfs::vfile_t;

unsafe extern "C" {
    pub fn a20_file_table_init();
    pub fn a20_file_vfile_alloc() -> *mut vfile_t;
    pub fn a20_file_vfile_free(vf: *mut vfile_t);
}

pub const VFS_MAX_OPEN: usize = 8192;

pub const EBADF: c_int = 9;
pub const EINVAL: c_int = 22;
pub const EMFILE: c_int = 24;
pub const EBUSY: c_int = 16;
