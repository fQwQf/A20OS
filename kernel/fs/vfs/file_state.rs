//! Open-file I/O-path policy engine (Rust-owned split for `fs/vfs/file.c`).
//!
//! `file.c` decides, per read/write, which data path a file uses: the page
//! cache, a buffered write through the vnode's writepage hook, or a direct
//! vfile operation.  That decision is a pure function of the access mode, the
//! vnode's regular-file-ness, the mount's filesystem type/flags and whether
//! the vnode exposes readpage/writepage.  This module owns those predicates so
//! the policy lives in one place instead of being scattered across the read
//! and write implementations.
//!
//! The C adapter keeps the vfile/vnode mount lookups and the I/O itself.
//!
//! The filesystem-type and access-mode numbers are mirrored from
//! `fs/vfs.h` / `core/fcntl.h`; the adapter asserts them against the real
//! definitions so drift cannot go unnoticed.

#![cfg_attr(not(test), no_std)]

use core::ffi::c_int;

// Access modes (core/fcntl.h).
pub const O_RDONLY: c_int = 0;
pub const O_WRONLY: c_int = 1;
pub const O_RDWR: c_int = 2;
pub const O_ACCMODE: c_int = 3;

// Filesystem types (fs/vfs.h).
pub const FS_TYPE_RAMFS: c_int = 1;
pub const FS_TYPE_EXT4: c_int = 3;
pub const FS_TYPE_PROCFS: c_int = 4;
pub const FS_TYPE_DEVFS: c_int = 5;
pub const FS_TYPE_CGROUP: c_int = 6;
pub const FS_TYPE_SYSFS: c_int = 7;
pub const FS_TYPE_UXFS: c_int = 10;
pub const FS_TYPE_LITTLEFS: c_int = 11;

pub const VFS_MOUNT_RDONLY: u32 = 0x1;

/// `read(2)` is allowed for a read-only or read-write descriptor.
#[no_mangle]
pub extern "C" fn a20_vfs_should_read(flags: c_int) -> c_int {
    let acc = flags & O_ACCMODE;
    if acc == O_RDONLY || acc == O_RDWR {
        1
    } else {
        0
    }
}

/// `write(2)` is allowed for a write-only or read-write descriptor.
#[no_mangle]
pub extern "C" fn a20_vfs_should_write(flags: c_int) -> c_int {
    let acc = flags & O_ACCMODE;
    if acc == O_WRONLY || acc == O_RDWR {
        1
    } else {
        0
    }
}

/// Whether a vnode's data goes through the VFS page cache.  Pseudo/synthetic
/// filesystems and littlefs serve their own data (the latter through
/// `lfs_file` with its own buffering, and no readpage/writepage hooks).
#[no_mangle]
pub extern "C" fn a20_vfs_file_uses_page_cache(is_regular: c_int, mnt_type: c_int) -> c_int {
    if is_regular == 0 {
        return 0;
    }
    match mnt_type {
        FS_TYPE_RAMFS | FS_TYPE_PROCFS | FS_TYPE_CGROUP | FS_TYPE_DEVFS | FS_TYPE_SYSFS
        | FS_TYPE_LITTLEFS => 0,
        _ => 1,
    }
}

/// Whether a write is buffered through the page cache and later flushed by
/// writepage: only the journaled/network backends (ext4, uxfs), only when the
/// mount is writable and the vnode exposes both hooks.
#[no_mangle]
pub extern "C" fn a20_vfs_file_uses_buffered_write(
    is_regular: c_int,
    mnt_type: c_int,
    mnt_flags: u32,
    has_readpage: c_int,
    has_writepage: c_int,
) -> c_int {
    if a20_vfs_file_uses_page_cache(is_regular, mnt_type) == 0 {
        return 0;
    }
    if mnt_flags & VFS_MOUNT_RDONLY != 0 {
        return 0;
    }
    if mnt_type != FS_TYPE_EXT4 && mnt_type != FS_TYPE_UXFS {
        return 0;
    }
    if has_readpage == 0 || has_writepage == 0 {
        return 0;
    }
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn read_write_access_modes() {
        assert_eq!(a20_vfs_should_read(O_RDONLY), 1);
        assert_eq!(a20_vfs_should_read(O_RDWR), 1);
        assert_eq!(a20_vfs_should_read(O_WRONLY), 0);
        assert_eq!(a20_vfs_should_write(O_WRONLY), 1);
        assert_eq!(a20_vfs_should_write(O_RDWR), 1);
        assert_eq!(a20_vfs_should_write(O_RDONLY), 0);
    }

    #[test]
    fn page_cache_is_for_regular_files_on_real_backends() {
        assert_eq!(a20_vfs_file_uses_page_cache(1, FS_TYPE_EXT4), 1);
        assert_eq!(a20_vfs_file_uses_page_cache(1, FS_TYPE_UXFS), 1);
        assert_eq!(a20_vfs_file_uses_page_cache(1, FS_TYPE_RAMFS), 0);
        assert_eq!(a20_vfs_file_uses_page_cache(1, FS_TYPE_PROCFS), 0);
        assert_eq!(a20_vfs_file_uses_page_cache(1, FS_TYPE_LITTLEFS), 0);
        /* a directory is never buffered through the page cache */
        assert_eq!(a20_vfs_file_uses_page_cache(0, FS_TYPE_EXT4), 0);
    }

    #[test]
    fn buffered_write_needs_writable_hooks_on_ext4_uxfs() {
        assert_eq!(a20_vfs_file_uses_buffered_write(1, FS_TYPE_EXT4, 0, 1, 1), 1);
        assert_eq!(a20_vfs_file_uses_buffered_write(1, FS_TYPE_UXFS, 0, 1, 1), 1);
        /* read-only mount */
        assert_eq!(
            a20_vfs_file_uses_buffered_write(1, FS_TYPE_EXT4, VFS_MOUNT_RDONLY, 1, 1),
            0
        );
        /* missing hooks */
        assert_eq!(a20_vfs_file_uses_buffered_write(1, FS_TYPE_EXT4, 0, 0, 1), 0);
        assert_eq!(a20_vfs_file_uses_buffered_write(1, FS_TYPE_EXT4, 0, 1, 0), 0);
        /* a backend with no page cache never buffers writes */
        assert_eq!(a20_vfs_file_uses_buffered_write(1, FS_TYPE_RAMFS, 0, 1, 1), 0);
    }
}
