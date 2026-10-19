//! VFS directory-mutation orchestration engine (Rust-owned split for `vfs.c`).
//!
//! `vfs_unlink`, `vfs_rmdir` and `vfs_rename_flags` were three inline C
//! sequences that decided, in a specific order, whether a mutation is allowed
//! and what happens after it.  This module owns that order and those decisions:
//!
//! * the argument and `RENAME_*` flag validation (`NOREPLACE`/`EXCHANGE` are
//!   mutually exclusive and nothing else is accepted);
//! * resolving both operands through the mount table and the process root, and
//!   the same-mount (`-EXDEV`) requirement for rename;
//! * the parent-directory checks in their original order -- type, then
//!   `W_OK|X_OK` permission, then the operation's presence (whose absence is
//!   `-ENOTDIR` for unlink and `-ENOSYS` for rmdir/rename);
//! * the victim probe and the sticky-bit removal gate, including rename's
//!   `-EEXIST` for `RENAME_NOREPLACE`;
//! * the post-success side effects and their order: the inotify/fs notifications,
//!   the quota release, the self-delete notification and the dentry-cache
//!   invalidation (the whole-cache drop for a RAMFS directory reparent).
//!
//! The adapter keeps the mechanical half behind `VfsMutateOps`: the path
//! plumbing (the Rust-owned string layer plus the mount table), the vnode
//! references, the filesystem `ops->lookup`/`unlink`/`rmdir`/`rename` calls,
//! the permission and sticky-bit checks, and the notification/quota/dcache
//! side-effect calls.  Nothing here takes a lock, allocates or sleeps.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};

pub const VFS_FT_DIR: c_int = 2;

pub const RENAME_NOREPLACE: u32 = 1;
pub const RENAME_EXCHANGE: u32 = 2;

pub const W_OK: c_int = 2;
pub const X_OK: c_int = 1;

pub const ENOENT: c_int = -2;
pub const EACCES: c_int = -13;
pub const EEXIST: c_int = -17;
pub const EXDEV: c_int = -18;
pub const ENOTDIR: c_int = -20;
pub const EINVAL: c_int = -22;
pub const ENAMETOOLONG: c_int = -36;
pub const ENOSYS: c_int = -38;

const SCRATCH: usize = 512;

/// Rust mirror of the adapter's vtable (`fs/vfs/mutate_state.h`).
#[repr(C)]
pub struct VfsMutateOps {
    // path plumbing
    pub join: unsafe extern "C" fn(
        cwd: *const u8,
        path: *const u8,
        out: *mut u8,
        outsz: usize,
    ) -> c_int,
    pub split_parent_name: unsafe extern "C" fn(
        path: *const u8,
        parent: *mut u8,
        parent_sz: usize,
        name: *mut u8,
        name_sz: usize,
        max_path_len: usize,
    ) -> c_int,
    pub normalize_with_root:
        unsafe extern "C" fn(path: *mut u8, root: *const u8, max_path_len: usize) -> c_int,
    pub is_beneath: unsafe extern "C" fn(start: *const u8, resolved: *const u8) -> c_int,
    pub find_mount: unsafe extern "C" fn(path: *const u8) -> *mut c_void,
    pub strip_mount_prefix: unsafe extern "C" fn(path: *const u8, mnt: *const c_void) -> *const u8,
    pub mnt_root: unsafe extern "C" fn(mnt: *const c_void) -> *mut c_void,
    pub mnt_type: unsafe extern "C" fn(mnt: *const c_void) -> c_int,
    /// `vnode_lookup_path(mnt, rel)`: referenced vnode or null.
    pub lookup_path: unsafe extern "C" fn(mnt: *mut c_void, rel: *const u8) -> *mut c_void,
    pub lookup_errno: unsafe extern "C" fn() -> c_int,
    // vnode accessors and references
    pub vnode_put: unsafe extern "C" fn(vn: *mut c_void),
    pub vn_type: unsafe extern "C" fn(vn: *const c_void) -> c_int,
    pub vn_ino: unsafe extern "C" fn(vn: *const c_void) -> u64,
    pub vn_size: unsafe extern "C" fn(vn: *const c_void) -> usize,
    pub vn_mnt: unsafe extern "C" fn(vn: *const c_void) -> *mut c_void,
    // directory operations
    pub has_lookup: unsafe extern "C" fn(dir: *const c_void) -> c_int,
    pub lookup:
        unsafe extern "C" fn(dir: *mut c_void, name: *const u8, out: *mut *mut c_void) -> c_int,
    pub has_unlink: unsafe extern "C" fn(dir: *const c_void) -> c_int,
    pub has_rmdir: unsafe extern "C" fn(dir: *const c_void) -> c_int,
    pub has_rename: unsafe extern "C" fn(dir: *const c_void) -> c_int,
    pub do_unlink: unsafe extern "C" fn(dir: *mut c_void, name: *const u8) -> c_int,
    pub do_rmdir: unsafe extern "C" fn(dir: *mut c_void, name: *const u8) -> c_int,
    pub do_rename: unsafe extern "C" fn(
        old_dir: *mut c_void,
        old_name: *const u8,
        new_dir: *mut c_void,
        new_name: *const u8,
        flags: u32,
    ) -> c_int,
    pub permission: unsafe extern "C" fn(vn: *mut c_void, mask: c_int) -> c_int,
    pub sticky_may_remove:
        unsafe extern "C" fn(dir: *mut c_void, victim: *mut c_void) -> c_int,
    // side effects
    pub notify_delete: unsafe extern "C" fn(dir: *mut c_void, name: *const u8, is_dir: c_int),
    pub notify_rename: unsafe extern "C" fn(
        old_dir: *mut c_void,
        old_name: *const u8,
        new_dir: *mut c_void,
        new_name: *const u8,
    ),
    pub notify_self_delete: unsafe extern "C" fn(vn: *mut c_void),
    pub quota_remove: unsafe extern "C" fn(vn: *mut c_void, size: usize),
    pub dcache_invalidate: unsafe extern "C" fn(dir: *mut c_void, name: *const u8),
    pub dcache_invalidate_all: unsafe extern "C" fn(),
    pub max_path_len: usize,
    pub max_name_len: usize,
}

#[inline]
unsafe fn slen(p: *const u8) -> usize {
    let mut n = 0usize;
    // SAFETY: NUL-terminated by contract.
    while unsafe { *p.add(n) } != 0 {
        n += 1;
    }
    n
}

/// Copy a NUL-terminated C string into a Rust buffer, bounded like `strncpy`.
unsafe fn load(dst: &mut [u8], src: *const u8) {
    if src.is_null() {
        dst[0] = 0;
        return;
    }
    // SAFETY: src is NUL-terminated by contract.
    let n = core::cmp::min(unsafe { slen(src) }, dst.len() - 1);
    // SAFETY: n bytes are readable at src and fit in dst.
    unsafe { core::ptr::copy_nonoverlapping(src, dst.as_mut_ptr(), n) };
    dst[n] = 0;
}

/// The parent-directory checks that unlink and rmdir share, in their original
/// order: type, then `W_OK|X_OK` permission, then the operation's presence.
unsafe fn check_parent(
    ops: &VfsMutateOps,
    parent: *mut c_void,
    op_missing_err: c_int,
    has_op: unsafe extern "C" fn(*const c_void) -> c_int,
) -> Result<(), c_int> {
    // SAFETY: parent is a referenced vnode held by the caller.
    unsafe {
        if (ops.vn_type)(parent) != VFS_FT_DIR {
            (ops.vnode_put)(parent);
            return Err(ENOTDIR);
        }
        if (ops.permission)(parent, W_OK | X_OK) < 0 {
            (ops.vnode_put)(parent);
            return Err(EACCES);
        }
        if has_op(parent) == 0 {
            (ops.vnode_put)(parent);
            return Err(op_missing_err);
        }
        Ok(())
    }
}

/// The shared parent directory and leaf name of a mutation.
struct Parent {
    dir: *mut c_void,
    name: [u8; SCRATCH],
}

/// `vfs_unlink`'s prelude: resolve, strip the mount prefix, then split, so the
/// parent is looked up relative to the mount that owns the full path.
unsafe fn resolve_parent_unlink(
    ops: &VfsMutateOps,
    cwd: *const u8,
    path: *const u8,
) -> Result<Parent, c_int> {
    // SAFETY: all pointers come from the adapter's contract.
    unsafe {
        let mut resolved = [0u8; SCRATCH];
        let pr = (ops.join)(cwd, path, resolved.as_mut_ptr(), ops.max_path_len);
        if pr < 0 {
            return Err(pr);
        }
        let mnt = (ops.find_mount)(resolved.as_ptr());
        if mnt.is_null() || (ops.mnt_root)(mnt).is_null() {
            return Err(ENOENT);
        }
        let rel = (ops.strip_mount_prefix)(resolved.as_ptr(), mnt);
        let mut parent_path = [0u8; SCRATCH];
        let mut name = [0u8; SCRATCH];
        let sr = (ops.split_parent_name)(
            rel,
            parent_path.as_mut_ptr(),
            ops.max_path_len,
            name.as_mut_ptr(),
            ops.max_name_len,
            ops.max_path_len,
        );
        if sr < 0 {
            return Err(sr);
        }
        let parent = (ops.lookup_path)(mnt, parent_path.as_ptr());
        if parent.is_null() {
            let e = (ops.lookup_errno)();
            return Err(if e != 0 { e } else { ENOENT });
        }
        check_parent(ops, parent, ENOTDIR, ops.has_unlink)?;
        Ok(Parent { dir: parent, name })
    }
}

/// `vfs_rmdir`'s prelude: split the resolved path first and find the mount that
/// owns the *parent* directory.  This differs from unlink's order and the two
/// disagree for a path that names a mount point.
unsafe fn resolve_parent_rmdir(
    ops: &VfsMutateOps,
    cwd: *const u8,
    path: *const u8,
) -> Result<Parent, c_int> {
    // SAFETY: all pointers come from the adapter's contract.
    unsafe {
        let mut resolved = [0u8; SCRATCH];
        let pr = (ops.join)(cwd, path, resolved.as_mut_ptr(), ops.max_path_len);
        if pr < 0 {
            return Err(pr);
        }
        let mut parent_path = [0u8; SCRATCH];
        let mut name = [0u8; SCRATCH];
        let sr = (ops.split_parent_name)(
            resolved.as_ptr(),
            parent_path.as_mut_ptr(),
            ops.max_path_len,
            name.as_mut_ptr(),
            ops.max_name_len,
            ops.max_path_len,
        );
        if sr < 0 {
            return Err(sr);
        }
        let mnt = (ops.find_mount)(parent_path.as_ptr());
        if mnt.is_null() || (ops.mnt_root)(mnt).is_null() {
            return Err(ENOENT);
        }
        let parent = (ops.lookup_path)(mnt, (ops.strip_mount_prefix)(parent_path.as_ptr(), mnt));
        if parent.is_null() {
            let e = (ops.lookup_errno)();
            return Err(if e != 0 { e } else { ENOENT });
        }
        check_parent(ops, parent, ENOSYS, ops.has_rmdir)?;
        Ok(Parent { dir: parent, name })
    }
}

/// The victim probe plus the sticky-bit gate.  Returns the referenced victim
/// (or null) or the errno to report.
unsafe fn probe_victim(
    ops: &VfsMutateOps,
    dir: *mut c_void,
    name: *const u8,
) -> Result<*mut c_void, c_int> {
    // SAFETY: dir is a live directory vnode held by the caller.
    unsafe {
        if (ops.has_lookup)(dir) == 0 {
            return Ok(core::ptr::null_mut());
        }
        let mut victim: *mut c_void = core::ptr::null_mut();
        if (ops.lookup)(dir, name, &mut victim) == 0 && !victim.is_null() {
            let sr = (ops.sticky_may_remove)(dir, victim);
            if sr < 0 {
                (ops.vnode_put)(victim);
                return Err(sr);
            }
        }
        Ok(victim)
    }
}

/// `vfs_unlink`.
///
/// # Safety
/// `ops` is a live vtable; `cwd` and `path` are NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_unlink(
    ops: *const VfsMutateOps,
    cwd: *const u8,
    path: *const u8,
) -> c_int {
    if ops.is_null() {
        return EINVAL;
    }
    // SAFETY: forwarded from the caller's contract.
    let ops = unsafe { &*ops };
    // SAFETY: forwarded from the caller's contract.
    let parent = match unsafe { resolve_parent_unlink(ops, cwd, path) } {
        Ok(p) => p,
        Err(e) => return e,
    };
    // SAFETY: parent.dir is referenced and held for this scope.
    unsafe {
        let victim = match probe_victim(ops, parent.dir, parent.name.as_ptr()) {
            Ok(v) => v,
            Err(e) => {
                (ops.vnode_put)(parent.dir);
                return e;
            }
        };
        let r = (ops.do_unlink)(parent.dir, parent.name.as_ptr());
        if r == 0 {
            (ops.notify_delete)(parent.dir, parent.name.as_ptr(), 0);
            if !victim.is_null() {
                (ops.quota_remove)(victim, (ops.vn_size)(victim));
                (ops.notify_self_delete)(victim);
            }
            (ops.dcache_invalidate)(parent.dir, parent.name.as_ptr());
        }
        (ops.vnode_put)(victim);
        (ops.vnode_put)(parent.dir);
        r
    }
}

/// `vfs_rmdir`.
///
/// # Safety
/// As `a20_vfs_unlink`.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_rmdir(
    ops: *const VfsMutateOps,
    cwd: *const u8,
    path: *const u8,
) -> c_int {
    if ops.is_null() {
        return EINVAL;
    }
    // SAFETY: forwarded from the caller's contract.
    let ops = unsafe { &*ops };
    // SAFETY: forwarded from the caller's contract.
    let parent = match unsafe { resolve_parent_rmdir(ops, cwd, path) } {
        Ok(p) => p,
        Err(e) => return e,
    };
    // SAFETY: parent.dir is referenced and held for this scope.
    unsafe {
        let victim = match probe_victim(ops, parent.dir, parent.name.as_ptr()) {
            Ok(v) => v,
            Err(e) => {
                (ops.vnode_put)(parent.dir);
                return e;
            }
        };
        let r = (ops.do_rmdir)(parent.dir, parent.name.as_ptr());
        if r == 0 {
            (ops.notify_delete)(parent.dir, parent.name.as_ptr(), 1);
            if !victim.is_null() {
                (ops.notify_self_delete)(victim);
            }
            (ops.dcache_invalidate)(parent.dir, parent.name.as_ptr());
        }
        (ops.vnode_put)(victim);
        (ops.vnode_put)(parent.dir);
        r
    }
}

/// The process-root pinning `vfs_rename_flags` applies to each operand before
/// splitting it.
unsafe fn pin_to_root(ops: &VfsMutateOps, path: &mut [u8], root: &[u8]) {
    // SAFETY: path is a NUL-terminated buffer; root is NUL-terminated.
    unsafe {
        let root_len = slen(root.as_ptr());
        let under = root_len == 1 && root[0] == b'/'
            || (ops.is_beneath)(root.as_ptr(), path.as_ptr()) != 0;
        if !under {
            let mut rooted = [0u8; SCRATCH];
            let cur = slen(path.as_ptr());
            if cur == 1 && path[0] == b'/' {
                load(&mut rooted, root.as_ptr());
            } else {
                let rl = core::cmp::min(root_len, SCRATCH - 1);
                rooted[..rl].copy_from_slice(&root[..rl]);
                let take = core::cmp::min(cur, SCRATCH - 1 - rl);
                rooted[rl..rl + take].copy_from_slice(&path[..take]);
                rooted[rl + take] = 0;
            }
            let _ = (ops.normalize_with_root)(rooted.as_mut_ptr(), root.as_ptr(), ops.max_path_len);
            load(path, rooted.as_ptr());
        } else {
            let _ = (ops.normalize_with_root)(path.as_mut_ptr(), root.as_ptr(), ops.max_path_len);
        }
    }
}

/// `vfs_rename_flags`.
///
/// # Safety
/// `ops` is a live vtable; `cwd`, `root`, `old` and `newpath` are
/// NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_rename_flags(
    ops: *const VfsMutateOps,
    cwd: *const u8,
    root: *const u8,
    old: *const u8,
    newpath: *const u8,
    flags: u32,
) -> c_int {
    if ops.is_null() || old.is_null() || newpath.is_null() {
        return EINVAL;
    }
    if (flags & !(RENAME_NOREPLACE | RENAME_EXCHANGE)) != 0 {
        return EINVAL;
    }
    if (flags & RENAME_NOREPLACE) != 0 && (flags & RENAME_EXCHANGE) != 0 {
        return EINVAL;
    }
    // SAFETY: forwarded from the caller's contract.
    let ops = unsafe { &*ops };

    let mut old_resolved = [0u8; SCRATCH];
    let mut new_resolved = [0u8; SCRATCH];
    let mut root_buf = [0u8; SCRATCH];
    // SAFETY: all pointers are valid per the contract above.
    unsafe {
        load(&mut root_buf, root);
        let mut pr = (ops.join)(cwd, old, old_resolved.as_mut_ptr(), ops.max_path_len);
        if pr < 0 {
            return pr;
        }
        pr = (ops.join)(cwd, newpath, new_resolved.as_mut_ptr(), ops.max_path_len);
        if pr < 0 {
            return pr;
        }
        let root_slice = &root_buf[..slen(root_buf.as_ptr())];
        pin_to_root(ops, &mut old_resolved, root_slice);
        pin_to_root(ops, &mut new_resolved, root_slice);
        // A normalisation that overflowed leaves an unusable path.
        if (ops.normalize_with_root)(old_resolved.as_mut_ptr(), root_buf.as_ptr(), ops.max_path_len)
            < 0
            || (ops.normalize_with_root)(
                new_resolved.as_mut_ptr(),
                root_buf.as_ptr(),
                ops.max_path_len,
            ) < 0
        {
            return ENAMETOOLONG;
        }

        let mut old_parent = [0u8; SCRATCH];
        let mut old_name = [0u8; SCRATCH];
        let mut new_parent = [0u8; SCRATCH];
        let mut new_name = [0u8; SCRATCH];
        let mut sr = (ops.split_parent_name)(
            old_resolved.as_ptr(),
            old_parent.as_mut_ptr(),
            ops.max_path_len,
            old_name.as_mut_ptr(),
            ops.max_name_len,
            ops.max_path_len,
        );
        if sr < 0 {
            return sr;
        }
        sr = (ops.split_parent_name)(
            new_resolved.as_ptr(),
            new_parent.as_mut_ptr(),
            ops.max_path_len,
            new_name.as_mut_ptr(),
            ops.max_name_len,
            ops.max_path_len,
        );
        if sr < 0 {
            return sr;
        }

        let old_mnt = (ops.find_mount)(old_parent.as_ptr());
        let new_mnt = (ops.find_mount)(new_parent.as_ptr());
        if old_mnt.is_null() || new_mnt.is_null() {
            return ENOENT;
        }
        if old_mnt != new_mnt {
            return EXDEV;
        }

        let old_dir = (ops.lookup_path)(old_mnt, (ops.strip_mount_prefix)(old_parent.as_ptr(), old_mnt));
        let new_dir = (ops.lookup_path)(new_mnt, (ops.strip_mount_prefix)(new_parent.as_ptr(), new_mnt));
        if old_dir.is_null() || new_dir.is_null() {
            (ops.vnode_put)(old_dir);
            (ops.vnode_put)(new_dir);
            let e = (ops.lookup_errno)();
            return if e != 0 { e } else { ENOENT };
        }
        if (ops.vn_type)(old_dir) != VFS_FT_DIR || (ops.vn_type)(new_dir) != VFS_FT_DIR {
            (ops.vnode_put)(old_dir);
            (ops.vnode_put)(new_dir);
            return ENOTDIR;
        }
        if (ops.permission)(old_dir, W_OK | X_OK) < 0
            || (ops.permission)(new_dir, W_OK | X_OK) < 0
        {
            (ops.vnode_put)(old_dir);
            (ops.vnode_put)(new_dir);
            return EACCES;
        }
        if (ops.has_rename)(old_dir) == 0 {
            (ops.vnode_put)(old_dir);
            (ops.vnode_put)(new_dir);
            return ENOSYS;
        }

        let mut old_is_dir = false;
        if (ops.has_lookup)(old_dir) != 0 {
            let mut old_victim: *mut c_void = core::ptr::null_mut();
            if (ops.lookup)(old_dir, old_name.as_ptr(), &mut old_victim) == 0
                && !old_victim.is_null()
            {
                old_is_dir = (ops.vn_type)(old_victim) == VFS_FT_DIR;
                let sr = (ops.sticky_may_remove)(old_dir, old_victim);
                (ops.vnode_put)(old_victim);
                if sr < 0 {
                    (ops.vnode_put)(old_dir);
                    (ops.vnode_put)(new_dir);
                    return sr;
                }
            }
        }

        let mut new_victim: *mut c_void = core::ptr::null_mut();
        if (ops.has_lookup)(new_dir) != 0 {
            let mut v: *mut c_void = core::ptr::null_mut();
            if (ops.lookup)(new_dir, new_name.as_ptr(), &mut v) == 0 && !v.is_null() {
                new_victim = v;
                if (flags & RENAME_NOREPLACE) != 0 {
                    (ops.vnode_put)(new_victim);
                    (ops.vnode_put)(old_dir);
                    (ops.vnode_put)(new_dir);
                    return EEXIST;
                }
                let sr = (ops.sticky_may_remove)(new_dir, new_victim);
                if sr < 0 {
                    (ops.vnode_put)(new_victim);
                    (ops.vnode_put)(old_dir);
                    (ops.vnode_put)(new_dir);
                    return sr;
                }
            }
        }

        let r = (ops.do_rename)(
            old_dir,
            old_name.as_ptr(),
            new_dir,
            new_name.as_ptr(),
            flags,
        );
        if r == 0 {
            (ops.notify_rename)(old_dir, old_name.as_ptr(), new_dir, new_name.as_ptr());
            let old_m = (ops.vn_mnt)(old_dir);
            let reparent = !old_m.is_null()
                && (ops.mnt_type)(old_m) == FS_TYPE_RAMFS
                && (ops.vn_ino)(old_dir) != (ops.vn_ino)(new_dir)
                && (old_is_dir
                    || ((flags & RENAME_EXCHANGE) != 0
                        && !new_victim.is_null()
                        && (ops.vn_type)(new_victim) == VFS_FT_DIR));
            if reparent {
                (ops.dcache_invalidate_all)();
            } else {
                (ops.dcache_invalidate)(old_dir, old_name.as_ptr());
                (ops.dcache_invalidate)(new_dir, new_name.as_ptr());
            }
        }
        (ops.vnode_put)(new_victim);
        (ops.vnode_put)(old_dir);
        (ops.vnode_put)(new_dir);
        r
    }
}

/// `FS_TYPE_RAMFS` from `fs/vfs.h`.
pub const FS_TYPE_RAMFS: c_int = 4;

#[cfg(test)]
mod tests {
    use super::*;

    // ---- a miniature directory world ----
    const T_DIR: c_int = VFS_FT_DIR;
    const T_REG: c_int = 1;

    struct Ent {
        dir: i32,
        name: &'static str,
        child: i32,
    }

    struct Node {
        ino: u64,
        ty: c_int,
        size: usize,
        allow: bool,
        sticky: bool,
        has_lookup: bool,
        has_unlink: bool,
        has_rmdir: bool,
        has_rename: bool,
        mnt: i32,
    }

    static mut NODES: [Node; 6] = [
        Node { ino: 1, ty: T_DIR, size: 0, allow: true, sticky: false, has_lookup: true, has_unlink: true, has_rmdir: true, has_rename: true, mnt: 0 },
        Node { ino: 2, ty: T_REG, size: 100, allow: true, sticky: false, has_lookup: false, has_unlink: false, has_rmdir: false, has_rename: false, mnt: 0 },
        Node { ino: 3, ty: T_DIR, size: 0, allow: true, sticky: false, has_lookup: true, has_unlink: true, has_rmdir: true, has_rename: true, mnt: 0 },
        Node { ino: 4, ty: T_DIR, size: 0, allow: true, sticky: true, has_lookup: true, has_unlink: false, has_rmdir: false, has_rename: false, mnt: 0 },
        Node { ino: 5, ty: T_REG, size: 7, allow: true, sticky: false, has_lookup: false, has_unlink: false, has_rmdir: false, has_rename: false, mnt: 0 },
        Node { ino: 6, ty: T_DIR, size: 0, allow: false, sticky: false, has_lookup: true, has_unlink: true, has_rmdir: true, has_rename: true, mnt: 0 },
    ];
    static mut ENTS: [Ent; 4] = [
        Ent { dir: 0, name: "f", child: 1 },
        Ent { dir: 0, name: "d", child: 2 },
        Ent { dir: 0, name: "s", child: 3 },
        Ent { dir: 0, name: "g", child: 4 },
    ];
    static mut LOG: [u8; 128] = [0u8; 128];
    static mut LOGN: usize = 0;

    fn note(b: &[u8]) {
        unsafe {
            let mut n = LOGN;
            for &c in b {
                LOG[n] = c;
                n += 1;
            }
            LOGN = n;
        }
    }
    /// The log and the tree are process-global, and the test harness runs the
    /// tests on parallel threads, so serialise the tests that touch them.
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());
    fn serial() -> std::sync::MutexGuard<'static, ()> {
        TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    fn log_reset() { unsafe { LOGN = 0; } }
    fn log_str() -> String {
        unsafe { String::from_utf8(LOG[..LOGN].to_vec()).unwrap() }
    }

    fn vn(i: i32) -> *mut c_void {
        (0x1000 + i) as *mut c_void
    }
    fn vi(p: *const c_void) -> i32 {
        (p as usize - 0x1000) as i32
    }
    unsafe fn node(p: *const c_void) -> &'static Node {
        unsafe { &NODES[vi(p) as usize] }
    }
    unsafe fn node_mut(p: *mut c_void) -> &'static mut Node {
        unsafe { &mut NODES[vi(p) as usize] }
    }

    unsafe extern "C" fn t_join(cwd: *const u8, path: *const u8, out: *mut u8, _sz: usize) -> c_int {
        // Test paths are already absolute; cwd is ignored.
        let _ = cwd;
        let n = unsafe { slen(path) };
        unsafe { core::ptr::copy_nonoverlapping(path, out, n + 1) };
        0
    }
    unsafe extern "C" fn t_split(
        path: *const u8,
        parent: *mut u8,
        _psz: usize,
        name: *mut u8,
        _nsz: usize,
        _mpl: usize,
    ) -> c_int {
        let s = unsafe { core::slice::from_raw_parts(path, slen(path)) };
        match s.iter().rposition(|&c| c == b'/') {
            None => -EINVAL,
            Some(0) => {
                unsafe { *parent = b'/'; *parent.add(1) = 0 };
                let leaf = &s[1..];
                unsafe { core::ptr::copy_nonoverlapping(leaf.as_ptr(), name, leaf.len()) };
                unsafe { *name.add(leaf.len()) = 0 };
                0
            }
            Some(_) => -EINVAL, /* the test tree is one level deep */
        }
    }
    unsafe extern "C" fn t_norm(path: *mut u8, _root: *const u8, _mpl: usize) -> c_int {
        let _ = path;
        0
    }
    unsafe extern "C" fn t_beneath(_s: *const u8, _r: *const u8) -> c_int { 1 }
    unsafe extern "C" fn t_find_mount(path: *const u8) -> *mut c_void {
        let s = unsafe { core::slice::from_raw_parts(path, slen(path)) };
        if s.first() == Some(&b'/') { 0x9000 as *mut c_void } else { core::ptr::null_mut() }
    }
    unsafe extern "C" fn t_strip(path: *const u8, _m: *const c_void) -> *const u8 { path }
    unsafe extern "C" fn t_mnt_root(_m: *const c_void) -> *mut c_void { vn(0) }
    unsafe extern "C" fn t_mnt_type(_m: *const c_void) -> c_int { 1 }
    unsafe extern "C" fn t_lookup_path(mnt: *mut c_void, rel: *const u8) -> *mut c_void {
        let _ = mnt;
        let s = unsafe { core::slice::from_raw_parts(rel, slen(rel)) };
        if s == b"/" || s.is_empty() { vn(0) } else { core::ptr::null_mut() }
    }
    unsafe extern "C" fn t_errno() -> c_int { 0 }
    unsafe extern "C" fn t_put(_v: *mut c_void) {}
    unsafe extern "C" fn t_type(v: *const c_void) -> c_int { unsafe { node(v).ty } }
    unsafe extern "C" fn t_ino(v: *const c_void) -> u64 { unsafe { node(v).ino } }
    unsafe extern "C" fn t_size(v: *const c_void) -> usize { unsafe { node(v).size } }
    unsafe extern "C" fn t_mnt(_v: *const c_void) -> *mut c_void { 0x9000 as *mut c_void }
    unsafe extern "C" fn t_has_lookup(v: *const c_void) -> c_int { unsafe { node(v).has_lookup as c_int } }
    unsafe extern "C" fn t_lookup(d: *mut c_void, n: *const u8, out: *mut *mut c_void) -> c_int {
        let want = unsafe { core::slice::from_raw_parts(n, slen(n)) };
        let di = vi(d);
        unsafe {
            for e in ENTS.iter() {
                if e.dir == di && e.name.as_bytes() == want {
                    *out = vn(e.child);
                    return 0;
                }
            }
        }
        ENOENT
    }
    unsafe extern "C" fn t_has_unlink(v: *const c_void) -> c_int { unsafe { node(v).has_unlink as c_int } }
    unsafe extern "C" fn t_has_rmdir(v: *const c_void) -> c_int { unsafe { node(v).has_rmdir as c_int } }
    unsafe extern "C" fn t_has_rename(v: *const c_void) -> c_int { unsafe { node(v).has_rename as c_int } }
    unsafe extern "C" fn t_do_unlink(_d: *mut c_void, _n: *const u8) -> c_int { note(b"unlink "); 0 }
    unsafe extern "C" fn t_do_rmdir(_d: *mut c_void, _n: *const u8) -> c_int { note(b"rmdir "); 0 }
    unsafe extern "C" fn t_do_rename(_a: *mut c_void, _an: *const u8, _b: *mut c_void, _bn: *const u8, _f: u32) -> c_int { note(b"rename "); 0 }
    static mut DENY_PARENT: bool = false;
    unsafe extern "C" fn t_perm(v: *mut c_void, _m: c_int) -> c_int {
        if vi(v) == 0 && unsafe { DENY_PARENT } {
            return EACCES;
        }
        if unsafe { node(v).allow } { 0 } else { EACCES }
    }
    unsafe extern "C" fn t_sticky(d: *mut c_void, v: *mut c_void) -> c_int {
        if unsafe { node(d).sticky || node(v).sticky } { -1 } else { 0 }
    }
    unsafe extern "C" fn t_notify_delete(_d: *mut c_void, _n: *const u8, is_dir: c_int) {
        note(if is_dir != 0 { b"del-dir " } else { b"del " });
    }
    unsafe extern "C" fn t_notify_rename(_a: *mut c_void, _an: *const u8, _b: *mut c_void, _bn: *const u8) {
        note(b"notify ");
    }
    unsafe extern "C" fn t_notify_self(_v: *mut c_void) { note(b"self "); }
    unsafe extern "C" fn t_quota(v: *mut c_void, sz: usize) {
        let _ = (v, sz);
        note(b"quota ");
    }
    unsafe extern "C" fn t_dc_inv(_d: *mut c_void, _n: *const u8) { note(b"dcinv "); }
    unsafe extern "C" fn t_dc_inv_all() { note(b"dcall "); }

    fn ops() -> VfsMutateOps {
        VfsMutateOps {
            join: t_join, split_parent_name: t_split, normalize_with_root: t_norm,
            is_beneath: t_beneath, find_mount: t_find_mount, strip_mount_prefix: t_strip,
            mnt_root: t_mnt_root, mnt_type: t_mnt_type, lookup_path: t_lookup_path,
            lookup_errno: t_errno, vnode_put: t_put, vn_type: t_type, vn_ino: t_ino,
            vn_size: t_size, vn_mnt: t_mnt, has_lookup: t_has_lookup, lookup: t_lookup,
            has_unlink: t_has_unlink, has_rmdir: t_has_rmdir, has_rename: t_has_rename,
            do_unlink: t_do_unlink, do_rmdir: t_do_rmdir, do_rename: t_do_rename,
            permission: t_perm, sticky_may_remove: t_sticky, notify_delete: t_notify_delete,
            notify_rename: t_notify_rename, notify_self_delete: t_notify_self,
            quota_remove: t_quota, dcache_invalidate: t_dc_inv,
            dcache_invalidate_all: t_dc_inv_all, max_path_len: 512, max_name_len: 256,
        }
    }

    fn call(path: &str, which: u32) -> c_int {
        let mut p = [0u8; 64];
        p[..path.len()].copy_from_slice(path.as_bytes());
        let o = ops();
        log_reset();
        unsafe {
            if which == 0 {
                a20_vfs_unlink(&o, b"/\0".as_ptr(), p.as_ptr())
            } else {
                a20_vfs_rmdir(&o, b"/\0".as_ptr(), p.as_ptr())
            }
        }
    }

    fn call_rename(old: &str, new: &str, flags: u32) -> c_int {
        let mut a = [0u8; 64];
        a[..old.len()].copy_from_slice(old.as_bytes());
        let mut b = [0u8; 64];
        b[..new.len()].copy_from_slice(new.as_bytes());
        let o = ops();
        log_reset();
        unsafe {
            a20_vfs_rename_flags(&o, b"/\0".as_ptr(), b"/\0".as_ptr(), a.as_ptr(), b.as_ptr(), flags)
        }
    }

    #[test]
    fn unlink_runs_side_effects_in_order() {
        let _g = serial();
        let r = call("/f", 0);
        assert_eq!(r, 0);
        assert_eq!(log_str(), "unlink del quota self dcinv ");
    }

    #[test]
    fn rmdir_marks_the_delete_as_a_directory_and_skips_quota() {
        let _g = serial();
        let r = call("/d", 1);
        assert_eq!(r, 0);
        assert_eq!(log_str(), "rmdir del-dir self dcinv ");
    }

    #[test]
    fn refusals_map_to_the_documented_errno() {
        let _g = serial();
        // /s is sticky, so the sticky gate refuses the removal.
        assert_eq!(call("/s", 0), -1);
        // A parent that denies W_OK|X_OK yields EACCES.
        unsafe { DENY_PARENT = true };
        assert_eq!(call("/f", 0), EACCES);
        unsafe { DENY_PARENT = false };
        // A name with no entry still reaches the filesystem op.
        assert_eq!(call("/missing", 0), 0);
    }

    #[test]
    fn rename_validates_flags_before_touching_the_tree() {
        let _g = serial();
        assert_eq!(call_rename("/f", "/f", 0x40), EINVAL);            // unknown flag
        assert_eq!(call_rename("/f", "/f", RENAME_NOREPLACE | RENAME_EXCHANGE), EINVAL);
        // NOREPLACE refuses an existing destination before calling the fs.
        assert_eq!(call_rename("/f", "/f", RENAME_NOREPLACE), EEXIST);
        assert_eq!(log_str(), "");
        // A plain rename runs the operation and both invalidations.
        assert_eq!(call_rename("/f", "/f", 0), 0);
        assert_eq!(log_str(), "rename notify dcinv dcinv ");
    }
}
