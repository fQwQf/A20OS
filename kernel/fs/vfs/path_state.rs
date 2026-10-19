//! VFS path/name state engine (Rust-owned split for `vfs/path.c`).
//!
//! `vfs/path.c` was the path-string layer every path-taking syscall funnels
//! through: `vfs_resolve*`, `vfs_rename_flags`, `vfs_unlink`, `vfs_mkdir`,
//! `mount_ops` and the `openat2` walk all join, normalise, split and bound-check
//! the same strings here.  This module owns those algorithms:
//!
//! * the `//`-and-`.`-and-`..` collapse with its `ENAMETOOLONG` overflow rule
//!   (`a20_vfs_path_normalize_absolute`);
//! * the same collapse floored at a chroot/`RESOLVE_IN_ROOT` boundary
//!   (`a20_vfs_path_normalize_absolute_with_root`);
//! * the cwd-relative join (`a20_vfs_path_join`), the parent/name split
//!   (`a20_vfs_path_split_parent_name`), the trailing-slash trim and the
//!   `BENEATH` containment test (`a20_vfs_path_is_beneath`).
//!
//! There is no state outside the caller's buffers and no allocation: the
//! adapter passes the `MAX_PATH_LEN`-sized buffers it already has, and the
//! `MAX_PATH_LEN` value itself is a parameter so the MCU (32/64) and full
//! (512) profiles share one engine.

#![cfg_attr(not(test), no_std)]

use core::ffi::c_int;

pub const PATH_EINVAL: c_int = -22;
pub const PATH_ENAMETOOLONG: c_int = -36;

/// Token ceiling and scratch ceiling of the pre-migration implementation.
const PARTS_MAX: usize = 64;
const SCRATCH: usize = 512;
/// `base + '/' + path` never exceeds this for the profiles that build path.c.
const CONCAT_MAX: usize = 2 * SCRATCH + 2;

#[inline]
unsafe fn slen(p: *const u8) -> usize {
    let mut n = 0usize;
    // SAFETY: callers pass NUL-terminated strings.
    while unsafe { *p.add(n) } != 0 {
        n += 1;
    }
    n
}

#[inline]
unsafe fn eq(a: *const u8, b: *const u8) -> bool {
    let mut i = 0usize;
    loop {
        // SAFETY: both are NUL-terminated.
        let (x, y) = unsafe { (*a.add(i), *b.add(i)) };
        if x != y {
            return false;
        }
        if x == 0 {
            return true;
        }
        i += 1;
    }
}

#[inline]
unsafe fn ncmp(a: *const u8, b: *const u8, n: usize) -> bool {
    for i in 0..n {
        // SAFETY: both are NUL-terminated and n is the shorter length.
        if unsafe { *a.add(i) } != unsafe { *b.add(i) } {
            return false;
        }
    }
    true
}

/// `path_is_beneath`: is `resolved` equal to or under `start`, ignoring
/// trailing slashes on `start`?  `start == "/"` contains every absolute path.
///
/// # Safety
/// Both arguments are NUL-terminated strings.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_path_is_beneath(start: *const u8, resolved: *const u8) -> c_int {
    if start.is_null() || resolved.is_null() {
        return 0;
    }
    // SAFETY: both are NUL-terminated by contract.
    unsafe {
        let mut len = slen(start);
        while len > 1 && *start.add(len - 1) == b'/' {
            len -= 1;
        }
        if len == 1 && *start == b'/' {
            return (*resolved == b'/') as c_int;
        }
        if eq(resolved, start) {
            return 1;
        }
        if ncmp(resolved, start, len) {
            let c = *resolved.add(len);
            if c == b'/' || c == 0 {
                return 1;
            }
        }
        0
    }
}

/// `vfs_path_join`: absolute `path` replaces the base; otherwise join onto
/// `cwd` (or `/`).  Truncates at `outsz - 1` like the `snprintf` it replaces.
///
/// # Safety
/// `cwd`/`path` are NUL-terminated (or null); `out` is valid for `outsz` bytes.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_path_join(
    cwd: *const u8,
    path: *const u8,
    out: *mut u8,
    outsz: usize,
) -> c_int {
    if path.is_null() || out.is_null() || outsz == 0 {
        return PATH_EINVAL;
    }
    static SLASH: [u8; 2] = [b'/', 0];
    // SAFETY: `path` is non-null and NUL-terminated.
    unsafe {
        if *path == b'/' {
            let n = core::cmp::min(slen(path), outsz - 1);
            core::ptr::copy_nonoverlapping(path, out, n);
            *out.add(n) = 0;
            return 0;
        }

        let base = if !cwd.is_null() && *cwd != 0 {
            cwd
        } else {
            SLASH.as_ptr()
        };

        // Build the concatenation, then truncate to the caller's bound.
        let mut buf = [0u8; CONCAT_MAX];
        let mut used = 0usize;
        let push = |buf: &mut [u8; CONCAT_MAX], used: &mut usize, src: *const u8, n: usize| {
            let take = core::cmp::min(n, CONCAT_MAX - 1 - *used);
            // SAFETY: `take` bytes are readable at src and fit in buf.
            core::ptr::copy_nonoverlapping(src, buf.as_mut_ptr().add(*used), take);
            *used += take;
        };

        let base_len = slen(base);
        if eq(base, SLASH.as_ptr()) {
            push(&mut buf, &mut used, SLASH.as_ptr(), 1);
        } else if base_len > 0 && *base.add(base_len - 1) == b'/' {
            push(&mut buf, &mut used, base, base_len);
        } else {
            push(&mut buf, &mut used, base, base_len);
            push(&mut buf, &mut used, SLASH.as_ptr(), 1);
        }
        push(&mut buf, &mut used, path, slen(path));

        let n = core::cmp::min(used, outsz - 1);
        core::ptr::copy_nonoverlapping(buf.as_ptr(), out, n);
        *out.add(n) = 0;
        0
    }
}

/// `vfs_path_trim_trailing_slashes`, in place.  `/` keeps its slash.
///
/// # Safety
/// `path` is a NUL-terminated mutable string, or null.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_path_trim_trailing_slashes(path: *mut u8) {
    if path.is_null() {
        return;
    }
    // SAFETY: NUL-terminated mutable buffer by contract.
    unsafe {
        let mut len = slen(path);
        while len > 1 && *path.add(len - 1) == b'/' {
            len -= 1;
            *path.add(len) = 0;
        }
    }
}

/// `vfs_path_split_parent_name`: split into the parent directory and the final
/// component.  `ENAMETOOLONG` when a bound is too small, `EINVAL` when the
/// path has no name component.
///
/// # Safety
/// `path` is NUL-terminated; the two output buffers are valid for their sizes.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_path_split_parent_name(
    path: *const u8,
    parent: *mut u8,
    parent_sz: usize,
    name: *mut u8,
    name_sz: usize,
    max_path_len: usize,
) -> c_int {
    if path.is_null() || parent.is_null() || name.is_null() || parent_sz == 0 || name_sz == 0 {
        return PATH_EINVAL;
    }
    let cap = core::cmp::min(max_path_len, SCRATCH);
    if cap == 0 {
        return PATH_EINVAL;
    }
    // SAFETY: all pointers are valid per contract.
    unsafe {
        let mut tmp = [0u8; SCRATCH];
        let n = core::cmp::min(slen(path), cap - 1);
        core::ptr::copy_nonoverlapping(path, tmp.as_mut_ptr(), n);
        tmp[n] = 0;
        // Trim trailing slashes in place (the tail is already NUL-terminated).
        let mut len = n;
        while len > 1 && tmp[len - 1] == b'/' {
            len -= 1;
            tmp[len] = 0;
        }

        let mut slash: Option<usize> = None;
        let mut i = len;
        while i > 0 {
            if tmp[i - 1] == b'/' {
                slash = Some(i - 1);
                break;
            }
            i -= 1;
        }

        let leaf_start;
        match slash {
            None => {
                *parent = 0;
                leaf_start = 0;
            }
            Some(s) => {
                leaf_start = s + 1;
                if s == 0 {
                    if parent_sz < 2 {
                        return PATH_ENAMETOOLONG;
                    }
                    *parent = b'/';
                    *parent.add(1) = 0;
                } else {
                    if s >= parent_sz {
                        return PATH_ENAMETOOLONG;
                    }
                    core::ptr::copy_nonoverlapping(tmp.as_ptr(), parent, s);
                    *parent.add(s) = 0;
                }
            }
        }

        let leaf_len = len - leaf_start;
        if leaf_len == 0 {
            return PATH_EINVAL;
        }
        if leaf_len >= name_sz {
            return PATH_ENAMETOOLONG;
        }
        core::ptr::copy_nonoverlapping(tmp.as_ptr().add(leaf_start), name, leaf_len);
        *name.add(leaf_len) = 0;
        0
    }
}

/// Shared body of the two normalisers.  `root` is null for the unfloored form.
unsafe fn normalize(path: *mut u8, root: *const u8, max_path_len: usize) -> c_int {
    if path.is_null() || unsafe { *path } != b'/' {
        return PATH_EINVAL;
    }
    let cap = core::cmp::min(max_path_len, SCRATCH);
    if cap == 0 {
        return PATH_EINVAL;
    }
    // SAFETY: all pointers are valid per the public wrappers' contracts.
    unsafe {
        let root_len = if root.is_null() {
            0usize
        } else {
            let mut l = slen(root);
            while l > 1 && *root.add(l - 1) == b'/' {
                l -= 1;
            }
            l
        };

        let mut input = [0u8; SCRATCH];
        let mut output = [0u8; SCRATCH];
        let n_in = core::cmp::min(slen(path), cap - 1);
        core::ptr::copy_nonoverlapping(path, input.as_mut_ptr(), n_in);
        input[n_in] = 0;

        let mut parts: [(usize, usize); PARTS_MAX] = [(0, 0); PARTS_MAX];
        let mut n = 0usize;

        let mut tok = 1usize;
        while tok < SCRATCH && input[tok] != 0 {
            let mut end = tok;
            while end < SCRATCH && input[end] != 0 && input[end] != b'/' {
                end += 1;
            }
            let has_slash = end < SCRATCH && input[end] == b'/';
            let len = end - tok;

            let dot = len == 1 && input[tok] == b'.';
            let dotdot = len == 2 && input[tok] == b'.' && input[tok + 1] == b'.';
            if len == 0 || dot {
                // empty component or "." -- skipped
            } else if dotdot {
                if root.is_null() {
                    if n > 0 {
                        n -= 1;
                    }
                } else {
                    // Floor at the root boundary.
                    let mut cur_len = 0usize;
                    for p in parts.iter().take(n) {
                        cur_len += p.1 + 1;
                    }
                    if cur_len > root_len {
                        n -= 1;
                    }
                }
            } else if n < PARTS_MAX {
                parts[n] = (tok, len);
                n += 1;
            } else {
                return PATH_ENAMETOOLONG;
            }

            if has_slash {
                tok = end + 1;
            } else {
                break;
            }
        }

        if n == 0 {
            *path = b'/';
            *path.add(1) = 0;
            return 0;
        }

        let mut used = 0usize;
        output[used] = b'/';
        used += 1;
        output[used] = 0;
        for i in 0..n {
            let (s, len) = parts[i];
            let sep = if i + 1 < n { 1 } else { 0 };
            if used + len + sep >= cap {
                return PATH_ENAMETOOLONG;
            }
            core::ptr::copy_nonoverlapping(input.as_ptr().add(s), output.as_mut_ptr().add(used), len);
            used += len;
            if sep == 1 {
                output[used] = b'/';
                used += 1;
            }
            output[used] = 0;
        }

        core::ptr::copy_nonoverlapping(output.as_ptr(), path, used);
        *path.add(used) = 0;
        *path.add(cap - 1) = 0;
        0
    }
}

/// `vfs_path_normalize_absolute`: collapse `.`, `..` and empty components,
/// flooring `..` at `/`.
///
/// # Safety
/// `path` is a NUL-terminated mutable buffer of at least `max_path_len` bytes.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_path_normalize_absolute(
    path: *mut u8,
    max_path_len: usize,
) -> c_int {
    // SAFETY: forwarded from the caller's contract.
    unsafe { normalize(path, core::ptr::null(), max_path_len) }
}

/// `vfs_path_normalize_absolute_with_root`: as above but `..` may not climb
/// above `root` (the chroot / `RESOLVE_IN_ROOT` floor).  A null or non-absolute
/// root falls back to the unfloored form.
///
/// # Safety
/// As `a20_vfs_path_normalize_absolute`; `root` is NUL-terminated or null.
#[no_mangle]
pub unsafe extern "C" fn a20_vfs_path_normalize_absolute_with_root(
    path: *mut u8,
    root: *const u8,
    max_path_len: usize,
) -> c_int {
    if !root.is_null() && unsafe { *root } == b'/' {
        // SAFETY: forwarded from the caller's contract.
        unsafe { normalize(path, root, max_path_len) }
    } else {
        // SAFETY: forwarded from the caller's contract.
        unsafe { normalize(path, core::ptr::null(), max_path_len) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn norm(s: &str) -> (String, i32) {
        let mut b = [0u8; SCRATCH];
        b[..s.len()].copy_from_slice(s.as_bytes());
        let rc = unsafe { a20_vfs_path_normalize_absolute(b.as_mut_ptr(), 512) };
        let n = b.iter().position(|&c| c == 0).unwrap();
        (String::from_utf8(b[..n].to_vec()).unwrap(), rc)
    }

    fn norm_root(s: &str, root: &str) -> (String, i32) {
        let mut b = [0u8; SCRATCH];
        b[..s.len()].copy_from_slice(s.as_bytes());
        let mut r = [0u8; SCRATCH];
        r[..root.len()].copy_from_slice(root.as_bytes());
        let rc = unsafe { a20_vfs_path_normalize_absolute_with_root(b.as_mut_ptr(), r.as_ptr(), 512) };
        let n = b.iter().position(|&c| c == 0).unwrap();
        (String::from_utf8(b[..n].to_vec()).unwrap(), rc)
    }

    fn join(cwd: &str, path: &str) -> String {
        let mut c = [0u8; SCRATCH];
        c[..cwd.len()].copy_from_slice(cwd.as_bytes());
        let mut p = [0u8; SCRATCH];
        p[..path.len()].copy_from_slice(path.as_bytes());
        let mut o = [0u8; SCRATCH];
        let rc = unsafe { a20_vfs_path_join(c.as_ptr(), p.as_ptr(), o.as_mut_ptr(), 512) };
        assert_eq!(rc, 0);
        let n = o.iter().position(|&x| x == 0).unwrap();
        String::from_utf8(o[..n].to_vec()).unwrap()
    }

    fn split(path: &str) -> (i32, String, String) {
        let mut p = [0u8; SCRATCH];
        p[..path.len()].copy_from_slice(path.as_bytes());
        let mut par = [0u8; SCRATCH];
        let mut nm = [0u8; SCRATCH];
        let rc = unsafe {
            a20_vfs_path_split_parent_name(p.as_ptr(), par.as_mut_ptr(), 512, nm.as_mut_ptr(), 512, 512)
        };
        let s = |b: &[u8; SCRATCH]| {
            let n = b.iter().position(|&x| x == 0).unwrap();
            String::from_utf8(b[..n].to_vec()).unwrap()
        };
        (rc, s(&par), s(&nm))
    }

    fn beneath(a: &str, b: &str) -> i32 {
        let mut x = [0u8; SCRATCH];
        x[..a.len()].copy_from_slice(a.as_bytes());
        let mut y = [0u8; SCRATCH];
        y[..b.len()].copy_from_slice(b.as_bytes());
        unsafe { a20_vfs_path_is_beneath(x.as_ptr(), y.as_ptr()) }
    }

    #[test]
    fn normalize_collapses_dots_and_slashes() {
        assert_eq!(norm("/"), ("/".into(), 0));
        assert_eq!(norm("/a//b/./c/"), ("/a/b/c".into(), 0));
        assert_eq!(norm("/a/b/../c"), ("/a/c".into(), 0));
        assert_eq!(norm("/../../.."), ("/".into(), 0));
        assert_eq!(norm("/a/.."), ("/".into(), 0));
        assert_eq!(norm("relative"), ("relative".into(), PATH_EINVAL));
    }

    #[test]
    fn normalize_with_root_floors_dotdot() {
        assert_eq!(norm_root("/a/..", "/a"), ("/a".into(), 0));
        assert_eq!(norm_root("/a/b/../../..", "/a"), ("/a".into(), 0));
        assert_eq!(norm_root("/a/b/../c", "/a"), ("/a/c".into(), 0));
        assert_eq!(norm_root("/x/y", "/a"), ("/x/y".into(), 0));
        // Non-absolute root falls back to the unfloored form.
        assert_eq!(norm_root("/a/..", "rel"), ("/".into(), 0));
    }

    #[test]
    fn normalize_reports_overflow() {
        // 64 components is the ceiling.
        let many: Vec<String> = (0..70).map(|i| format!("p{i}")).collect();
        let p = format!("/{}", many.join("/"));
        assert_eq!(norm(&p).1, PATH_ENAMETOOLONG);
        // Truncation at the caller's bound happens first, so a single
        // over-long component cannot overflow the output: the pre-migration
        // loop behaves the same way.
    }

    #[test]
    fn join_handles_absolute_and_cwd_forms() {
        assert_eq!(join("/home/user", "rel"), "/home/user/rel");
        assert_eq!(join("/", "rel"), "/rel");
        assert_eq!(join("/home/user/", "rel"), "/home/user/rel");
        assert_eq!(join("/home/user", "/abs"), "/abs");
        assert_eq!(join("", "rel"), "/rel");
    }

    #[test]
    fn split_extracts_parent_and_name() {
        assert_eq!(split("/a/b/c"), (0, "/a/b".into(), "c".into()));
        assert_eq!(split("/a"), (0, "/".into(), "a".into()));
        assert_eq!(split("/a/"), (0, "/".into(), "a".into()));
        assert_eq!(split("/"), (PATH_EINVAL, "/".into(), "".into()));
        assert_eq!(split("/a/.."), (0, "/a".into(), "..".into()));
    }

    #[test]
    fn beneath_respects_component_boundaries() {
        assert_eq!(beneath("/", "/anything"), 1);
        assert_eq!(beneath("/a", "/a"), 1);
        assert_eq!(beneath("/a", "/a/b"), 1);
        assert_eq!(beneath("/a", "/ab"), 0);
        assert_eq!(beneath("/a/", "/a/b"), 1);
        assert_eq!(beneath("/a", "/"), 0);
    }

    #[test]
    fn trim_trailing_slashes_keeps_root() {
        let mut b = [0u8; SCRATCH];
        b[..4].copy_from_slice(b"/a//");
        unsafe { a20_vfs_path_trim_trailing_slashes(b.as_mut_ptr()) };
        assert_eq!(&b[..2], b"/a");
        let mut r = [0u8; SCRATCH];
        r[..3].copy_from_slice(b"///");
        unsafe { a20_vfs_path_trim_trailing_slashes(r.as_mut_ptr()) };
        assert_eq!(&r[..1], b"/");
    }
}
