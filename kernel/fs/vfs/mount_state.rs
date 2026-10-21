//! VFS mount-table orchestration state engine (Rust-owned split for
//! `fs/vfs/mount_ops.c` and `fs/vfs/mount.c`).
//!
//! Two operations on the mount table used to run their validation and
//! matching inline in C.  This module owns both:
//!
//! * **umount(2)** (`a20_vfs_umount_plan`): normalise the query path (the
//!   trailing-slash trim), find the mount whose normalised path matches, and
//!   apply the busy rules in their original order -- a process rooted there
//!   (`-EBUSY`), a mount below it (`-EBUSY`), a process standing in it
//!   (`-EBUSY`), and the namespace root itself (`-EINVAL`) -- unless
//!   `MNT_DETACH` waives them.  The plan also says whether the mount's
//!   children must be detached and whether the dentry cache is dropped.
//! * **move_mount(2)** (`a20_vfs_move_mount_check`): the argument, detached
//!   flag, target-covered, same-path and self-nesting checks in their original
//!   order, before the adapter re-parents the mount.
//!
//! The adapter keeps the mount table, the per-mount counters, the filesystem
//! teardown, the re-parenting and the dcache/namespace side effects; it
//! supplies the per-mount facts this engine decides on.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};

pub const ENOENT: c_int = -2;
pub const EBUSY: c_int = -16;
pub const EINVAL: c_int = -22;

/// `MNT_*` bits carried by `umount2(2)`.
pub const VFS_UMOUNT_FORCE: i32 = 0x1;
pub const VFS_UMOUNT_DETACH: i32 = 0x2;
pub const VFS_UMOUNT_EXPIRE: i32 = 0x4;

const SCRATCH: usize = 512;

/// Rust mirror of `vfs_mount_cand_t`: one mount-table entry's facts.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VfsMountCand {
    pub path: *const u8,
    pub root_users: u32,
    pub has_child: u32,
    pub cwd_users: u32,
    pub is_ns_root: u32,
}

/// Rust mirror of `vfs_umount_plan_t`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VfsUmountPlan {
    /// Index into the candidate array, or -1.
    pub index: i32,
    pub detach_children: u32,
    pub invalidate_all: u32,
}

/// Borrow a NUL-terminated byte string.
#[inline]
unsafe fn cbytes<'a>(p: *const u8) -> &'a [u8] {
    let n = unsafe { slen(p) };
    // SAFETY: n bytes are readable at p.
    unsafe { core::slice::from_raw_parts(p, n) }
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

/// Copy a path and trim trailing slashes, keeping a lone "/".
unsafe fn norm(dst: &mut [u8], src: *const u8) -> usize {
    // SAFETY: src is NUL-terminated by contract.
    let n = unsafe { slen(src) };
    let n = core::cmp::min(n, dst.len() - 1);
    // SAFETY: n bytes readable at src, fit in dst.
    unsafe { core::ptr::copy_nonoverlapping(src, dst.as_mut_ptr(), n) };
    dst[n] = 0;
    let mut len = n;
    while len > 1 && dst[len - 1] == b'/' {
        len -= 1;
        dst[len] = 0;
    }
    len
}

/// Decide the umount(2) for `query`: which mount matches and whether the
/// operation is admitted.
///
/// `check_busy` is 0 for the legacy `vfs_umount()` path, which skips the busy
/// rules entirely.  Returns 0 with `out` filled, or the errno to report
/// (`-EINVAL` when no mount matches).
///
/// # Safety
/// `query` is NUL-terminated; `cands` points at `n` descriptors whose paths are
/// NUL-terminated; `out` is writable.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_umount_plan(
    query: *const u8,
    flags: c_int,
    check_busy: c_int,
    cands: *const VfsMountCand,
    n: usize,
    out: *mut VfsUmountPlan,
) -> c_int {
    if query.is_null() {
        return EINVAL;
    }
    if !out.is_null() {
        // SAFETY: writable by contract.
        unsafe {
            (*out).index = -1;
            (*out).detach_children = 0;
            (*out).invalidate_all = 0;
        }
    }
    if cands.is_null() {
        return EINVAL;
    }
    let mut want = [0u8; SCRATCH];
    // SAFETY: query is NUL-terminated.
    let want_len = unsafe { norm(&mut want, query) };

    for i in 0..n {
        // SAFETY: i < n and cands points at n descriptors.
        let c = unsafe { &*cands.add(i) };
        if c.path.is_null() {
            continue;
        }
        let mut have = [0u8; SCRATCH];
        // SAFETY: c.path is NUL-terminated by contract.
        let have_len = unsafe { norm(&mut have, c.path) };
        if have_len != want_len {
            continue;
        }
        let mut same = true;
        for k in 0..want_len {
            if have[k] != want[k] {
                same = false;
                break;
            }
        }
        if !same {
            continue;
        }

        if check_busy != 0 && (flags & VFS_UMOUNT_DETACH) == 0 {
            // The order matters: root pin, child mount, cwd, namespace root.
            if c.root_users > 1 {
                return EBUSY;
            }
            if c.has_child != 0 {
                return EBUSY;
            }
            if c.cwd_users > 0 {
                return EBUSY;
            }
            if c.is_ns_root != 0 {
                return EINVAL;
            }
        }
        if !out.is_null() {
            // SAFETY: writable by contract.
            unsafe {
                (*out).index = i as i32;
                (*out).detach_children = c.has_child;
                (*out).invalidate_all = 1;
            }
        }
        return 0;
    }
    EINVAL
}

/// The `move_mount(2)` validation sequence.
///
/// Returns 0 when the move may proceed (with `*out_noop` set when the source
/// already is the destination, which the caller must treat as success without
/// re-parenting), or the errno to report.
///
/// # Safety
/// `from`/`to` are NUL-terminated; `from_path` is the resolved source mount's
/// path (NUL-terminated) and `out_noop` is writable.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_move_mount_check(
    from: *const u8,
    to: *const u8,
    found: c_int,
    from_path: *const u8,
    from_detached: c_int,
    target_covered: c_int,
    out_noop: *mut c_int,
) -> c_int {
    if !out_noop.is_null() {
        // SAFETY: writable by contract.
        unsafe { *out_noop = 0 };
    }
    if from.is_null() || to.is_null() {
        return EINVAL;
    }
    // SAFETY: both are NUL-terminated.
    unsafe {
        if *from != b'/' || *to != b'/' {
            return EINVAL;
        }
        if found == 0 {
            return ENOENT;
        }
        if from_detached != 0 {
            return EINVAL;
        }
        // A destination already covered by another mount would silently merge
        // two filesystems into one path.
        if target_covered != 0 {
            return EBUSY;
        }
        if from_path.is_null() {
            return EINVAL;
        }
        let src_len = slen(from_path);
        let dst_len = slen(to);
        if src_len == dst_len {
            let mut equal = true;
            for i in 0..src_len {
                if *from_path.add(i) != *to.add(i) {
                    equal = false;
                    break;
                }
            }
            if equal {
                // Unreachable in practice (the source mount covers `to`), kept
                // because the pre-migration code returned success here.
                if !out_noop.is_null() {
                    *out_noop = 1;
                }
                return 0;
            }
        }
        // Refuse to move a mount underneath itself.
        if dst_len >= src_len {
            let mut prefix = true;
            for i in 0..src_len {
                if *to.add(i) != *from_path.add(i) {
                    prefix = false;
                    break;
                }
            }
            if prefix {
                let c = *to.add(src_len);
                if c == 0 || c == b'/' {
                    return EINVAL;
                }
            }
        }
        0
    }
}

/// Rust mirror of `vfs_mount_match_t`: one visible-mount snapshot entry.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct VfsMountMatch {
    pub path: *const u8,
    pub ready: u32,
    pub teardown: u32,
    pub detached: u32,
}

/// Longest-prefix visible-mount match over a snapshot.  This is the mount
/// tree's *propagation/visibility* decision: which mount a path resolves
/// into, honouring `publish_ready`, teardown, the detached-root rule and the
/// external-root equal-length-root tie.  Returns the best index, or -1.
///
/// # Safety
/// `query` is NUL-terminated; `cands` points at `n` entries whose paths are
/// NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_find_mount(
    query: *const u8,
    include_detached: c_int,
    external_root: c_int,
    cands: *const VfsMountMatch,
    n: usize,
) -> c_int {
    if query.is_null() || (cands.is_null() && n > 0) {
        return -1;
    }
    // SAFETY: NUL-terminated by contract.
    let q = unsafe { cbytes(query) };
    let mut best: c_int = -1;
    let mut best_len = 0usize;
    for i in 0..n {
        // SAFETY: i < n and cands has n entries.
        let c = unsafe { &*cands.add(i) };
        if c.ready == 0 || c.teardown != 0 {
            continue;
        }
        if c.detached != 0 && include_detached == 0 {
            continue;
        }
        if c.path.is_null() {
            continue;
        }
        // SAFETY: NUL-terminated by contract.
        let p = unsafe { cbytes(c.path) };
        let len = p.len();
        if q.len() < len || &q[..len] != p {
            continue;
        }
        /* A path boundary: the whole path, or the next byte is a separator. */
        if !(len == 1 || q.len() == len || q[len] == b'/') {
            continue;
        }
        if len > best_len || (external_root != 0 && len == 1 && best_len == 1) {
            best = i as c_int;
            best_len = len;
        }
    }
    best
}

/// Offset to add to `path` to strip the matched `mnt_path` prefix (0 when it
/// does not match, exactly what `vfs_strip_mount_prefix` returns).
///
/// # Safety
/// Both arguments are NUL-terminated.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_strip_prefix(path: *const u8, mnt_path: *const u8) -> usize {
    if path.is_null() || mnt_path.is_null() {
        return 0;
    }
    // SAFETY: NUL-terminated by contract.
    let q = unsafe { cbytes(path) };
    let p = unsafe { cbytes(mnt_path) };
    let len = p.len();
    if q.len() < len || &q[..len] != p {
        return 0;
    }
    let mut off = len;
    if off < q.len() && q[off] == b'/' {
        off += 1;
    }
    off
}

/// Compute the parent path of a mount path (the `vfs_mount_parent` prefix
/// arithmetic), writing it into `out`.  Returns 1 when a parent path was
/// produced, 0 for the namespace root / no separator / no room.
///
/// # Safety
/// `path` is NUL-terminated; `out` has `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_parent_path(
    path: *const u8,
    out: *mut u8,
    cap: usize,
) -> c_int {
    if path.is_null() || out.is_null() || cap == 0 {
        return 0;
    }
    // SAFETY: NUL-terminated by contract.
    let q = unsafe { cbytes(path) };
    if q == b"/" {
        return 0;
    }
    let n = core::cmp::min(q.len(), cap - 1);
    // SAFETY: n bytes readable at q and writable at out.
    unsafe { core::ptr::copy_nonoverlapping(q.as_ptr(), out, n) };
    // SAFETY: out[n] is within cap.
    unsafe { *out.add(n) = 0; }
    let mut len = n;
    while len > 1 && unsafe { *out.add(len - 1) } == b'/' {
        len -= 1;
        unsafe { *out.add(len) = 0; }
    }
    let mut slash: isize = -1;
    for i in 0..len {
        if unsafe { *out.add(i) } == b'/' {
            slash = i as isize;
        }
    }
    if slash < 0 {
        return 0;
    }
    if slash == 0 {
        // SAFETY: out has at least 2 bytes (cap >= 1 and a slash at 0).
        unsafe { *out.add(1) = 0; }
    } else {
        unsafe { *out.add(slash as usize) = 0; }
    }
    1
}

/// Narrow mechanical bridge for the mount-tree side effects.  Field order
/// mirrors `struct vfs_mount_tree_ops` in `fs/vfs/mount_state.h`.
#[repr(C)]
pub struct VfsMountTreeOps {
    pub parent: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub child: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub sibling: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub mp: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub set_parent: Option<extern "C" fn(*mut c_void, *mut c_void)>,
    pub set_child: Option<extern "C" fn(*mut c_void, *mut c_void)>,
    pub set_sibling: Option<extern "C" fn(*mut c_void, *mut c_void)>,
    pub set_mp: Option<extern "C" fn(*mut c_void, *mut c_void)>,
    pub set_detached: Option<extern "C" fn(*mut c_void)>,
    pub vnode_get: Option<extern "C" fn(*mut c_void)>,
    pub vnode_put: Option<extern "C" fn(*mut c_void)>,
}

/// Bridge for the mount-path-to-tree publication transaction.  The Rust
/// engine decides the parent-path derivation and the attach ordering; C only
/// resolves paths and exposes the existing mount/vnode layout.
#[repr(C)]
pub struct VfsMountLinkOps {
    pub path: Option<extern "C" fn(*mut c_void) -> *const u8>,
    pub find_mount: Option<extern "C" fn(*const u8) -> *mut c_void>,
    pub resolve: Option<extern "C" fn(*const u8) -> *mut c_void>,
    pub vnode_mount: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub mark_ready: Option<extern "C" fn(*mut c_void)>,
}

struct LinkOps<'a> {
    path: &'a extern "C" fn(*mut c_void) -> *const u8,
    find_mount: &'a extern "C" fn(*const u8) -> *mut c_void,
    resolve: &'a extern "C" fn(*const u8) -> *mut c_void,
    vnode_mount: &'a extern "C" fn(*mut c_void) -> *mut c_void,
    mark_ready: &'a extern "C" fn(*mut c_void),
}

impl<'a> LinkOps<'a> {
    unsafe fn load(t: *const VfsMountLinkOps) -> Option<Self> {
        if t.is_null() { return None; }
        // SAFETY: caller guarantees the callback table lives for this call.
        let t = unsafe { &*t };
        Some(Self {
            path: t.path.as_ref()?,
            find_mount: t.find_mount.as_ref()?,
            resolve: t.resolve.as_ref()?,
            vnode_mount: t.vnode_mount.as_ref()?,
            mark_ready: t.mark_ready.as_ref()?,
        })
    }
}

/// Publish a mount's tree position after its filesystem root and path have
/// been initialized.  A synthetic or not-yet-resolvable mountpoint remains
/// an orphan in the flattened path table, matching the old VFS contract.
///
/// # Safety
/// The callback tables are live; `mnt` is an initialized mount and all paths
/// and vnode references returned by callbacks remain valid for this call.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_link_tree(
    tree: *const VfsMountTreeOps,
    ops: *const VfsMountLinkOps,
    mnt: *mut c_void,
) {
    let t = match unsafe { Mt::load(tree) } { Some(v) => v, None => return };
    let o = match unsafe { LinkOps::load(ops) } { Some(v) => v, None => return };
    if mnt.is_null() { return; }
    if !(t.parent)(mnt).is_null() || !(t.child)(mnt).is_null() {
        (o.mark_ready)(mnt);
        return;
    }
    let path = (o.path)(mnt);
    if path.is_null() { (o.mark_ready)(mnt); return; }
    let path_len = unsafe { slen(path) };
    let mut trimmed = path_len;
    while trimmed > 1 && unsafe { *path.add(trimmed - 1) } == b'/' { trimmed -= 1; }
    if trimmed <= 1 { (o.mark_ready)(mnt); return; }
    if trimmed >= SCRATCH { (o.mark_ready)(mnt); return; }

    let mut parent_path = [0u8; SCRATCH];
    unsafe { core::ptr::copy_nonoverlapping(path, parent_path.as_mut_ptr(), trimmed); }
    parent_path[trimmed] = 0;
    let mut slash = None;
    for i in 0..trimmed { if parent_path[i] == b'/' { slash = Some(i); } }
    let parent_end = match slash { Some(v) => v, None => { (o.mark_ready)(mnt); return; } };
    if parent_end == 0 { parent_path[1] = 0; } else { parent_path[parent_end] = 0; }

    let parent = (o.find_mount)(parent_path.as_ptr());
    if parent.is_null() { (o.mark_ready)(mnt); return; }

    // Resolve the mountpoint in the currently visible parent.  The returned
    // vnode reference is temporary; attach takes its own reference.
    let mp = (o.resolve)(path);
    if !mp.is_null() {
        if (o.vnode_mount)(mp) == parent {
            let _ = unsafe { a20_vfs_mount_attach(tree, mnt, parent, mp) };
            (t.vnode_put)(mp);
        } else {
            (t.vnode_put)(mp);
            let _ = unsafe { a20_vfs_mount_attach(tree, mnt, parent, core::ptr::null_mut()) };
        }
    } else {
        let _ = unsafe { a20_vfs_mount_attach(tree, mnt, parent, core::ptr::null_mut()) };
    }
    (o.mark_ready)(mnt);
}

/// Commit a validated move_mount operation. Rust owns the atomic publication
/// order: recheck namespace membership/teardown under the mount-table lock,
/// update the visible path, then transfer the tree and mountpoint reference.
#[repr(C)]
pub struct VfsMoveMountOps {
    pub lock: Option<extern "C" fn() -> u64>,
    pub unlock: Option<extern "C" fn(u64)>,
    pub is_present: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub is_teardown: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub set_path: Option<extern "C" fn(*mut c_void, *const u8)>,
    pub invalidate_dcache: Option<extern "C" fn()>,
}

#[repr(C)]
pub struct VfsUmountExecuteOps {
    pub teardown: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub invalidate_all: Option<extern "C" fn()>,
    pub drop_time_meta: Option<extern "C" fn(*mut c_void)>,
    pub remove: Option<extern "C" fn(*mut c_void)>,
    pub retry_pending: Option<extern "C" fn()>,
}

#[repr(C)]
pub struct VfsMountNamespaceTeardownOps {
    pub mount_count: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub mount_at: Option<extern "C" fn(*mut c_void, c_int) -> *mut c_void>,
    pub dead_head: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub dead_next: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub shared_source: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub fs_teardown: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub decrement_source_users: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub restore_source_users: Option<extern "C" fn(*mut c_void)>,
    pub clear_shared_source: Option<extern "C" fn(*mut c_void)>,
}

#[repr(C)]
pub struct VfsMountRemoveOps {
    pub lock: Option<extern "C" fn() -> u64>,
    pub unlock: Option<extern "C" fn(u64)>,
    pub count: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub set_count: Option<extern "C" fn(*mut c_void, c_int)>,
    pub current_ns: Option<extern "C" fn() -> *mut c_void>,
    pub at: Option<extern "C" fn(*mut c_void, c_int) -> *mut c_void>,
    pub set_at: Option<extern "C" fn(*mut c_void, c_int, *mut c_void)>,
    pub dead_head: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub set_dead_head: Option<extern "C" fn(*mut c_void, *mut c_void)>,
    pub set_dead_next: Option<extern "C" fn(*mut c_void, *mut c_void)>,
    pub set_attached: Option<extern "C" fn(*mut c_void, c_int)>,
    pub clear_teardown: Option<extern "C" fn(*mut c_void)>,
    pub mountpoint: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub clear_mountpoint: Option<extern "C" fn(*mut c_void)>,
    pub vnode_put: Option<extern "C" fn(*mut c_void)>,
    pub root_put: Option<extern "C" fn(*mut c_void)>,
}

#[repr(C)]
pub struct VfsMountFsTeardownOps {
    pub lock: Option<extern "C" fn() -> u64>,
    pub unlock: Option<extern "C" fn(u64)>,
    pub shared_source: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub users: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub is_running: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub set_running: Option<extern "C" fn(*mut c_void, c_int)>,
    pub kind: Option<extern "C" fn(*mut c_void) -> c_int>,
    pub root: Option<extern "C" fn(*mut c_void) -> *mut c_void>,
    pub unmount_backend: Option<extern "C" fn(c_int, *mut c_void) -> c_int>,
    pub set_users_zero: Option<extern "C" fn(*mut c_void)>,
    pub trace: Option<extern "C" fn(*mut c_void, c_int, c_int)>,
}

#[repr(C)]
pub struct VfsMountDispatchOps {
    pub update_existing: Option<extern "C" fn(*const u8, c_int) -> c_int>,
    pub mount_cgroup: Option<extern "C" fn(*const u8, *const u8, *const u8, c_int, *const u8) -> c_int>,
    pub mount_virtual: Option<extern "C" fn(*const u8, *const u8, *const u8, c_int, *const u8) -> c_int>,
    pub mount_ram: Option<extern "C" fn(*const u8, *const u8, *const u8, c_int, *const u8) -> c_int>,
    pub mount_block: Option<extern "C" fn(c_int, c_int, *const u8, *const u8, c_int) -> c_int>,
}

unsafe fn cstr_eq(a: *const u8, b: &[u8]) -> bool {
    if a.is_null() { return false; }
    for (i, expected) in b.iter().enumerate() {
        if unsafe { *a.add(i) } != *expected { return false; }
    }
    unsafe { *a.add(b.len()) == 0 }
}

/// Select the VFS mount backend and preserve the mount(2) route ordering.
/// C owns only filesystem construction callbacks; Rust owns the dispatch
/// decision and `/dev/vdX[N]` parsing/fallback route.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_dispatch(
    ops: *const VfsMountDispatchOps,
    dev: *const u8,
    path: *const u8,
    fstype: *const u8,
    flags: c_int,
    data: *const u8,
) -> c_int {
    if ops.is_null() || path.is_null() || fstype.is_null() { return -22; }
    let o = unsafe { &*ops };
    if cstr_eq(fstype, b"cgroup") || cstr_eq(fstype, b"cgroup2") {
        return o.mount_cgroup.map_or(-22, |f| f(dev, path, fstype, flags, data));
    }
    let update = match o.update_existing { Some(f) => f, None => return -22 };
    if update(path, flags) != 0 { return 0; }
    if cstr_eq(fstype,b"proc") || cstr_eq(fstype,b"sysfs") ||
       cstr_eq(fstype,b"devtmpfs") || cstr_eq(fstype,b"devfs") ||
       cstr_eq(fstype,b"devpts") {
        return o.mount_virtual.map_or(-22, |f| f(dev,path,fstype,flags,data));
    }
    if cstr_eq(fstype,b"tmpfs") || cstr_eq(fstype,b"ramfs") {
        return o.mount_ram.map_or(-22, |f| f(dev,path,fstype,flags,data));
    }
    if dev.is_null() { return 0; }
    let prefix=b"/dev/vd";
    for (i,b) in prefix.iter().enumerate() {
        let got = unsafe { *dev.add(i) };
        if got == 0 || got != *b { return 0; }
    }
    let disk=unsafe { *dev.add(7) };
    if !(b'a'..=b'z').contains(&disk) { return 0; }
    let mut part=0;
    let mut end=8;
    let digit=unsafe { *dev.add(end) };
    if (b'1'..=b'9').contains(&digit) { part=(digit-b'0') as c_int; end+=1; }
    if unsafe { *dev.add(end) } != 0 { return 0; }
    let try_one = match o.mount_block { Some(f) => f, None => return -22 };
    let primary=(disk-b'a') as c_int;
    let auto=unsafe { *fstype == 0 };
    let attempt = |index:c_int| {
        if auto {
            let mut rc=try_one(index,part,path,b"ext4\0".as_ptr(),flags);
            if rc < 0 { rc=try_one(index,part,path,b"vfat\0".as_ptr(),flags); }
            rc
        } else { try_one(index,part,path,fstype,flags) }
    };
    let mut rc=attempt(primary);
    if rc < 0 && part > 1 {
        let compat=primary+part-1;
        if compat != primary { rc=attempt(compat); }
    }
    rc
}

/// Own the mount teardown admission and retry state machine. Filesystem
/// destructors remain backend calls; Rust controls shared-source refusal,
/// user-count admission, the running guard and failure rollback.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_fs_teardown(
    ops: *const VfsMountFsTeardownOps,
    mnt: *mut c_void,
) -> c_int {
    if ops.is_null() { return 0; }
    if mnt.is_null() { return 0; }
    let o = unsafe { &*ops };
    let (lock, unlock, source, users, running, set_running, kind, root,
        unmount, zero_users, trace) = match (
        o.lock, o.unlock, o.shared_source, o.users, o.is_running, o.set_running,
        o.kind, o.root, o.unmount_backend, o.set_users_zero, o.trace,
    ) {
        (Some(a),Some(b),Some(c),Some(d),Some(e),Some(f),Some(g),Some(h),Some(i),Some(j),Some(k)) =>
            (a,b,c,d,e,f,g,h,i,j,k),
        _ => return -22,
    };
    let flags = lock();
    if !source(mnt).is_null() { unlock(flags); return 0; }
    let count = users(mnt);
    trace(mnt, 0, count);
    if count == 0 { unlock(flags); return 0; }
    if count > 1 || running(mnt) != 0 { unlock(flags); return -16; }
    set_running(mnt, 1);
    unlock(flags);

    let rc = unmount(kind(mnt), root(mnt));
    if rc < 0 {
        let flags = lock();
        set_running(mnt, 0);
        unlock(flags);
        return rc;
    }
    let flags = lock();
    zero_users(mnt);
    unlock(flags);
    trace(mnt, 1, 0);
    0
}

/// Remove a mount from the active namespace table while preserving the
/// graveyard object and its tree/refcount invariants for stale vnode holders.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_remove(
    tree: *const VfsMountTreeOps,
    ops: *const VfsMountRemoveOps,
    mnt: *mut c_void,
) {
    if unsafe { Mt::load(tree) }.is_none() { return; }
    if ops.is_null() || mnt.is_null() { return; }
    let o = unsafe { &*ops };
    let (lock, unlock, count, set_count, current_ns, at, set_at, dead_head, set_dead_head,
        set_dead_next, set_attached, clear_teardown, mountpoint,
        clear_mountpoint, vnode_put, root_put) = match (
        o.lock, o.unlock, o.count, o.set_count, o.current_ns, o.at, o.set_at, o.dead_head,
        o.set_dead_head, o.set_dead_next, o.set_attached,
        o.clear_teardown, o.mountpoint, o.clear_mountpoint, o.vnode_put, o.root_put,
    ) {
        (Some(a),Some(b),Some(c),Some(d),Some(e),Some(f),Some(g),Some(h),
         Some(i),Some(j),Some(k),Some(l),Some(m),Some(n),Some(p),Some(q)) =>
            (a,b,c,d,e,f,g,h,i,j,k,l,m,n,p,q),
        _ => return,
    };
    let flags = lock();
    let ns = current_ns();
    if ns.is_null() { unlock(flags); return; }
    let n = count(ns).max(0);
    let mut index = -1;
    for i in 0..n {
        if at(ns, i) == mnt { index = i; break; }
    }
    if index < 0 { unlock(flags); return; }
    unsafe { a20_vfs_mount_unlink(tree, mnt) };
    set_attached(mnt, 0);
    let mp = mountpoint(mnt);
    if !mp.is_null() {
        /* Clear the field under the mount lock, drop the reference after it:
         * vnode_put can run a filesystem release that takes a sleeping lock
         * (ramfs takes a mutex there), and the switch path asserts on a switch
         * with preemption disabled -- which is exactly what holding this
         * spinlock across that release produces.  root_put below is already
         * ordered the same way. */
        clear_mountpoint(mnt);
    }
    set_dead_next(mnt, dead_head(ns));
    set_dead_head(ns, mnt);
    for i in index..(n - 1) { set_at(ns, i, at(ns, i + 1)); }
    set_at(ns, n - 1, core::ptr::null_mut());
    set_count(ns, n - 1);
    clear_teardown(mnt);
    unlock(flags);
    if !mp.is_null() {
        vnode_put(mp);
    }
    root_put(mnt);
}

/// Destroy namespace-owned mount resources in two phases: first finish every
/// filesystem destructor (so a failure leaves all shared-source pins intact),
/// then release active and graveyard pins. This preserves retryability.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_namespace_teardown(
    ops: *const VfsMountNamespaceTeardownOps,
    ns: *mut c_void,
) -> c_int {
    if ops.is_null() || ns.is_null() { return 0; }
    let o = unsafe { &*ops };
    let (count, at, dead_head, dead_next, source, teardown, dec_users, restore_users, clear_source) = match (
        o.mount_count, o.mount_at, o.dead_head, o.dead_next,
        o.shared_source, o.fs_teardown, o.decrement_source_users,
        o.restore_source_users, o.clear_shared_source,
    ) {
        (Some(a), Some(b), Some(c), Some(d), Some(e), Some(f), Some(g),Some(h),Some(i)) => (a,b,c,d,e,f,g,h,i),
        _ => return -22,
    };
    let n = count(ns).max(0);
    for i in 0..n {
        let m = at(ns, i);
        if m.is_null() || !source(m).is_null() { continue; }
        let rc = teardown(m);
        if rc < 0 { return rc; }
    }
    for i in 0..n {
        let m = at(ns, i);
        if !m.is_null() {
            let rc = unsafe { mount_drop_shared_source(m, source, dec_users, restore_users, clear_source) };
            if rc < 0 { return rc; }
        }
    }
    let mut m = dead_head(ns);
    while !m.is_null() {
        let next = dead_next(m);
        let rc = unsafe { mount_drop_shared_source(m, source, dec_users, restore_users, clear_source) };
        if rc < 0 { return rc; }
        m = next;
    }
    0
}

unsafe fn mount_drop_shared_source(
    mnt: *mut c_void,
    get_source: extern "C" fn(*mut c_void) -> *mut c_void,
    dec_users: extern "C" fn(*mut c_void) -> c_int,
    restore_users: extern "C" fn(*mut c_void),
    clear_source: extern "C" fn(*mut c_void),
) -> c_int {
    let source = get_source(mnt);
    if source.is_null() { return 0; }
    let old = dec_users(source);
    if old <= 1 {
        restore_users(source);
        return -22;
    }
    clear_source(mnt);
    0
}

/// Execute the admitted unmount plan. Rust owns the phase order and child
/// detach walk; C callbacks are limited to filesystem destructors and VFS
/// side-effect primitives whose objects remain C-layout ABI state.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_umount_execute(
    tree: *const VfsMountTreeOps,
    ops: *const VfsUmountExecuteOps,
    mnt: *mut c_void,
    plan: *const VfsUmountPlan,
) -> c_int {
    let t = match unsafe { Mt::load(tree) } { Some(v) => v, None => return -22 };
    if ops.is_null() || mnt.is_null() || plan.is_null() { return -22; }
    let p = unsafe { &*plan };
    let o = unsafe { &*ops };
    let (teardown, invalidate, drop_meta, remove, retry) = match (
        o.teardown, o.invalidate_all, o.drop_time_meta, o.remove, o.retry_pending,
    ) {
        (Some(a), Some(b), Some(c), Some(d), Some(e)) => (a,b,c,d,e),
        _ => return -22,
    };
    let rc = teardown(mnt);
    if rc < 0 { return rc; }
    if p.detach_children != 0 {
        let mut child = (t.child)(mnt);
        while !child.is_null() {
            let next = (t.sibling)(child);
            unsafe { a20_vfs_mount_detach_root(tree, child) };
            child = next;
        }
    }
    if p.invalidate_all != 0 { invalidate(); }
    drop_meta(mnt);
    remove(mnt);
    retry();
    0
}

/// # Safety
/// All pointers and callback tables remain valid for this synchronous call.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_move_mount_commit(
    tree: *const VfsMountTreeOps,
    ops: *const VfsMoveMountOps,
    mnt: *mut c_void,
    parent: *mut c_void,
    mountpoint: *mut c_void,
    path: *const u8,
) -> c_int {
    if unsafe { Mt::load(tree) }.is_none() { return -22; }
    if ops.is_null() || mnt.is_null() || path.is_null() { return -22; }
    let o = unsafe { &*ops };
    let (lock, unlock, present, teardown, set_path, invalidate) = match (
        o.lock, o.unlock, o.is_present, o.is_teardown, o.set_path, o.invalidate_dcache,
    ) {
        (Some(a), Some(b), Some(c), Some(d), Some(e), Some(f)) => (a,b,c,d,e,f),
        _ => return -22,
    };
    let flags = lock();
    if present(mnt) == 0 || teardown(mnt) != 0 {
        unlock(flags);
        return -16; // -EBUSY
    }
    set_path(mnt, path);
    let rc = unsafe { a20_vfs_mount_attach(tree, mnt, parent, mountpoint) };
    unlock(flags);
    if rc < 0 { return rc; }
    invalidate();
    0
}

struct Mt<'a> {
    parent: &'a extern "C" fn(*mut c_void) -> *mut c_void,
    child: &'a extern "C" fn(*mut c_void) -> *mut c_void,
    sibling: &'a extern "C" fn(*mut c_void) -> *mut c_void,
    mp: &'a extern "C" fn(*mut c_void) -> *mut c_void,
    set_parent: &'a extern "C" fn(*mut c_void, *mut c_void),
    set_child: &'a extern "C" fn(*mut c_void, *mut c_void),
    set_sibling: &'a extern "C" fn(*mut c_void, *mut c_void),
    set_mp: &'a extern "C" fn(*mut c_void, *mut c_void),
    set_detached: &'a extern "C" fn(*mut c_void),
    vnode_get: &'a extern "C" fn(*mut c_void),
    vnode_put: &'a extern "C" fn(*mut c_void),
}

impl<'a> Mt<'a> {
    unsafe fn load(t: *const VfsMountTreeOps) -> Option<Mt<'a>> {
        if t.is_null() {
            return None;
        }
        // SAFETY: caller passes a live table for the duration of the call.
        let t = unsafe { &*t };
        Some(Mt {
            parent: t.parent.as_ref()?,
            child: t.child.as_ref()?,
            sibling: t.sibling.as_ref()?,
            mp: t.mp.as_ref()?,
            set_parent: t.set_parent.as_ref()?,
            set_child: t.set_child.as_ref()?,
            set_sibling: t.set_sibling.as_ref()?,
            set_mp: t.set_mp.as_ref()?,
            set_detached: t.set_detached.as_ref()?,
            vnode_get: t.vnode_get.as_ref()?,
            vnode_put: t.vnode_put.as_ref()?,
        })
    }
}

/// Detach `mnt` from its parent's child list, reproducing `mount_tree_unlink`:
/// the link that points at `mnt` (the parent's child head or a preceding
/// sibling's next) becomes NULL, which truncates the list at `mnt`.
///
/// # Safety
/// `ops` is a live table; `mnt` is a live mount (or NULL).
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_unlink(ops: *const VfsMountTreeOps, mnt: *mut c_void) {
    let o = match unsafe { Mt::load(ops) } {
        Some(o) => o,
        None => return,
    };
    if mnt.is_null() {
        return;
    }
    let parent = (o.parent)(mnt);
    (o.set_sibling)(mnt, core::ptr::null_mut());
    if parent.is_null() {
        return;
    }
    let mut pp = (o.child)(parent);
    if pp == mnt {
        (o.set_child)(parent, core::ptr::null_mut());
        return;
    }
    while !pp.is_null() {
        let next = (o.sibling)(pp);
        if next == mnt {
            (o.set_sibling)(pp, core::ptr::null_mut());
            return;
        }
        pp = next;
    }
}

/// Attach `mnt` under `parent` covering `mp`.  The mount takes a reference on
/// `mp` and drops the one it held on its previous mountpoint, so re-parenting
/// never leaks or dangles.
///
/// # Safety
/// `ops` is a live table; `mnt` is a live mount.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_attach(
    ops: *const VfsMountTreeOps,
    mnt: *mut c_void,
    parent: *mut c_void,
    mp: *mut c_void,
) -> c_int {
    let o = match unsafe { Mt::load(ops) } {
        Some(o) => o,
        None => return -22, /* -EINVAL */
    };
    if mnt.is_null() {
        return -22;
    }
    unsafe { a20_vfs_mount_unlink(ops, mnt) };
    let old = (o.mp)(mnt);
    if !old.is_null() {
        (o.vnode_put)(old);
    }
    (o.set_mp)(mnt, mp);
    if !mp.is_null() {
        (o.vnode_get)(mp);
    }
    (o.set_parent)(mnt, parent);
    if !parent.is_null() {
        (o.set_sibling)(mnt, (o.child)(parent));
        (o.set_child)(parent, mnt);
    }
    0
}

/// Detach a mount made unreachable by pivot_root: unlink it, drop its parent
/// and mark it detached.
///
/// # Safety
/// `ops` is a live table; `mnt` is a live mount (or NULL).
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_mount_detach_root(ops: *const VfsMountTreeOps, mnt: *mut c_void) {
    let o = match unsafe { Mt::load(ops) } {
        Some(o) => o,
        None => return,
    };
    if mnt.is_null() {
        return;
    }
    unsafe { a20_vfs_mount_unlink(ops, mnt) };
    (o.set_parent)(mnt, core::ptr::null_mut());
    (o.set_detached)(mnt);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn cands<'a>(paths: &'a [&'a str], facts: &[(u32, u32, u32, u32)]) -> Vec<VfsMountCand> {
        // The caller keeps `paths` alive; we leak the C strings for the test.
        let mut v = Vec::new();
        for (i, p) in paths.iter().enumerate() {
            let mut b = [0u8; 64];
            b[..p.len()].copy_from_slice(p.as_bytes());
            let leaked: &'static mut [u8; 64] = Box::leak(Box::new(b));
            let (ru, hc, cu, nr) = facts[i];
            v.push(VfsMountCand {
                path: leaked.as_ptr(),
                root_users: ru,
                has_child: hc,
                cwd_users: cu,
                is_ns_root: nr,
            });
        }
        v
    }

    fn plan(query: &str, flags: i32, check: bool, paths: &[&str], facts: &[(u32, u32, u32, u32)]) -> (i32, VfsUmountPlan) {
        let mut q = [0u8; 64];
        q[..query.len()].copy_from_slice(query.as_bytes());
        let c = cands(paths, facts);
        let mut out = VfsUmountPlan { index: -1, detach_children: 0, invalidate_all: 0 };
        let r = unsafe {
            a20_vfs_umount_plan(q.as_ptr(), flags, check as c_int, c.as_ptr(), c.len(), &mut out)
        };
        (r, out)
    }

    #[test]
    fn matches_after_trailing_slash_normalisation() {
        let paths = ["/", "/data", "/data/sub/"];
        let facts = [(1, 1, 0, 1), (1, 0, 0, 0), (1, 0, 0, 0)];
        let (r, p) = plan("/data/", 0, true, &paths, &facts);
        assert_eq!((r, p.index, p.invalidate_all), (0, 1, 1));
        let (r, p) = plan("/data/sub", 0, true, &paths, &facts);
        assert_eq!((r, p.index), (0, 2));
        // No such mount.
        assert_eq!(plan("/nope", 0, true, &paths, &facts).0, EINVAL);
        // A path that only shares a prefix does not match.
        assert_eq!(plan("/dat", 0, true, &paths, &facts).0, EINVAL);
    }

    #[test]
    fn busy_rules_keep_their_order_and_detach_waives_them() {
        let paths = ["/", "/m"];
        // root pinned
        assert_eq!(plan("/m", 0, true, &paths, &[(1, 0, 0, 0), (2, 0, 0, 0)]).0, EBUSY);
        // a child mount below
        assert_eq!(plan("/m", 0, true, &paths, &[(1, 0, 0, 0), (1, 1, 0, 0)]).0, EBUSY);
        // a process standing in it
        assert_eq!(plan("/m", 0, true, &paths, &[(1, 0, 0, 0), (1, 0, 3, 0)]).0, EBUSY);
        // the namespace root
        assert_eq!(plan("/", 0, true, &paths, &[(1, 0, 0, 1), (1, 0, 0, 0)]).0, EINVAL);
        // MNT_DETACH waives all four.
        let (r, p) = plan("/", VFS_UMOUNT_DETACH, true, &paths, &[(9, 1, 9, 1), (1, 0, 0, 0)]);
        assert_eq!((r, p.index, p.detach_children), (0, 0, 1));
        // The legacy path skips the rules entirely.
        assert_eq!(plan("/", 0, false, &paths, &[(9, 1, 9, 1), (1, 0, 0, 0)]).0, 0);
    }

    #[test]
    fn plan_reports_detach_children_for_the_matched_mount_only() {
        let paths = ["/", "/m"];
        let (r, p) = plan("/m", 0, true, &paths, &[(1, 1, 0, 0), (1, 0, 0, 0)]);
        assert_eq!((r, p.index, p.detach_children), (0, 1, 0));
    }

    fn mv(from: &str, to: &str, found: bool, from_path: &str, detached: bool, covered: bool) -> (i32, i32) {
        let mut a = [0u8; 64];
        a[..from.len()].copy_from_slice(from.as_bytes());
        let mut b = [0u8; 64];
        b[..to.len()].copy_from_slice(to.as_bytes());
        let mut p = [0u8; 64];
        p[..from_path.len()].copy_from_slice(from_path.as_bytes());
        let mut noop = 0;
        let r = unsafe {
            a20_vfs_move_mount_check(a.as_ptr(), b.as_ptr(), found as c_int, p.as_ptr(),
                                     detached as c_int, covered as c_int, &mut noop)
        };
        (r, noop)
    }

    #[test]
    fn move_validation_keeps_its_order() {
        assert_eq!(mv("rel", "/x", true, "/rel", false, false).0, EINVAL);
        assert_eq!(mv("/a", "rel", true, "/a", false, false).0, EINVAL);
        assert_eq!(mv("/a", "/b", false, "/a", false, false).0, ENOENT);
        assert_eq!(mv("/a", "/b", true, "/a", true, false).0, EINVAL);
        // A covered destination is refused before the nesting check.
        assert_eq!(mv("/a", "/a/b", true, "/a", false, true).0, EBUSY);
        // Moving a mount underneath itself.
        assert_eq!(mv("/a", "/a/b", true, "/a", false, false).0, EINVAL);
        // The same path is the no-op branch, which precedes the nesting check.
        assert_eq!(mv("/a", "/a", true, "/a", false, false), (0, 1));
        // A sibling prefix is not nesting.
        assert_eq!(mv("/a", "/ab", true, "/a", false, false).0, 0);
        assert_eq!(mv("/a", "/b", true, "/a", false, false).0, 0);
    }

    #[test]
    fn move_reports_the_unreachable_noop_branch() {
        // `target_covered` is false only in the caller's synthetic case; the
        // engine still reports the original success-with-noop.
        let (r, noop) = mv("/a", "/a", true, "/a", false, false);
        assert_eq!((r, noop), (0, 1));
    }
    fn mp(path: &'static [u8]) -> VfsMountMatch {
        VfsMountMatch { path: path.as_ptr(), ready: 1, teardown: 0, detached: 0 }
    }

    unsafe fn find(q: &[u8], include_detached: c_int, ext: c_int, c: &[VfsMountMatch]) -> i32 {
        unsafe { a20_vfs_find_mount(q.as_ptr(), include_detached, ext, c.as_ptr(), c.len()) }
    }

    #[test]
    fn find_mount_prefers_the_longest_visible_prefix() {
        let c = [mp(b"/\0"), mp(b"/a\0"), mp(b"/a/b\0"), mp(b"/ab\0")];
        assert_eq!(unsafe { find(b"/a/b/c\0", 0, 0, &c) }, 2);
        assert_eq!(unsafe { find(b"/a/x\0", 0, 0, &c) }, 1);
        assert_eq!(unsafe { find(b"/ab\0", 0, 0, &c) }, 3);
        assert_eq!(unsafe { find(b"/z\0", 0, 0, &c) }, 0); /* only "/" matches */
        /* "/ab" must not be reached through the "/a" prefix (boundary rule) */
        assert_ne!(unsafe { find(b"/ab/c\0", 0, 0, &c) }, 1);
    }

    #[test]
    fn find_mount_skips_unready_teardown_and_detached() {
        let mut c = [mp(b"/\0"), mp(b"/a\0")];
        c[1].ready = 0;
        assert_eq!(unsafe { find(b"/a/x\0", 0, 0, &c) }, 0);
        c[1].ready = 1;
        c[1].teardown = 1;
        assert_eq!(unsafe { find(b"/a/x\0", 0, 0, &c) }, 0);
        c[1].teardown = 0;
        c[1].detached = 1;
        assert_eq!(unsafe { find(b"/a/x\0", 0, 0, &c) }, 0);
        assert_eq!(unsafe { find(b"/a/x\0", 1, 0, &c) }, 1);
    }

    #[test]
    fn find_mount_external_root_tie_prefers_the_later_root() {
        let c = [mp(b"/\0"), mp(b"/\0")];
        assert_eq!(unsafe { find(b"/x\0", 0, 0, &c) }, 0);
        assert_eq!(unsafe { find(b"/x\0", 0, 1, &c) }, 1);
    }

    #[test]
    fn strip_prefix_offsets() {
        unsafe {
            assert_eq!(a20_vfs_strip_prefix(b"/a/b/c\0".as_ptr(), b"/a\0".as_ptr()), 3);
            assert_eq!(a20_vfs_strip_prefix(b"/a\0".as_ptr(), b"/a\0".as_ptr()), 2);
            assert_eq!(a20_vfs_strip_prefix(b"/a\0".as_ptr(), b"/a/\0".as_ptr()), 0); /* no match */
            assert_eq!(a20_vfs_strip_prefix(b"/x\0".as_ptr(), b"/a\0".as_ptr()), 0);
            assert_eq!(a20_vfs_strip_prefix(b"/x\0".as_ptr(), b"/\0".as_ptr()), 1);
        }
    }

    #[test]
    fn mount_parent_path_trims_and_splits() {
        let mut out = [0u8; 64];
        unsafe {
            assert_eq!(a20_vfs_mount_parent_path(b"/\0".as_ptr(), out.as_mut_ptr(), out.len()), 0);
            assert_eq!(a20_vfs_mount_parent_path(b"/a\0".as_ptr(), out.as_mut_ptr(), out.len()), 1);
            assert_eq!(&out[..2], b"/\0");
            assert_eq!(a20_vfs_mount_parent_path(b"/a/b/\0".as_ptr(), out.as_mut_ptr(), out.len()), 1);
            assert_eq!(&out[..3], b"/a\0");
            assert_eq!(a20_vfs_mount_parent_path(b"rel\0".as_ptr(), out.as_mut_ptr(), out.len()), 0);
        }
    }

    /* ---- mount-tree side effects ---- */

    #[derive(Clone)]
    struct Tree {
        parent: [usize; 8],
        child: [usize; 8],
        sib: [usize; 8],
        mp: [usize; 8],
        detached: [bool; 8],
        vget: [u32; 8],
        vput: [u32; 8],
    }
    impl Tree {
        fn new() -> Self {
            Tree { parent: [0;8], child: [0;8], sib: [0;8], mp: [0;8], detached: [false;8], vget: [0;8], vput: [0;8] }
        }
    }
    thread_local! {
        static TREE: core::cell::RefCell<Tree> = core::cell::RefCell::new(Tree::new());
    }
    fn p2i(p: *mut c_void) -> usize { p as usize }
    fn i2p(i: usize) -> *mut c_void { i as *mut c_void }
    fn tree_reset() { TREE.with(|t| *t.borrow_mut() = Tree::new()); }
    extern "C" fn t_parent(m: *mut c_void) -> *mut c_void { TREE.with(|t| i2p(t.borrow().parent[p2i(m)])) }
    extern "C" fn t_child(m: *mut c_void) -> *mut c_void { TREE.with(|t| i2p(t.borrow().child[p2i(m)])) }
    extern "C" fn t_sib(m: *mut c_void) -> *mut c_void { TREE.with(|t| i2p(t.borrow().sib[p2i(m)])) }
    extern "C" fn t_mp(m: *mut c_void) -> *mut c_void { TREE.with(|t| i2p(t.borrow().mp[p2i(m)])) }
    extern "C" fn t_set_parent(m: *mut c_void, v: *mut c_void) { TREE.with(|t| t.borrow_mut().parent[p2i(m)] = p2i(v)); }
    extern "C" fn t_set_child(m: *mut c_void, v: *mut c_void) { TREE.with(|t| t.borrow_mut().child[p2i(m)] = p2i(v)); }
    extern "C" fn t_set_sib(m: *mut c_void, v: *mut c_void) { TREE.with(|t| t.borrow_mut().sib[p2i(m)] = p2i(v)); }
    extern "C" fn t_set_mp(m: *mut c_void, v: *mut c_void) { TREE.with(|t| t.borrow_mut().mp[p2i(m)] = p2i(v)); }
    extern "C" fn t_set_detached(m: *mut c_void) { TREE.with(|t| t.borrow_mut().detached[p2i(m)] = true); }
    extern "C" fn t_vget(v: *mut c_void) { TREE.with(|t| t.borrow_mut().vget[p2i(v)] += 1); }
    extern "C" fn t_vput(v: *mut c_void) { TREE.with(|t| t.borrow_mut().vput[p2i(v)] += 1); }
    fn ttable() -> VfsMountTreeOps {
        VfsMountTreeOps {
            parent: Some(t_parent), child: Some(t_child), sibling: Some(t_sib), mp: Some(t_mp),
            set_parent: Some(t_set_parent), set_child: Some(t_set_child), set_sibling: Some(t_set_sib),
            set_mp: Some(t_set_mp), set_detached: Some(t_set_detached),
            vnode_get: Some(t_vget), vnode_put: Some(t_vput),
        }
    }
    fn tget<F: Fn(&Tree)->usize>(f: F) -> usize { TREE.with(|t| f(&t.borrow())) }
fn tget32<F: Fn(&Tree)->u32>(f: F) -> u32 { TREE.with(|t| f(&t.borrow())) }

    thread_local! {
        static MOVE_STATE: core::cell::RefCell<(bool, bool, [u8; 32], u32)> =
            core::cell::RefCell::new((true, false, [0; 32], 0));
    }
    extern "C" fn mv_lock() -> u64 { 7 }
    extern "C" fn mv_unlock(v: u64) { assert_eq!(v, 7); }
    extern "C" fn mv_present(_: *mut c_void) -> c_int { MOVE_STATE.with(|s| s.borrow().0 as c_int) }
    extern "C" fn mv_teardown(_: *mut c_void) -> c_int { MOVE_STATE.with(|s| s.borrow().1 as c_int) }
    extern "C" fn mv_set_path(_: *mut c_void, p: *const u8) {
        MOVE_STATE.with(|s| {
            let mut s = s.borrow_mut();
            s.2 = [0; 32];
            for i in 0..31 { let b = unsafe { *p.add(i) }; s.2[i] = b; if b == 0 { break; } }
        });
    }
    extern "C" fn mv_invalidate() { MOVE_STATE.with(|s| s.borrow_mut().3 += 1); }

    #[test]
    fn move_commit_rechecks_and_publishes_atomically() {
        tree_reset();
        MOVE_STATE.with(|s| *s.borrow_mut() = (true, false, [0; 32], 0));
        TREE.with(|t| t.borrow_mut().mp[2] = 5);
        let tree = ttable();
        let ops = VfsMoveMountOps {
            lock: Some(mv_lock), unlock: Some(mv_unlock), is_present: Some(mv_present),
            is_teardown: Some(mv_teardown), set_path: Some(mv_set_path),
            invalidate_dcache: Some(mv_invalidate),
        };
        let rc = unsafe { a20_vfs_move_mount_commit(&tree, &ops, i2p(2), i2p(1), i2p(6), b"/new\0".as_ptr()) };
        assert_eq!(rc, 0);
        assert_eq!(tget(|t| t.parent[2]), 1);
        assert_eq!(tget(|t| t.mp[2]), 6);
        assert_eq!(tget32(|t| t.vput[5]), 1);
        assert_eq!(tget32(|t| t.vget[6]), 1);
        MOVE_STATE.with(|s| { let s = s.borrow(); assert_eq!(&s.2[..5], b"/new\0"); assert_eq!(s.3, 1); });

        MOVE_STATE.with(|s| *s.borrow_mut() = (true, true, [0; 32], 0));
        let rc = unsafe { a20_vfs_move_mount_commit(&tree, &ops, i2p(3), i2p(1), core::ptr::null_mut(), b"/blocked\0".as_ptr()) };
        assert_eq!(rc, EBUSY);
        MOVE_STATE.with(|s| { let s = s.borrow(); assert_eq!(s.2[0], 0); assert_eq!(s.3, 0); });
    }

    thread_local! {
        static UMOUNT_STATE: core::cell::RefCell<(Vec<i32>, bool)> =
            core::cell::RefCell::new((Vec::new(), false));
    }
    fn ulog(n: i32) { UMOUNT_STATE.with(|s| s.borrow_mut().0.push(n)); }
    extern "C" fn u_teardown(_: *mut c_void) -> c_int {
        ulog(1);
        UMOUNT_STATE.with(|s| if s.borrow().1 { -16 } else { 0 })
    }
    extern "C" fn u_invalidate() { ulog(2); }
    extern "C" fn u_meta(_: *mut c_void) { ulog(3); }
    extern "C" fn u_remove(_: *mut c_void) { ulog(4); }
    extern "C" fn u_retry() { ulog(5); }

    #[test]
    fn umount_executor_keeps_failure_atomic_and_orders_side_effects() {
        tree_reset();
        TREE.with(|t| { let mut b=t.borrow_mut(); b.child[1]=2; b.parent[2]=1; b.sib[2]=3; b.parent[3]=1; });
        UMOUNT_STATE.with(|s| *s.borrow_mut() = (Vec::new(), true));
        let tree = ttable();
        let ops = VfsUmountExecuteOps {
            teardown: Some(u_teardown), invalidate_all: Some(u_invalidate),
            drop_time_meta: Some(u_meta), remove: Some(u_remove), retry_pending: Some(u_retry),
        };
        let plan = VfsUmountPlan { index: 0, detach_children: 1, invalidate_all: 1 };
        assert_eq!(unsafe { a20_vfs_umount_execute(&tree, &ops, i2p(1), &plan) }, EBUSY);
        UMOUNT_STATE.with(|s| assert_eq!(s.borrow().0, vec![1]));
        assert_eq!(tget(|t| t.child[1]), 2); // failed teardown leaves graph untouched

        UMOUNT_STATE.with(|s| *s.borrow_mut() = (Vec::new(), false));
        assert_eq!(unsafe { a20_vfs_umount_execute(&tree, &ops, i2p(1), &plan) }, 0);
        UMOUNT_STATE.with(|s| assert_eq!(s.borrow().0, vec![1, 2, 3, 4, 5]));
        assert_eq!(tget(|t| t.child[1]), 0);
        assert_eq!(tget(|t| t.parent[2]), 0);
        assert_eq!(tget(|t| t.parent[3]), 0);
    }

    struct NsTeardownTest { fail: bool, log: Vec<i32>, shared: [usize; 4], users: [i32; 4] }
    thread_local! {
        static NS_TEARDOWN: core::cell::RefCell<NsTeardownTest> = core::cell::RefCell::new(
            NsTeardownTest { fail: false, log: Vec::new(), shared: [0; 4], users: [0; 4] });
    }
    extern "C" fn ns_t_count(_: *mut c_void) -> c_int { 2 }
    extern "C" fn ns_t_at(_: *mut c_void, i: c_int) -> *mut c_void { i2p((i + 1) as usize) }
    extern "C" fn ns_t_dead(_: *mut c_void) -> *mut c_void { i2p(3) }
    extern "C" fn ns_t_next(_: *mut c_void) -> *mut c_void { core::ptr::null_mut() }
    extern "C" fn ns_t_source(m: *mut c_void) -> *mut c_void {
        NS_TEARDOWN.with(|s| i2p(s.borrow().shared[p2i(m)]))
    }
    extern "C" fn ns_t_fs(m: *mut c_void) -> c_int {
        let id = p2i(m);
        NS_TEARDOWN.with(|s| { let mut s=s.borrow_mut(); s.log.push(10 + id as i32); if s.fail && id == 1 { -5 } else { 0 } })
    }
    extern "C" fn ns_t_dec(source: *mut c_void) -> c_int {
        let id = p2i(source);
        NS_TEARDOWN.with(|s| { let mut s=s.borrow_mut(); let old=s.users[id]; s.users[id]-=1; old })
    }
    extern "C" fn ns_t_restore(source: *mut c_void) { NS_TEARDOWN.with(|s| s.borrow_mut().users[p2i(source)]+=1); }
    extern "C" fn ns_t_clear(m: *mut c_void) {
        let id=p2i(m);
        NS_TEARDOWN.with(|s| { let mut s=s.borrow_mut(); s.log.push(20 + id as i32); s.shared[id]=0; });
    }

    #[test]
    fn namespace_teardown_finishes_destructors_before_releasing_any_pin() {
        NS_TEARDOWN.with(|s| *s.borrow_mut() = NsTeardownTest { fail: true, log: Vec::new(), shared: [0,0,1,1], users: [0,3,0,0] });
        let ops = VfsMountNamespaceTeardownOps {
            mount_count: Some(ns_t_count), mount_at: Some(ns_t_at), dead_head: Some(ns_t_dead),
            dead_next: Some(ns_t_next), shared_source: Some(ns_t_source),
            fs_teardown: Some(ns_t_fs), decrement_source_users: Some(ns_t_dec),
            restore_source_users: Some(ns_t_restore), clear_shared_source: Some(ns_t_clear),
        };
        assert_eq!(unsafe { a20_vfs_mount_namespace_teardown(&ops, i2p(1)) }, -5);
        NS_TEARDOWN.with(|s| { let s=s.borrow(); assert_eq!(s.log, vec![11]); assert_eq!(s.shared[2], 1); assert_eq!(s.shared[3], 1); });

        NS_TEARDOWN.with(|s| *s.borrow_mut() = NsTeardownTest { fail: false, log: Vec::new(), shared: [0,0,1,1], users: [0,3,0,0] });
        assert_eq!(unsafe { a20_vfs_mount_namespace_teardown(&ops, i2p(1)) }, 0);
        NS_TEARDOWN.with(|s| { let s=s.borrow(); assert_eq!(s.log, vec![11,22,23]); assert_eq!(s.shared[2], 0); assert_eq!(s.shared[3], 0); assert_eq!(s.users[1], 1); });
    }

    struct FsTearTest { shared: bool, users: i32, running: bool, backend_rc: i32, trace: Vec<(i32,i32)> }
    thread_local! {
        static FS_TEAR: core::cell::RefCell<FsTearTest> = core::cell::RefCell::new(
            FsTearTest { shared:false, users:1, running:false, backend_rc:0, trace:Vec::new() });
    }
    extern "C" fn ft_lock() -> u64 { 9 }
    extern "C" fn ft_unlock(v:u64) { assert_eq!(v,9); }
    extern "C" fn ft_shared(_: *mut c_void) -> *mut c_void { FS_TEAR.with(|s| if s.borrow().shared { i2p(1) } else { core::ptr::null_mut() }) }
    extern "C" fn ft_users(_: *mut c_void) -> c_int { FS_TEAR.with(|s| s.borrow().users) }
    extern "C" fn ft_running(_: *mut c_void) -> c_int { FS_TEAR.with(|s| s.borrow().running as c_int) }
    extern "C" fn ft_set_running(_: *mut c_void, v:c_int) { FS_TEAR.with(|s| s.borrow_mut().running = v != 0); }
    extern "C" fn ft_kind(_: *mut c_void) -> c_int { 4 }
    extern "C" fn ft_root(_: *mut c_void) -> *mut c_void { i2p(2) }
    extern "C" fn ft_backend(kind:c_int, root:*mut c_void) -> c_int { assert_eq!(kind,4); assert_eq!(root,i2p(2)); FS_TEAR.with(|s| s.borrow().backend_rc) }
    extern "C" fn ft_zero(_: *mut c_void) { FS_TEAR.with(|s| s.borrow_mut().users=0); }
    extern "C" fn ft_trace(_: *mut c_void, phase:c_int, users:c_int) { FS_TEAR.with(|s| s.borrow_mut().trace.push((phase,users))); }

    #[test]
    fn filesystem_teardown_owns_admission_and_failure_rollback() {
        let ops=VfsMountFsTeardownOps { lock:Some(ft_lock), unlock:Some(ft_unlock), shared_source:Some(ft_shared),
            users:Some(ft_users), is_running:Some(ft_running), set_running:Some(ft_set_running), kind:Some(ft_kind),
            root:Some(ft_root), unmount_backend:Some(ft_backend), set_users_zero:Some(ft_zero), trace:Some(ft_trace) };
        FS_TEAR.with(|s| *s.borrow_mut()=FsTearTest { shared:true, users:1, running:false, backend_rc:0, trace:Vec::new() });
        assert_eq!(unsafe { a20_vfs_mount_fs_teardown(&ops,i2p(3)) },0);
        FS_TEAR.with(|s| assert!(s.borrow().trace.is_empty()));
        FS_TEAR.with(|s| *s.borrow_mut()=FsTearTest { shared:false, users:2, running:false, backend_rc:0, trace:Vec::new() });
        assert_eq!(unsafe { a20_vfs_mount_fs_teardown(&ops,i2p(3)) },EBUSY);
        FS_TEAR.with(|s| {let s=s.borrow(); assert!(!s.running); assert_eq!(s.trace,vec![(0,2)]);});
        FS_TEAR.with(|s| *s.borrow_mut()=FsTearTest { shared:false, users:1, running:false, backend_rc:-5, trace:Vec::new() });
        assert_eq!(unsafe { a20_vfs_mount_fs_teardown(&ops,i2p(3)) },-5);
        FS_TEAR.with(|s| {let s=s.borrow(); assert!(!s.running); assert_eq!(s.users,1); assert_eq!(s.trace,vec![(0,1)]);});
        FS_TEAR.with(|s| *s.borrow_mut()=FsTearTest { shared:false, users:1, running:false, backend_rc:0, trace:Vec::new() });
        assert_eq!(unsafe { a20_vfs_mount_fs_teardown(&ops,i2p(3)) },0);
        FS_TEAR.with(|s| {let s=s.borrow(); assert!(s.running); assert_eq!(s.users,0); assert_eq!(s.trace,vec![(0,1),(1,0)]);});
    }

    thread_local! { static MOUNT_ROUTE: core::cell::RefCell<(bool, Vec<(i32,i32,i32)>)> = core::cell::RefCell::new((false,Vec::new())); }
    extern "C" fn md_existing(_: *const u8, _: c_int) -> c_int { MOUNT_ROUTE.with(|s| { let mut s=s.borrow_mut(); s.1.push((0,0,0)); s.0 as c_int }) }
    extern "C" fn md_cgroup(_: *const u8, _: *const u8, _: *const u8, _: c_int, _: *const u8) -> c_int { MOUNT_ROUTE.with(|s| s.borrow_mut().1.push((1,0,0))); 11 }
    extern "C" fn md_virtual(_: *const u8, _: *const u8, _: *const u8, _: c_int, _: *const u8) -> c_int { MOUNT_ROUTE.with(|s| s.borrow_mut().1.push((2,0,0))); 12 }
    extern "C" fn md_ram(_: *const u8, _: *const u8, _: *const u8, _: c_int, _: *const u8) -> c_int { MOUNT_ROUTE.with(|s| s.borrow_mut().1.push((3,0,0))); 13 }
    extern "C" fn md_block(d:c_int,p:c_int,_:*const u8,fs:*const u8,_:c_int) -> c_int {
        let first=unsafe { *fs };
        let route=if first==b'e' {40} else if first==b'v' {41} else {4};
        let fail= MOUNT_ROUTE.with(|s| { let mut s=s.borrow_mut(); s.1.push((route,d,p)); route==40 && p==0 });
        if fail {-5} else {14}
    }

    #[test]
    fn mount_dispatch_preserves_backend_order_and_block_partition_parse() {
        let ops=VfsMountDispatchOps { update_existing:Some(md_existing), mount_cgroup:Some(md_cgroup),
            mount_virtual:Some(md_virtual), mount_ram:Some(md_ram), mount_block:Some(md_block) };
        MOUNT_ROUTE.with(|s| *s.borrow_mut()=(false,Vec::new()));
        assert_eq!(unsafe { a20_vfs_mount_dispatch(&ops,b"none\0".as_ptr(),b"/m\0".as_ptr(),b"cgroup2\0".as_ptr(),0,core::ptr::null()) },11);
        MOUNT_ROUTE.with(|s| assert_eq!(s.borrow().1,vec![(1,0,0)])); // cgroup precedes same-path update

        MOUNT_ROUTE.with(|s| *s.borrow_mut()=(true,Vec::new()));
        assert_eq!(unsafe { a20_vfs_mount_dispatch(&ops,b"none\0".as_ptr(),b"/m\0".as_ptr(),b"proc\0".as_ptr(),0,core::ptr::null()) },0);
        MOUNT_ROUTE.with(|s| assert_eq!(s.borrow().1,vec![(0,0,0)])); // existing route short-circuits

        MOUNT_ROUTE.with(|s| *s.borrow_mut()=(false,Vec::new()));
        assert_eq!(unsafe { a20_vfs_mount_dispatch(&ops,b"/dev/vdb2\0".as_ptr(),b"/m\0".as_ptr(),b"ext4\0".as_ptr(),0,core::ptr::null()) },14);
        MOUNT_ROUTE.with(|s| assert_eq!(s.borrow().1,vec![(0,0,0),(40,1,2)]));

        MOUNT_ROUTE.with(|s| *s.borrow_mut()=(false,Vec::new()));
        assert_eq!(unsafe { a20_vfs_mount_dispatch(&ops,b"/dev/vdb\0".as_ptr(),b"/m\0".as_ptr(),b"\0".as_ptr(),0,core::ptr::null()) },14);
        MOUNT_ROUTE.with(|s| assert_eq!(s.borrow().1,vec![(0,0,0),(40,1,0),(41,1,0)])); // auto probes ext4, then vfat

        MOUNT_ROUTE.with(|s| *s.borrow_mut()=(false,Vec::new()));
        assert_eq!(unsafe { a20_vfs_mount_dispatch(&ops,b"/dev/vdb10\0".as_ptr(),b"/m\0".as_ptr(),b"ext4\0".as_ptr(),0,core::ptr::null()) },0);
        MOUNT_ROUTE.with(|s| assert_eq!(s.borrow().1,vec![(0,0,0)])); // multi-digit partition is invalid
    }

    #[test]
    fn tree_unlink_truncates_at_the_mount() {
        tree_reset();
        TREE.with(|t| { let mut b=t.borrow_mut(); b.child[1]=2; b.parent[2]=1; b.sib[2]=3; b.parent[3]=1; b.sib[3]=4; b.parent[4]=1; });
        let o = ttable();
        unsafe { a20_vfs_mount_unlink(&o, i2p(3)) };
        assert_eq!(tget(|t| t.child[1]), 2);
        assert_eq!(tget(|t| t.sib[2]), 0); /* link to 3 truncated */
        assert_eq!(tget(|t| t.sib[3]), 0);
    }

    #[test]
    fn tree_unlink_head_and_absent() {
        tree_reset();
        TREE.with(|t| { let mut b=t.borrow_mut(); b.child[1]=2; b.parent[2]=1; });
        let o = ttable();
        unsafe { a20_vfs_mount_unlink(&o, i2p(2)) };
        assert_eq!(tget(|t| t.child[1]), 0);
        /* an unrelated mount is a no-op */
        unsafe { a20_vfs_mount_unlink(&o, i2p(7)) };
        assert_eq!(tget(|t| t.child[1]), 0);
    }

    #[test]
    fn tree_attach_transfers_the_mountpoint_reference() {
        tree_reset();
        TREE.with(|t| { let mut b=t.borrow_mut(); b.mp[2]=5; });
        let o = ttable();
        let rc = unsafe { a20_vfs_mount_attach(&o, i2p(2), i2p(1), i2p(6)) };
        assert_eq!(rc, 0);
        assert_eq!(tget32(|t| t.vput[5]), 1);   /* old mountpoint released */
        assert_eq!(tget32(|t| t.vget[6]), 1);   /* new mountpoint pinned */
        assert_eq!(tget(|t| t.mp[2]), 6);
        assert_eq!(tget(|t| t.parent[2]), 1);
        assert_eq!(tget(|t| t.child[1]), 2);
    }

    #[test]
    fn tree_attach_null_mp_and_detach_root() {
        tree_reset();
        let o = ttable();
        assert_eq!(unsafe { a20_vfs_mount_attach(&o, i2p(2), i2p(1), core::ptr::null_mut()) }, 0);
        assert_eq!(tget(|t| t.mp[2]), 0);
        unsafe { a20_vfs_mount_detach_root(&o, i2p(2)) };
        assert_eq!(tget(|t| t.parent[2]), 0);
        assert!(TREE.with(|t| t.borrow().detached[2]));
        assert_eq!(tget(|t| t.child[1]), 0); /* unlinked first */
    }

}
