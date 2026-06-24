use core::ffi::c_int;

pub const VFS_TIME_META_MAX: usize = 8192;
pub const LINUX_UTIME_NOW: u64 = 0x3fffffff;
pub const LINUX_UTIME_OMIT: u64 = 0x3ffffffe;
pub const MAX_GROUPS: usize = 32;

pub const F_OK: c_int = 0;
pub const R_OK: c_int = 4;
pub const W_OK: c_int = 2;
pub const X_OK: c_int = 1;

pub const EINVAL: c_int = 22;
pub const EACCES: c_int = 13;
pub const EPERM: c_int = 1;
pub const ENOSPC: c_int = 28;

pub const CAP_DAC_OVERRIDE: c_int = 1;
pub const CAP_DAC_READ_SEARCH: c_int = 2;
pub const CAP_FOWNER: c_int = 3;

pub const S_IFMT: u32 = 0o170000;
pub const S_IFREG: u32 = 0o100000;
pub const S_IFIFO: u32 = 0o010000;
pub const S_IFCHR: u32 = 0o020000;
pub const S_ISVTX: u32 = 0o1000;
pub const S_IXUSR: u32 = 0o100;
pub const S_IXGRP: u32 = 0o010;
pub const S_IXOTH: u32 = 0o001;

/// C-compatible `kstat_t`. Must match `kernel/include/fs/vfs.h`.
#[repr(C)]
pub struct kstat_t {
    pub st_dev: u64,
    pub st_ino: u64,
    pub st_mode: u32,
    pub st_nlink: u32,
    pub st_uid: u32,
    pub st_gid: u32,
    pub st_rdev: u64,
    pub st_size: u64,
    pub st_blksize: u64,
    pub st_blocks: u64,
    pub st_atime: u64,
    pub st_atime_nsec: u64,
    pub st_mtime: u64,
    pub st_mtime_nsec: u64,
    pub st_ctime: u64,
    pub st_ctime_nsec: u64,
}

/// Credential subset used by stat/permission checks.
#[repr(C)]
pub struct vfs_cred_t {
    pub uid: c_int,
    pub gid: c_int,
    pub egid: c_int,
    pub fsgid: c_int,
    pub fsuid: c_int,
    pub ngroups: c_int,
    pub groups: [c_int; MAX_GROUPS],
}

extern "C" {
    pub fn a20_proc_current() -> *mut core::ffi::c_void;
    pub fn a20_proc_has_cap(t: *mut core::ffi::c_void, cap: c_int) -> c_int;
    pub fn a20_proc_get_cred(t: *mut core::ffi::c_void, out: *mut vfs_cred_t);

    pub fn a20_timekeeping_get_realtime(now: *mut u64);

    pub fn a20_vnode_key(vn: *mut core::ffi::c_void, mnt_out: *mut *mut core::ffi::c_void, ino_out: *mut u64);
    pub fn a20_vnode_stat_op(vn: *mut core::ffi::c_void, st: *mut kstat_t) -> c_int;
}
