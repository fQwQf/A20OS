#![allow(non_camel_case_types)]

use core::ffi::c_int;

#[repr(C)]
pub struct vnode_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct mount_t {
    _opaque: [u8; 0],
}

unsafe extern "C" {
    pub fn vnode_get(vn: *mut vnode_t);
    pub fn vnode_put(vn: *mut vnode_t);
    pub fn vnode_ref_read(vn: *mut vnode_t) -> c_int;
    pub fn a20_dcache_dir_key(
        dir: *mut vnode_t,
        mnt_out: *mut *mut mount_t,
        ino_out: *mut u64,
    ) -> c_int;
    pub fn a20_dcache_mount_type(mnt: *mut mount_t) -> c_int;
}

pub const FS_TYPE_RAMFS: c_int = 1;
pub const FS_TYPE_FAT32: c_int = 2;
pub const FS_TYPE_EXT4: c_int = 3;
pub const MAX_NAME_LEN: usize = 256;
