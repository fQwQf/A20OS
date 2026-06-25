use core::ffi::{c_int, c_void};

#[repr(C)]
pub struct vfile_t {
    _opaque: [u8; 0],
}

unsafe extern "C" {
    pub fn a20_file_table_init();
    pub fn a20_file_vfile_alloc() -> *mut vfile_t;
    pub fn a20_file_vfile_free(vf: *mut vfile_t);
    pub fn a20_file_vfile_ref_init(vf: *mut vfile_t, refs: c_int);
    pub fn a20_file_vfile_get(vf: *mut vfile_t);
    pub fn a20_file_vfile_ref_read(vf: *mut vfile_t) -> c_int;
    pub fn a20_file_vfile_put_ref_only(vf: *mut vfile_t) -> c_int;
}

pub const VFS_MAX_OPEN: usize = 8192;

pub const EBADF: c_int = 9;
pub const EINVAL: c_int = 22;
pub const EMFILE: c_int = 24;
pub const EBUSY: c_int = 16;
