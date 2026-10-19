//! VFS path-resolution walk engine (Rust-owned split for `vfs/path_resolution.c`).
//!
//! `vnode_lookup_path_openat2` used to be a 300-line loop in C that walked a
//! normalised absolute path to a vnode, and `vnode_lookup_path` / `vfs_resolve*`
//! funnel through it.  This module owns the walk itself: the component
//! iteration, the `.`/`..` handling with its depth counter and mount-root
//! crossing, the symlink decision (`RESOLVE_NO_SYMLINKS`,
//! `RESOLVE_NO_MAGICLINKS`, the `MAX_SYMLINKS` loop limit, absolute-vs-relative
//! target joining and the `RESOLVE_IN_ROOT` floor), the `RESOLVE_CACHED`
//! behaviour, the permission and directory checks in their original order, the
//! `RESOLVE_NO_XDEV`/`RESOLVE_BENEATH` boundary checks at every mount crossing,
//! the `resolved_out` bookkeeping and the errno each failure reports.
//!
//! The adapter keeps the mechanical half: `vnode_get`/`vnode_put`, the
//! filesystem `ops->lookup`/`ops->readlink` calls, the dcache probe/insert, the
//! permission check and the mount-table lookups, reached through the
//! `VfsWalkOps` vtable below.  No lock is taken, nothing allocates and nothing
//! sleeps here.
//!
//! The string helpers are the Rust-owned ones from `path_state.rs`
//! (`a20_vfs_path_*`), so the walk and the layer it consumes share one
//! implementation.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};

pub const VFS_FT_DIR: c_int = 2;
pub const VFS_FT_SYMLINK: c_int = 3;

pub const RESOLVE_NO_XDEV: u64 = 0x01;
pub const RESOLVE_NO_MAGICLINKS: u64 = 0x02;
pub const RESOLVE_NO_SYMLINKS: u64 = 0x04;
pub const RESOLVE_BENEATH: u64 = 0x08;
pub const RESOLVE_IN_ROOT: u64 = 0x10;
pub const RESOLVE_CACHED: u64 = 0x20;

pub const VNODE_MAGICLINK: u32 = 0x1;

pub const X_OK: c_int = 1;

pub const ENOENT: c_int = -2;
pub const EAGAIN: c_int = -11;
pub const EACCES: c_int = -13;
pub const EXDEV: c_int = -18;
pub const ENOTDIR: c_int = -20;
pub const EINVAL: c_int = -22;
pub const ENAMETOOLONG: c_int = -36;
pub const ENOSYS: c_int = -38;
pub const ELOOP: c_int = -40;

/// Scratch ceiling.  Every profile that builds `path_resolution.c` has
/// `MAX_PATH_LEN` at or below this (MCU 32/64, full 512); the adapter passes
/// the profile's value so the walk bounds its copies exactly as C did.
const SCRATCH: usize = 512;

/// Rust mirror of the adapter's vtable (`fs/vfs/path_walk_state.h`).
#[repr(C)]
pub struct VfsWalkOps {
    pub vnode_get: unsafe extern "C" fn(vn: *mut c_void),
    pub vnode_put: unsafe extern "C" fn(vn: *mut c_void),
    pub vn_type: unsafe extern "C" fn(vn: *const c_void) -> c_int,
    pub vn_ino: unsafe extern "C" fn(vn: *const c_void) -> u64,
    pub vn_link_flags: unsafe extern "C" fn(vn: *const c_void) -> u32,
    pub vn_parent: unsafe extern "C" fn(vn: *const c_void) -> *mut c_void,
    pub vn_mnt: unsafe extern "C" fn(vn: *const c_void) -> *mut c_void,
    /// `dir->ops && dir->ops->lookup`.
    pub has_lookup: unsafe extern "C" fn(dir: *const c_void) -> c_int,
    /// `dir->ops->lookup(dir, name, &out)`; 0 with `*out` set on success.
    pub lookup: unsafe extern "C" fn(
        dir: *mut c_void,
        name: *const u8,
        out: *mut *mut c_void,
    ) -> c_int,
    /// `vn->ops && vn->ops->readlink`.
    pub has_readlink: unsafe extern "C" fn(vn: *const c_void) -> c_int,
    /// `vn->ops->readlink(vn, buf, sz)`; byte count or negative errno.
    pub readlink: unsafe extern "C" fn(vn: *mut c_void, buf: *mut u8, sz: usize) -> c_int,
    pub permission: unsafe extern "C" fn(vn: *mut c_void, mask: c_int) -> c_int,
    /// Referenced vnode, or null.
    pub dcache_lookup: unsafe extern "C" fn(dir: *mut c_void, name: *const u8) -> *mut c_void,
    pub dcache_insert: unsafe extern "C" fn(dir: *mut c_void, name: *const u8, vn: *mut c_void),
    pub find_mount: unsafe extern "C" fn(path: *const u8) -> *mut c_void,
    pub mount_parent: unsafe extern "C" fn(mnt: *mut c_void) -> *mut c_void,
    /// Pointer into `path` (valid for the call), or `path` itself.
    pub strip_mount_prefix: unsafe extern "C" fn(path: *const u8, mnt: *const c_void) -> *const u8,
    pub mnt_root: unsafe extern "C" fn(mnt: *const c_void) -> *mut c_void,
    pub mnt_path: unsafe extern "C" fn(mnt: *const c_void) -> *const u8,
    /// `vnode_lookup_path(mnt, rel)`: referenced vnode or null.  Re-enters the
    /// walk, which is what makes the mount-crossing `..` case work.
    pub lookup_path: unsafe extern "C" fn(mnt: *mut c_void, rel: *const u8) -> *mut c_void,
    pub lookup_errno: unsafe extern "C" fn() -> c_int,
    /// The Rust-owned string layer (`a20_vfs_path_normalize_absolute_with_root`).
    pub normalize_with_root:
        unsafe extern "C" fn(path: *mut u8, root: *const u8, max_path_len: usize) -> c_int,
    /// The Rust-owned string layer (`a20_vfs_path_is_beneath`).
    pub is_beneath: unsafe extern "C" fn(start: *const u8, resolved: *const u8) -> c_int,
    pub max_path_len: usize,
    pub max_name_len: usize,
    pub max_symlinks: c_int,
}

// ---------------------------------------------------------------------------
// small NUL-terminated buffer helpers
// ---------------------------------------------------------------------------

#[inline]
fn c_len(s: &[u8]) -> usize {
    s.iter().position(|&c| c == 0).unwrap_or(s.len())
}

#[inline]
fn set(dst: &mut [u8], src: &[u8]) {
    let n = core::cmp::min(src.len(), dst.len() - 1);
    dst[..n].copy_from_slice(&src[..n]);
    dst[n] = 0;
}

#[inline]
fn eq(a: &[u8], b: &[u8]) -> bool {
    a == b
}

/// Copy a C string into a fixed buffer, bounded like the original `strncpy`.
unsafe fn load(dst: &mut [u8], src: *const u8) {
    if src.is_null() {
        dst[0] = 0;
        return;
    }
    // SAFETY: the caller guarantees a NUL-terminated source.
    let n = unsafe { slen(src) };
    let n = core::cmp::min(n, dst.len() - 1);
    // SAFETY: n bytes are readable at src and fit in dst.
    unsafe { core::ptr::copy_nonoverlapping(src, dst.as_mut_ptr(), n) };
    dst[n] = 0;
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

/// `strrchr(s, '/')` over a NUL-terminated slice.
fn rfind_slash(s: &[u8]) -> Option<usize> {
    let n = c_len(s);
    let mut i = n;
    while i > 0 {
        if s[i - 1] == b'/' {
            return Some(i - 1);
        }
        i -= 1;
    }
    None
}

/// `vfs_path_join` semantics for a caller-provided destination.
fn join_into(dst: &mut [u8], base: &[u8], path: &[u8]) {
    let mut out = [0u8; SCRATCH];
    let mut used = 0usize;
    let push = |out: &mut [u8; SCRATCH], used: &mut usize, src: &[u8]| {
        let take = core::cmp::min(src.len(), SCRATCH - 1 - *used);
        out[*used..*used + take].copy_from_slice(&src[..take]);
        *used += take;
    };
    let base = if base.is_empty() { b"/" as &[u8] } else { base };
    if eq(base, b"/") {
        push(&mut out, &mut used, b"/");
    } else if *base.last().unwrap() == b'/' {
        push(&mut out, &mut used, base);
    } else {
        push(&mut out, &mut used, base);
        push(&mut out, &mut used, b"/");
    }
    push(&mut out, &mut used, path);
    set(dst, &out[..used]);
}

fn trim_trailing_slashes(s: &mut [u8]) {
    let mut len = c_len(s);
    while len > 1 && s[len - 1] == b'/' {
        len -= 1;
        s[len] = 0;
    }
}

fn count_path_components(path: &[u8]) -> i32 {
    let mut n = 0i32;
    let s = &path[..c_len(path)];
    let mut i = 0usize;
    while i < s.len() && s[i] == b'/' {
        i += 1;
    }
    while i < s.len() {
        n += 1;
        while i < s.len() && s[i] != b'/' {
            i += 1;
        }
        while i < s.len() && s[i] == b'/' {
            i += 1;
        }
    }
    n
}

// ---------------------------------------------------------------------------
// the walk
// ---------------------------------------------------------------------------

struct Walk<'a> {
    ops: &'a VfsWalkOps,
    start: [u8; SCRATCH],
    root_path: [u8; SCRATCH],
}

impl<'a> Walk<'a> {
    #[inline]
    unsafe fn is_beneath(&self, resolved: &[u8]) -> bool {
        // SAFETY: both are NUL-terminated local buffers.
        unsafe { (self.ops.is_beneath)(self.start.as_ptr(), resolved.as_ptr()) != 0 }
    }

    #[inline]
    unsafe fn normalize(&self, path: &mut [u8], floor: &[u8]) -> bool {
        // SAFETY: `path` is a NUL-terminated buffer of max_path_len bytes.
        unsafe {
            (self.ops.normalize_with_root)(path.as_mut_ptr(), floor.as_ptr(), self.ops.max_path_len)
                >= 0
        }
    }

    /// `vnode_is_mount_root`.
    unsafe fn is_mount_root(&self, vn: *mut c_void) -> bool {
        let mnt = unsafe { (self.ops.vn_mnt)(vn) };
        if mnt.is_null() {
            return false;
        }
        let root = unsafe { (self.ops.mnt_root)(mnt) };
        if root.is_null() {
            return false;
        }
        unsafe {
            (self.ops.vn_type)(vn) == VFS_FT_DIR
                && (self.ops.vn_ino)(vn) == (self.ops.vn_ino)(root)
        }
    }
}

/// Resolve `path` to a referenced vnode, mirroring
/// `vnode_lookup_path_openat2`.
///
/// # Safety
/// `ops` points at a live vtable whose callbacks obey the documented
/// reference-transfer contract; `path`, `start` and `root_path` are
/// NUL-terminated; `resolved_out` is valid for `resolved_out_sz` bytes and
/// `lookup_err` is writable.  On success the returned vnode is referenced and
/// the caller owns that reference.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_walk_path(
    ops: *const VfsWalkOps,
    path: *const u8,
    start: *const u8,
    root_path: *const u8,
    resolve: u64,
    resolved_out: *mut u8,
    resolved_out_sz: usize,
    lookup_err: *mut c_int,
) -> *mut c_void {
    if ops.is_null()
        || path.is_null()
        || start.is_null()
        || root_path.is_null()
        || resolved_out.is_null()
        || resolved_out_sz == 0
        || lookup_err.is_null()
    {
        if !lookup_err.is_null() {
            // SAFETY: checked non-null above.
            unsafe { *lookup_err = EINVAL };
        }
        return core::ptr::null_mut();
    }
    // SAFETY: all pointers are valid per the contract above.
    let ops = unsafe { &*ops };
    // SAFETY: checked non-null above.
    unsafe { *lookup_err = 0 };

    let mut w = Walk {
        ops,
        start: [0u8; SCRATCH],
        root_path: [0u8; SCRATCH],
    };
    // SAFETY: NUL-terminated by contract.
    unsafe {
        load(&mut w.start, start);
        load(&mut w.root_path, root_path);
    }

    let out = resolved_out;
    let outsz = resolved_out_sz;
    // SAFETY: out is valid for outsz bytes.
    unsafe { load(core::slice::from_raw_parts_mut(out, outsz), path) };
    // SAFETY: out is a NUL-terminated buffer of outsz bytes.
    if !unsafe { w.normalize(core::slice::from_raw_parts_mut(out, outsz), &w.root_path) } {
        // SAFETY: lookup_err is writable.
        unsafe { *lookup_err = ENAMETOOLONG };
        return core::ptr::null_mut();
    }

    // SAFETY: all vtable calls below follow the documented contract.
    unsafe {
        let mut start_mnt = (ops.find_mount)(w.start.as_ptr());
        if start_mnt.is_null() {
            start_mnt = (ops.find_mount)(b"/\0".as_ptr());
        }
        let mnt = (ops.find_mount)(out);
        if mnt.is_null() || (ops.mnt_root)(mnt).is_null() {
            *lookup_err = ENOENT;
            return core::ptr::null_mut();
        }
        if (resolve & RESOLVE_NO_XDEV) != 0 && mnt != start_mnt {
            *lookup_err = EXDEV;
            return core::ptr::null_mut();
        }
        if (resolve & RESOLVE_BENEATH) != 0
            && !w.is_beneath(core::slice::from_raw_parts(out, c_len(core::slice::from_raw_parts(out, outsz))))
        {
            *lookup_err = EXDEV;
            return core::ptr::null_mut();
        }

        let mut cur = (ops.mnt_root)(mnt);
        (ops.vnode_get)(cur);

        if c_len(core::slice::from_raw_parts(out, outsz)) == 1
            && *out == b'/'
        {
            return cur;
        }

        let mut cur_abs_path = [0u8; SCRATCH];
        load(&mut cur_abs_path, (ops.mnt_path)(mnt));
        trim_trailing_slashes(&mut cur_abs_path);
        if cur_abs_path[0] == 0 {
            set(&mut cur_abs_path, b"/");
        }

        let mut buf = [0u8; SCRATCH];
        load(&mut buf, (ops.strip_mount_prefix)(out, mnt));

        let mut link_target = [0u8; SCRATCH];
        let mut depth = 0i32;
        let mut symlink_depth = 0i32;
        let mut p = 0usize;
        while buf[p] == b'/' {
            p += 1;
        }

        loop {
            if buf[p] == 0 {
                break;
            }
            // Split off the next component.
            let mut sep: Option<usize> = None;
            let mut i = p;
            while buf[i] != 0 {
                if buf[i] == b'/' {
                    sep = Some(i);
                    break;
                }
                i += 1;
            }
            if let Some(s) = sep {
                buf[s] = 0;
            }
            let comp_len = c_len(&buf[p..]);
            let mut comp = [0u8; SCRATCH];
            comp[..comp_len].copy_from_slice(&buf[p..p + comp_len]);
            comp[comp_len] = 0;

            if comp_len == 0 {
                // empty component: skipped
            } else if comp_len == 1 && comp[0] == b'.' {
                // ".": skipped
            } else if comp_len == 2 && comp[0] == b'.' && comp[1] == b'.' {
                let parent = (ops.vn_parent)(cur);
                if depth > 0 && !parent.is_null() && parent != cur {
                    (ops.vnode_get)(parent);
                    (ops.vnode_put)(cur);
                    cur = parent;
                    depth -= 1;
                    if let Some(s) = rfind_slash(&cur_abs_path) {
                        if s == 0 {
                            cur_abs_path[1] = 0;
                        } else {
                            cur_abs_path[s] = 0;
                        }
                    }
                } else if w.is_mount_root(cur) {
                    let cur_mnt = (ops.vn_mnt)(cur);
                    let parent_mnt = (ops.mount_parent)(cur_mnt);
                    if !parent_mnt.is_null() {
                        let mut mp_rel = [0u8; SCRATCH];
                        load(&mut mp_rel, (ops.strip_mount_prefix)((ops.mnt_path)(cur_mnt), parent_mnt));
                        let mut parent_rel = [0u8; SCRATCH];
                        let mp_rel_len = c_len(&mp_rel);
                        if mp_rel_len == 0 || (mp_rel_len == 1 && mp_rel[0] == b'/') {
                            parent_rel[0] = 0;
                        } else {
                            set(&mut parent_rel, &mp_rel[..c_len(&mp_rel)]);
                            if let Some(s) = rfind_slash(&parent_rel) {
                                if s == 0 {
                                    parent_rel[1] = 0;
                                } else {
                                    parent_rel[s] = 0;
                                }
                            } else {
                                parent_rel[0] = 0;
                            }
                        }
                        let mut new_path = [0u8; SCRATCH];
                        let mut pm_path = [0u8; SCRATCH];
                        load(&mut pm_path, (ops.mnt_path)(parent_mnt));
                        join_into(&mut new_path, &pm_path[..c_len(&pm_path)],
                                  &parent_rel[..c_len(&parent_rel)]);
                        if !w.normalize(&mut new_path, &w.root_path) {
                            (ops.vnode_put)(cur);
                            *lookup_err = ENAMETOOLONG;
                            return core::ptr::null_mut();
                        }
                        if (resolve & RESOLVE_BENEATH) != 0
                            && !w.is_beneath(&new_path[..c_len(&new_path)])
                        {
                            (ops.vnode_put)(cur);
                            *lookup_err = EXDEV;
                            return core::ptr::null_mut();
                        }
                        if (resolve & RESOLVE_NO_XDEV) != 0 && parent_mnt != start_mnt {
                            (ops.vnode_put)(cur);
                            *lookup_err = EXDEV;
                            return core::ptr::null_mut();
                        }
                        (ops.vnode_put)(cur);
                        cur = (ops.lookup_path)(parent_mnt, parent_rel.as_ptr());
                        if cur.is_null() {
                            let e = (ops.lookup_errno)();
                            *lookup_err = if e != 0 { e } else { ENOENT };
                            return core::ptr::null_mut();
                        }
                        depth = count_path_components(&parent_rel);
                        set(&mut cur_abs_path, &new_path[..c_len(&new_path)]);
                    }
                }
            } else {
                if comp_len >= ops.max_name_len {
                    (ops.vnode_put)(cur);
                    *lookup_err = ENAMETOOLONG;
                    return core::ptr::null_mut();
                }
                let cur_type = (ops.vn_type)(cur);
                if cur_type != VFS_FT_DIR || (ops.has_lookup)(cur) == 0 {
                    (ops.vnode_put)(cur);
                    *lookup_err = ENOTDIR;
                    return core::ptr::null_mut();
                }
                if (ops.permission)(cur, X_OK) < 0 {
                    (ops.vnode_put)(cur);
                    *lookup_err = EACCES;
                    return core::ptr::null_mut();
                }

                let mut next = (ops.dcache_lookup)(cur, comp.as_ptr());
                if next.is_null() {
                    if (resolve & RESOLVE_CACHED) != 0 {
                        (ops.vnode_put)(cur);
                        *lookup_err = EAGAIN;
                        return core::ptr::null_mut();
                    }
                    let mut looked: *mut c_void = core::ptr::null_mut();
                    let r = (ops.lookup)(cur, comp.as_ptr(), &mut looked);
                    if r < 0 || looked.is_null() {
                        (ops.vnode_put)(cur);
                        *lookup_err = if r < 0 { r } else { ENOENT };
                        return core::ptr::null_mut();
                    }
                    next = looked;
                    (ops.dcache_insert)(cur, comp.as_ptr(), next);
                }

                let parent = cur;
                cur = next;

                if (ops.vn_type)(cur) == VFS_FT_SYMLINK {
                    let mut follow = true;
                    if (resolve & RESOLVE_NO_SYMLINKS) != 0 {
                        follow = false;
                    }
                    if (resolve & RESOLVE_NO_MAGICLINKS) != 0
                        && ((ops.vn_link_flags)(cur) & VNODE_MAGICLINK) != 0
                    {
                        follow = false;
                    }
                    if !follow {
                        (ops.vnode_put)(parent);
                        (ops.vnode_put)(cur);
                        *lookup_err = ELOOP;
                        return core::ptr::null_mut();
                    }
                    symlink_depth += 1;
                    if symlink_depth > ops.max_symlinks {
                        (ops.vnode_put)(parent);
                        (ops.vnode_put)(cur);
                        *lookup_err = ELOOP;
                        return core::ptr::null_mut();
                    }
                    if (ops.has_readlink)(cur) == 0 {
                        (ops.vnode_put)(parent);
                        (ops.vnode_put)(cur);
                        *lookup_err = ENOSYS;
                        return core::ptr::null_mut();
                    }
                    let len = (ops.readlink)(cur, link_target.as_mut_ptr(), SCRATCH - 1);
                    if len < 0 || (len as usize) >= SCRATCH - 1 {
                        (ops.vnode_put)(parent);
                        (ops.vnode_put)(cur);
                        *lookup_err = if len < 0 { len } else { ENAMETOOLONG };
                        return core::ptr::null_mut();
                    }
                    link_target[len as usize] = 0;
                    let lt_len = len as usize;
                    if lt_len == 0 || ((resolve & RESOLVE_BENEATH) != 0 && link_target[0] == b'/') {
                        (ops.vnode_put)(parent);
                        (ops.vnode_put)(cur);
                        *lookup_err = if lt_len == 0 { ENOENT } else { EXDEV };
                        return core::ptr::null_mut();
                    }

                    let old = cur;
                    // rest = link_target ["/" remainder]
                    let mut rest = [0u8; SCRATCH];
                    {
                        let take = core::cmp::min(lt_len, SCRATCH - 1);
                        rest[..take].copy_from_slice(&link_target[..take]);
                        let mut used = take;
                        if let Some(s) = sep {
                            // remainder of the original component buffer
                            let rem_start = s + 1;
                            let rem_len = c_len(&buf[rem_start..]);
                            if used + 1 + rem_len < SCRATCH {
                                rest[used] = b'/';
                                used += 1;
                                rest[used..used + rem_len]
                                    .copy_from_slice(&buf[rem_start..rem_start + rem_len]);
                                used += rem_len;
                            }
                        }
                        rest[used] = 0;
                    }
                    let rest_len = c_len(&rest);
                    // The original refused a rest that overflowed its buffer.
                    if rest_len >= SCRATCH - 1 {
                        (ops.vnode_put)(old);
                        (ops.vnode_put)(parent);
                        *lookup_err = ENAMETOOLONG;
                        return core::ptr::null_mut();
                    }

                    let mut new_path = [0u8; SCRATCH];
                    if rest[0] == b'/' {
                        let floor: &[u8] = if (resolve & RESOLVE_IN_ROOT) != 0 {
                            &w.start
                        } else {
                            &w.root_path
                        };
                        let fl = c_len(floor);
                        if fl == 1 && floor[0] == b'/' {
                            set(&mut new_path, &rest[..rest_len]);
                        } else {
                            let mut t = [0u8; SCRATCH];
                            let mut used = 0usize;
                            used += fl;
                            t[..fl].copy_from_slice(&floor[..fl]);
                            let take = core::cmp::min(rest_len, SCRATCH - 1 - used);
                            t[used..used + take].copy_from_slice(&rest[..take]);
                            used += take;
                            t[used] = 0;
                            set(&mut new_path, &t[..used]);
                        }
                    } else {
                        join_into(
                            &mut new_path,
                            &cur_abs_path[..c_len(&cur_abs_path)],
                            &rest[..rest_len],
                        );
                    }

                    let floor: &[u8] = if (resolve & RESOLVE_IN_ROOT) != 0 {
                        &w.start
                    } else {
                        &w.root_path
                    };
                    if !w.normalize(&mut new_path, floor) {
                        (ops.vnode_put)(old);
                        (ops.vnode_put)(parent);
                        *lookup_err = ENAMETOOLONG;
                        return core::ptr::null_mut();
                    }
                    if (resolve & RESOLVE_BENEATH) != 0 && !w.is_beneath(&new_path[..c_len(&new_path)]) {
                        (ops.vnode_put)(old);
                        (ops.vnode_put)(parent);
                        *lookup_err = EXDEV;
                        return core::ptr::null_mut();
                    }
                    let new_mnt = (ops.find_mount)(new_path.as_ptr());
                    if new_mnt.is_null() || (ops.mnt_root)(new_mnt).is_null() {
                        (ops.vnode_put)(old);
                        (ops.vnode_put)(parent);
                        *lookup_err = ENOENT;
                        return core::ptr::null_mut();
                    }
                    if (resolve & RESOLVE_NO_XDEV) != 0 && new_mnt != start_mnt {
                        (ops.vnode_put)(old);
                        (ops.vnode_put)(parent);
                        *lookup_err = EXDEV;
                        return core::ptr::null_mut();
                    }
                    cur = (ops.mnt_root)(new_mnt);
                    (ops.vnode_get)(cur);
                    depth = 0;
                    load(&mut cur_abs_path, (ops.mnt_path)(new_mnt));
                    trim_trailing_slashes(&mut cur_abs_path);
                    if cur_abs_path[0] == 0 {
                        set(&mut cur_abs_path, b"/");
                    }
                    set(
                        core::slice::from_raw_parts_mut(out, outsz),
                        &new_path[..c_len(&new_path)],
                    );
                    load(&mut buf, (ops.strip_mount_prefix)(out, new_mnt));
                    (ops.vnode_put)(old);
                    (ops.vnode_put)(parent);
                    p = 0;
                    while buf[p] == b'/' {
                        p += 1;
                    }
                    continue;
                }

                if (ops.vn_type)(cur) == VFS_FT_DIR {
                    depth += 1;
                    let mut tmp = [0u8; SCRATCH];
                    join_into(&mut tmp, &cur_abs_path[..c_len(&cur_abs_path)], &comp[..comp_len]);
                    set(&mut cur_abs_path, &tmp[..c_len(&tmp)]);
                }
                (ops.vnode_put)(parent);
            }

            match sep {
                Some(s) => p = s + 1,
                None => break,
            }
        }

        cur
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use core::ffi::c_void;

    // A miniature tree + mount table, enough to exercise every branch.
    #[derive(Clone, Copy, PartialEq)]
    struct N {
        ino: u64,
        ty: c_int,
        parent: i32,
        mnt: i32,
        link: Option<&'static str>,
        magic: bool,
    }

    static ROOT: N = N { ino: 1, ty: VFS_FT_DIR, parent: -1, mnt: 0, link: None, magic: false };
    static A: N = N { ino: 2, ty: VFS_FT_DIR, parent: 0, mnt: 0, link: None, magic: false };
    static B: N = N { ino: 3, ty: VFS_FT_DIR, parent: 1, mnt: 0, link: None, magic: false };
    static F: N = N { ino: 4, ty: 1, parent: 1, mnt: 0, link: None, magic: false };
    static REL: N = N { ino: 5, ty: VFS_FT_SYMLINK, parent: 0, mnt: 0, link: Some("a/f"), magic: false };
    static ABS: N = N { ino: 6, ty: VFS_FT_SYMLINK, parent: 0, mnt: 0, link: Some("/a/b"), magic: false };
    static LOOP: N = N { ino: 7, ty: VFS_FT_SYMLINK, parent: 0, mnt: 0, link: Some("loop"), magic: false };
    static MAGIC: N = N { ino: 8, ty: VFS_FT_SYMLINK, parent: 0, mnt: 0, link: Some("a/f"), magic: true };
    static SUB: N = N { ino: 1, ty: VFS_FT_DIR, parent: -1, mnt: 1, link: None, magic: false };

    static ALL: [(&N, &str); 8] = [
        (&ROOT, ""), (&A, "a"), (&B, "b"), (&F, "f"),
        (&REL, "rel"), (&ABS, "abs"), (&LOOP, "loop"), (&MAGIC, "magic"),
    ];

    fn all() -> &'static [(&'static N, &'static str)] {
        &ALL
    }

    fn as_vn(n: &'static N) -> *mut c_void {
        n as *const N as *mut c_void
    }

    unsafe extern "C" fn t_get(_vn: *mut c_void) {}
    unsafe extern "C" fn t_put(_vn: *mut c_void) {}
    unsafe extern "C" fn t_type(vn: *const c_void) -> c_int { unsafe { (*(vn as *const N)).ty } }
    unsafe extern "C" fn t_ino(vn: *const c_void) -> u64 { unsafe { (*(vn as *const N)).ino } }
    unsafe extern "C" fn t_link(vn: *const c_void) -> u32 {
        if unsafe { (*(vn as *const N)).magic } { VNODE_MAGICLINK } else { 0 }
    }
    unsafe extern "C" fn t_parent(vn: *const c_void) -> *mut c_void {
        let i = unsafe { (*(vn as *const N)).parent };
        if i < 0 { core::ptr::null_mut() } else { as_vn(all()[i as usize].0) }
    }
    unsafe extern "C" fn t_mnt(vn: *const c_void) -> *mut c_void {
        let m = unsafe { (*(vn as *const N)).mnt };
        if m == 0 { 0x1000 as *mut c_void } else { 0x2000 as *mut c_void }
    }
    unsafe extern "C" fn t_lookup(dir: *mut c_void, name: *const u8, out: *mut *mut c_void) -> c_int {
        let want = unsafe { core::slice::from_raw_parts(name, slen(name)) };
        let dn = unsafe { (*(dir as *const N)).ino };
        for (n, nm) in all() {
            if n.parent >= 0 && all()[n.parent as usize].0.ino == dn && !nm.is_empty() && eq(nm.as_bytes(), want) {
                unsafe { *out = as_vn(n) };
                return 0;
            }
        }
        -2
    }
    unsafe extern "C" fn t_readlink(vn: *mut c_void, buf: *mut u8, sz: usize) -> c_int {
        match unsafe { (*(vn as *const N)).link } {
            Some(t) => {
                let b = t.as_bytes();
                if b.len() >= sz { return -36; }
                unsafe { core::ptr::copy_nonoverlapping(b.as_ptr(), buf, b.len()) };
                b.len() as c_int
            }
            None => -22,
        }
    }
    unsafe extern "C" fn t_perm(_vn: *mut c_void, _mask: c_int) -> c_int { 0 }
    unsafe extern "C" fn t_has_lookup(_d: *const c_void) -> c_int { 1 }
    unsafe extern "C" fn t_has_readlink(vn: *const c_void) -> c_int {
        (unsafe { (*(vn as *const N)).link }.is_some()) as c_int
    }
    unsafe extern "C" fn t_dc_lookup(_d: *mut c_void, _n: *const u8) -> *mut c_void { core::ptr::null_mut() }
    unsafe extern "C" fn t_dc_insert(_d: *mut c_void, _n: *const u8, _vn: *mut c_void) {}
    unsafe extern "C" fn t_find_mount(path: *const u8) -> *mut c_void {
        let s = unsafe { core::slice::from_raw_parts(path, slen(path)) };
        if s.starts_with(b"/sub") { 0x2000 as *mut c_void } else { 0x1000 as *mut c_void }
    }
    unsafe extern "C" fn t_mount_parent(mnt: *mut c_void) -> *mut c_void {
        if mnt as usize == 0x2000 { 0x1000 as *mut c_void } else { core::ptr::null_mut() }
    }
    unsafe extern "C" fn t_strip(path: *const u8, mnt: *const c_void) -> *const u8 {
        if mnt as usize == 0x2000 {
            let s = unsafe { core::slice::from_raw_parts(path, slen(path)) };
            if s.starts_with(b"/sub") {
                let rest = &s[4..];
                let rest = if rest.first() == Some(&b'/') { &rest[1..] } else { rest };
                // point into a static so the pointer stays valid
                static NUL: u8 = 0;
                let _ = NUL;
                unsafe { core::ptr::copy_nonoverlapping(rest.as_ptr(), STRIP_BUF.as_ptr() as *mut u8, rest.len()) };
                unsafe { STRIP_BUF.as_mut_ptr().add(rest.len()).write(0) };
                return STRIP_BUF.as_ptr();
            }
        }
        path
    }
    static mut STRIP_BUF: [u8; 64] = [0u8; 64];
    unsafe extern "C" fn t_mnt_root(mnt: *const c_void) -> *mut c_void {
        if mnt as usize == 0x2000 { as_vn(&SUB) } else { as_vn(&ROOT) }
    }
    unsafe extern "C" fn t_mnt_path(mnt: *const c_void) -> *const u8 {
        if mnt as usize == 0x2000 { b"/sub\0".as_ptr() } else { b"/\0".as_ptr() }
    }
    unsafe extern "C" fn t_lookup_path(mnt: *mut c_void, rel: *const u8) -> *mut c_void {
        // Resolve `rel` against the mount root by hand (the walk re-enters here).
        let s = unsafe { core::slice::from_raw_parts(rel, slen(rel)) };
        let mut cur = if mnt as usize == 0x2000 { &SUB } else { &ROOT };
        for part in s.split(|&c| c == b'/') {
            if part.is_empty() { continue; }
            let mut found: Option<&'static N> = None;
            for (n, nm) in all() {
                if nm.as_bytes() == part && n.parent >= 0 && all()[n.parent as usize].0.ino == cur.ino {
                    found = Some(n);
                }
            }
            match found { Some(n) => cur = n, None => return core::ptr::null_mut() }
        }
        as_vn(cur)
    }
    unsafe extern "C" fn t_errno() -> c_int { 0 }

    // The string layer the production adapter points at path.c (which is
    // Rust-owned); the unit test supplies the same algorithm directly.
    unsafe extern "C" fn t_norm(path: *mut u8, root: *const u8, _max: usize) -> c_int {
        let src = unsafe { core::slice::from_raw_parts(path, slen(path)) };
        if src.first() != Some(&b'/') {
            return -22;
        }
        let r = unsafe { core::slice::from_raw_parts(root, slen(root)) };
        let mut root_len = r.len();
        while root_len > 1 && r[root_len - 1] == b'/' {
            root_len -= 1;
        }
        let mut parts: [&[u8]; 64] = [b""; 64];
        let mut n = 0usize;
        for tok in src[1..].split(|&c| c == b'/') {
            if tok.is_empty() || tok == b"." {
                continue;
            }
            if tok == b".." {
                let cur_len: usize = parts[..n].iter().map(|q| q.len() + 1).sum();
                if n > 0 && cur_len > root_len {
                    n -= 1;
                }
                continue;
            }
            if n < 64 {
                parts[n] = tok;
                n += 1;
            } else {
                return -36;
            }
        }
        let mut out = [0u8; 512];
        let mut used = 1usize;
        out[0] = b'/';
        for i in 0..n {
            let sep = if i + 1 < n { 1 } else { 0 };
            if used + parts[i].len() + sep >= 512 {
                return -36;
            }
            out[used..used + parts[i].len()].copy_from_slice(parts[i]);
            used += parts[i].len();
            if sep == 1 {
                out[used] = b'/';
                used += 1;
            }
        }
        unsafe {
            core::ptr::copy_nonoverlapping(out.as_ptr(), path, used);
            *path.add(used) = 0;
        }
        0
    }

    unsafe extern "C" fn t_beneath(start: *const u8, resolved: *const u8) -> c_int {
        let s = unsafe { core::slice::from_raw_parts(start, slen(start)) };
        let r = unsafe { core::slice::from_raw_parts(resolved, slen(resolved)) };
        let mut len = s.len();
        while len > 1 && s[len - 1] == b'/' {
            len -= 1;
        }
        if len == 1 && s[0] == b'/' {
            return (r.first() == Some(&b'/')) as c_int;
        }
        if r == s {
            return 1;
        }
        if r.len() >= len && &r[..len] == &s[..len] {
            let c = r.get(len).copied().unwrap_or(0);
            if c == b'/' || c == 0 {
                return 1;
            }
        }
        0
    }

    fn ops() -> VfsWalkOps {
        VfsWalkOps {
            vnode_get: t_get, vnode_put: t_put, vn_type: t_type, vn_ino: t_ino,
            vn_link_flags: t_link, vn_parent: t_parent, vn_mnt: t_mnt,
            has_lookup: t_has_lookup, lookup: t_lookup,
            has_readlink: t_has_readlink, readlink: t_readlink,
            permission: t_perm, dcache_lookup: t_dc_lookup,
            dcache_insert: t_dc_insert, find_mount: t_find_mount, mount_parent: t_mount_parent,
            strip_mount_prefix: t_strip, mnt_root: t_mnt_root, mnt_path: t_mnt_path,
            lookup_path: t_lookup_path, lookup_errno: t_errno,
            normalize_with_root: t_norm, is_beneath: t_beneath,
            max_path_len: 512, max_name_len: 256, max_symlinks: 40,
        }
    }

    fn walk(path: &str, resolve: u64) -> (i64, i32, String) {
        let mut pb = [0u8; 128];
        pb[..path.len()].copy_from_slice(path.as_bytes());
        let mut out = [0u8; 512];
        let mut err: c_int = 0;
        let o = ops();
        let vn = unsafe {
            a20_vfs_walk_path(&o, pb.as_ptr(), b"/\0".as_ptr(), b"/\0".as_ptr(), resolve,
                              out.as_mut_ptr(), 512, &mut err)
        };
        let ino = if vn.is_null() { -1 } else { (unsafe { (*(vn as *const N)).ino }) as i64 };
        let n = c_len(&out);
        (ino, err, String::from_utf8(out[..n].to_vec()).unwrap())
    }

    #[test]
    fn resolves_plain_and_dotted_paths() {
        assert_eq!(walk("/", 0), (1, 0, "/".into()));
        assert_eq!(walk("/a", 0), (2, 0, "/a".into()));
        assert_eq!(walk("/a/b", 0), (3, 0, "/a/b".into()));
        assert_eq!(walk("/a/./b/", 0), (3, 0, "/a/b".into()));
        assert_eq!(walk("/a/../a", 0), (2, 0, "/a".into()));
        assert_eq!(walk("/a/..", 0), (1, 0, "/".into()));
        assert_eq!(walk("/nope", 0), (-1, ENOENT, "/nope".into()));
        assert_eq!(walk("/a/f/x", 0), (-1, ENOTDIR, "/a/f/x".into()));
    }

    #[test]
    fn follows_relative_and_absolute_symlinks() {
        assert_eq!(walk("/rel", 0), (4, 0, "/a/f".into()));
        assert_eq!(walk("/abs", 0), (3, 0, "/a/b".into()));
        // RESOLVE_NO_SYMLINKS refuses the final symlink.
        assert_eq!(walk("/rel", RESOLVE_NO_SYMLINKS), (-1, ELOOP, "/rel".into()));
        // A magic link is refused only with RESOLVE_NO_MAGICLINKS.
        assert_eq!(walk("/magic", 0), (4, 0, "/a/f".into()));
        assert_eq!(walk("/magic", RESOLVE_NO_MAGICLINKS), (-1, ELOOP, "/magic".into()));
        // A symlink loop trips MAX_SYMLINKS.
        assert_eq!(walk("/loop", 0), (-1, ELOOP, "/loop".into()));
    }

    #[test]
    fn resolves_through_a_second_mount() {
        // "/sub" is a second mount; the walk selects it and its own root.
        assert_eq!(walk("/sub", 0), (1, 0, "/sub".into()));
        // ".." never reaches the walk: the string layer collapses it first,
        // floored at the process root, so the mount-crossing branch in the
        // component loop stays defensive.
        assert_eq!(walk("/sub/..", 0), (1, 0, "/".into()));
        assert_eq!(walk("/sub/../a", 0), (2, 0, "/a".into()));
        assert_eq!(walk("/sub/nope", 0), (-1, ENOENT, "/sub/nope".into()));
    }

    #[test]
    fn resolve_cached_reports_eagain_when_the_dcache_misses() {
        assert_eq!(walk("/a/b", RESOLVE_CACHED), (-1, EAGAIN, "/a/b".into()));
    }

    #[test]
    fn empty_component_run_is_skipped() {
        assert_eq!(walk("//a///b//", 0), (3, 0, "/a/b".into()));
    }
}
