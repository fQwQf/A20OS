//! Rust implementation of the A20OS extended attribute (xattr) table.
//!
//! This is a drop-in replacement for kernel/fs/xattr.c.  It keeps the exact
//! same C ABI so callers (VFS, syscall layer) do not need to change.
//!
//! Safety policy for this module:
//! - The public functions are `extern "C"` and may receive raw pointers from
//!   C.  Every raw pointer is validated (null/length) before dereference.
//! - The global xattr table is protected by a spinlock because xattr can be
//!   accessed from syscall context on multiple CPUs once SMP is enabled.
//! - C strings are converted to byte slices using `strlen` + pointer checks;
//!   no Rust string is required.

#![no_std]
#![allow(static_mut_refs)]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(rust_2018_idioms)]

mod ffi;

use core::ffi::{c_char, c_int, c_void};
use core::ptr;
use core::slice;

use ffi::{
    proc_current, proc_has_cap, spin_lock, spin_unlock, strlen, strncpy, CAP_SYS_ADMIN,
};

pub const XATTR_NAME_MAX_LOCAL: usize = 64;
pub const XATTR_VALUE_MAX_LOCAL: usize = 512;
pub const XATTR_TABLE_MAX: usize = 1024;

pub const XATTR_CREATE: c_int = 1;
pub const XATTR_REPLACE: c_int = 2;

pub const XATTR_USER_PREFIX: &[u8] = b"user.";
pub const XATTR_SYSTEM_PREFIX: &[u8] = b"system.";
pub const XATTR_TRUSTED_PREFIX: &[u8] = b"trusted.";
pub const XATTR_SECURITY_PREFIX: &[u8] = b"security.";

/// Opaque C struct mirrored from `kernel/include/fs/vfs.h`.
#[repr(C)]
pub struct vnode_t {
    _opaque: [u8; 0],
}

impl vnode_t {
    /// Read the `(mnt, ino)` key from a valid `vnode_t`.
    ///
    /// # Safety
    /// `self` must be a valid, non-null pointer to a `vnode_t`.
    unsafe fn key(&self) -> (usize, u64) {
        let base = self as *const _ as *const u8;
        // vnode_t has `void *mnt` at offset 0 and `uint64_t ino` at offset 8
        // on all supported 64-bit architectures.
        let mnt = unsafe { ptr::read_unaligned(base as *const usize) };
        let ino = unsafe { ptr::read_unaligned(base.add(8) as *const u64) };
        (mnt, ino)
    }
}

/// One extended attribute record.
#[derive(Clone, Copy)]
struct XattrEntry {
    used: bool,
    mnt: usize,
    ino: u64,
    name: [u8; XATTR_NAME_MAX_LOCAL],
    value_len: usize,
    value: [u8; XATTR_VALUE_MAX_LOCAL],
}

impl XattrEntry {
    const fn empty() -> Self {
        Self {
            used: false,
            mnt: 0,
            ino: 0,
            name: [0; XATTR_NAME_MAX_LOCAL],
            value_len: 0,
            value: [0; XATTR_VALUE_MAX_LOCAL],
        }
    }
}

/// Global xattr table, protected by `G_XATTR_LOCK`.
static mut G_XATTRS: [XattrEntry; XATTR_TABLE_MAX] = [XattrEntry::empty(); XATTR_TABLE_MAX];

/// Spinlock serialising access to the xattr table.
///
/// # Safety
/// This is a raw C spinlock because the rest of the kernel uses C spinlocks
/// and lock-order contracts.  The lock is never held across blocking calls.
static mut G_XATTR_LOCK: ffi::spinlock_t = ffi::spinlock_t::new();

fn table_lock() {
    // Safety: lock address is valid and never moved.
    unsafe { spin_lock(ptr::addr_of_mut!(G_XATTR_LOCK)) };
}

fn table_unlock() {
    // Safety: lock address is valid and never moved.
    unsafe { spin_unlock(ptr::addr_of_mut!(G_XATTR_LOCK)) };
}

fn c_str_len(ptr: *const c_char) -> Option<usize> {
    if ptr.is_null() {
        return None;
    }
    // Safety: pointer is non-null; strlen only reads up to the NUL.
    Some(unsafe { strlen(ptr) })
}

fn c_str_to_bytes<'a>(ptr: *const c_char) -> Option<&'a [u8]> {
    let len = c_str_len(ptr)?;
    // Safety: strlen returned a bounded length; bytes are read-only.
    Some(unsafe { slice::from_raw_parts(ptr as *const u8, len) })
}

fn prefix_match(name: &[u8], prefix: &[u8]) -> bool {
    name.len() >= prefix.len() && &name[..prefix.len()] == prefix
}

fn xattr_check_name(name: *const c_char) -> c_int {
    let bytes = match c_str_to_bytes(name) {
        Some(b) if !b.is_empty() => b,
        _ => return -ffi::EINVAL,
    };
    if bytes.len() >= XATTR_NAME_MAX_LOCAL {
        return -ffi::ERANGE;
    }
    0
}

#[no_mangle]
pub extern "C" fn xattr_check_namespace(name: *const c_char, needs_cap: *mut c_int) -> c_int {
    let r = xattr_check_name(name);
    if r < 0 {
        return r;
    }
    if !needs_cap.is_null() {
        // Safety: caller promised a valid `needs_cap` when non-null.
        unsafe { ptr::write(needs_cap, 0) };
    }
    let bytes = match c_str_to_bytes(name) {
        Some(b) => b,
        _ => return -ffi::EINVAL,
    };
    if prefix_match(bytes, XATTR_USER_PREFIX) || prefix_match(bytes, XATTR_SYSTEM_PREFIX) {
        return 0;
    }
    if prefix_match(bytes, XATTR_TRUSTED_PREFIX) || prefix_match(bytes, XATTR_SECURITY_PREFIX) {
        if !needs_cap.is_null() {
            unsafe { ptr::write(needs_cap, 1) };
        }
        return 0;
    }
    -ffi::EINVAL
}

fn check_cap_if_needed(needs_cap: c_int) -> c_int {
    if needs_cap == 0 {
        return 0;
    }
    // Safety: FFI calls with valid constants.
    let cur = unsafe { proc_current() };
    if cur.is_null() || unsafe { proc_has_cap(cur, CAP_SYS_ADMIN) } == 0 {
        return -ffi::EPERM;
    }
    0
}

unsafe fn find_entry(mnt: usize, ino: u64, name: &[u8]) -> Option<usize> {
    // Safety: caller must hold G_XATTR_LOCK.
    unsafe {
        for (i, e) in G_XATTRS.iter().enumerate() {
            if e.used && e.mnt == mnt && e.ino == ino && e.name_prefix_matches(name) {
                return Some(i);
            }
        }
        None
    }
}

unsafe fn find_free_slot() -> c_int {
    // Safety: caller must hold G_XATTR_LOCK.
    unsafe {
        for (i, e) in G_XATTRS.iter().enumerate() {
            if !e.used {
                return i as c_int;
            }
        }
        -ffi::ENOSPC
    }
}

#[no_mangle]
pub unsafe extern "C" fn xattr_set_vnode(
    vn: *mut vnode_t,
    name: *const c_char,
    value: *const c_void,
    size: usize,
    flags: c_int,
) -> i64 {
    if vn.is_null() {
        return -ffi::ENOENT as i64;
    }
    if flags & !(XATTR_CREATE | XATTR_REPLACE) != 0 {
        return -ffi::EINVAL as i64;
    }

    let mut needs_cap: c_int = 0;
    let nr = xattr_check_namespace(name, &mut needs_cap);
    if nr < 0 {
        return nr as i64;
    }

    let name_bytes = match c_str_to_bytes(name) {
        Some(b) => b,
        _ => return -ffi::EINVAL as i64,
    };

    // Validate capability before taking the lock to keep the lock hold short.
    if check_cap_if_needed(needs_cap) < 0 {
        return -ffi::EPERM as i64;
    }

    if size > XATTR_VALUE_MAX_LOCAL {
        return -ffi::ENOSPC as i64;
    }
    if size != 0 && value.is_null() {
        return -ffi::EINVAL as i64;
    }

    let (mnt, ino) = unsafe { (*vn).key() };
    let value_slice = if size == 0 {
        &[][..]
    } else {
        // Safety: value is non-null and size is bounded.
        unsafe { slice::from_raw_parts(value as *const u8, size) }
    };

    table_lock();
    // Safety: lock held.
    let idx = unsafe { find_entry(mnt, ino, name_bytes) };

    if flags & XATTR_CREATE != 0 && idx.is_some() {
        table_unlock();
        return -ffi::EEXIST as i64;
    }
    if flags & XATTR_REPLACE != 0 && idx.is_none() {
        table_unlock();
        return -ffi::ENODATA as i64;
    }

    let idx = match idx {
        Some(i) => i,
        None => {
            // Safety: lock held.
            let slot = unsafe { find_free_slot() };
            if slot < 0 {
                table_unlock();
                return slot as i64;
            }
            let slot = slot as usize;
            unsafe {
                G_XATTRS[slot] = XattrEntry::empty();
                G_XATTRS[slot].used = true;
                G_XATTRS[slot].mnt = mnt;
                G_XATTRS[slot].ino = ino;
                let _ = strncpy(
                    G_XATTRS[slot].name.as_mut_ptr() as *mut c_char,
                    name,
                    XATTR_NAME_MAX_LOCAL - 1,
                );
            }
            slot
        }
    };

    unsafe {
        if size != 0 {
            G_XATTRS[idx].value[..size].copy_from_slice(value_slice);
        }
        G_XATTRS[idx].value_len = size;
    }
    table_unlock();
    0
}

#[no_mangle]
pub unsafe extern "C" fn xattr_get_vnode(
    vn: *mut vnode_t,
    name: *const c_char,
    value: *mut c_void,
    size: usize,
) -> i64 {
    if vn.is_null() {
        return -ffi::ENOENT as i64;
    }
    let nr = xattr_check_namespace(name, ptr::null_mut());
    if nr < 0 {
        return nr as i64;
    }
    let name_bytes = match c_str_to_bytes(name) {
        Some(b) => b,
        _ => return -ffi::EINVAL as i64,
    };

    let (mnt, ino) = unsafe { (*vn).key() };

    table_lock();
    // Safety: lock held.
    let idx = match unsafe { find_entry(mnt, ino, name_bytes) } {
        Some(i) => i,
        None => {
            table_unlock();
            return -ffi::ENODATA as i64;
        }
    };
    let entry_len = unsafe { G_XATTRS[idx].value_len };
    if value.is_null() || size == 0 {
        table_unlock();
        return entry_len as i64;
    }
    if size < entry_len {
        table_unlock();
        return -ffi::ERANGE as i64;
    }
    unsafe {
        let dst = slice::from_raw_parts_mut(value as *mut u8, entry_len);
        dst.copy_from_slice(&G_XATTRS[idx].value[..entry_len]);
    }
    table_unlock();
    entry_len as i64
}

#[no_mangle]
pub unsafe extern "C" fn xattr_list_vnode(vn: *mut vnode_t, list: *mut c_char, size: usize) -> i64 {
    if vn.is_null() {
        return -ffi::ENOENT as i64;
    }
    let (mnt, ino) = unsafe { (*vn).key() };

    table_lock();
    // Safety: lock held.
    let mut total = 0usize;
    for e in unsafe { G_XATTRS.iter() } {
        if e.used && e.mnt == mnt && e.ino == ino {
            total += e.name_len() + 1;
        }
    }
    if list.is_null() || size == 0 {
        table_unlock();
        return total as i64;
    }
    if size < total {
        table_unlock();
        return -ffi::ERANGE as i64;
    }

    let mut off = 0usize;
    for e in unsafe { G_XATTRS.iter() } {
        if e.used && e.mnt == mnt && e.ino == ino {
            let len = e.name_len() + 1;
            unsafe {
                let dst = slice::from_raw_parts_mut(list.add(off) as *mut u8, len);
                dst[..len - 1].copy_from_slice(&e.name[..len - 1]);
                dst[len - 1] = 0;
            }
            off += len;
        }
    }
    table_unlock();
    total as i64
}

#[no_mangle]
pub unsafe extern "C" fn xattr_remove_vnode(vn: *mut vnode_t, name: *const c_char) -> i64 {
    if vn.is_null() {
        return -ffi::ENOENT as i64;
    }
    let mut needs_cap: c_int = 0;
    let nr = xattr_check_namespace(name, &mut needs_cap);
    if nr < 0 {
        return nr as i64;
    }
    let name_bytes = match c_str_to_bytes(name) {
        Some(b) => b,
        _ => return -ffi::EINVAL as i64,
    };
    if check_cap_if_needed(needs_cap) < 0 {
        return -ffi::EPERM as i64;
    }
    let (mnt, ino) = unsafe { (*vn).key() };

    table_lock();
    // Safety: lock held.
    let idx = match unsafe { find_entry(mnt, ino, name_bytes) } {
        Some(i) => i,
        None => {
            table_unlock();
            return -ffi::ENODATA as i64;
        }
    };
    unsafe {
        G_XATTRS[idx] = XattrEntry::empty();
    }
    table_unlock();
    0
}

#[no_mangle]
pub unsafe extern "C" fn xattr_cleanup_vnode(vn: *mut vnode_t) {
    if vn.is_null() {
        return;
    }
    let (mnt, ino) = unsafe { (*vn).key() };

    table_lock();
    for e in unsafe { G_XATTRS.iter_mut() } {
        if e.used && e.mnt == mnt && e.ino == ino {
            *e = XattrEntry::empty();
        }
    }
    table_unlock();
}

impl XattrEntry {
    fn name_len(&self) -> usize {
        // Names are NUL-terminated by strncpy; scan for first NUL.
        self.name
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(XATTR_NAME_MAX_LOCAL)
    }

    fn name_prefix_matches(&self, prefix: &[u8]) -> bool {
        let len = self.name_len();
        if prefix.len() > len {
            return false;
        }
        &self.name[..prefix.len()] == prefix
    }
}
