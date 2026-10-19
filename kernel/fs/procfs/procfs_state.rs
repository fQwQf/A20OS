//! procfs read/seek offset state and directory-cursor state engine.
//!
//! `kernel/fs/procfs/procfs.c` keeps the vnode/priv plumbing, the `readdir`
//! buffer, the `fdtable`/tasklist lookups and the `memcpy`/`snprintf` bridges.
//! This module owns the state those bridges operate on:
//!
//! * the **read offset** — where a `read(2)` on a rendered procfs file starts,
//!   how many bytes a short read consumes, and the EOF transition;
//! * the **seek arithmetic** — `SEEK_SET`/`SEEK_CUR`/`SEEK_END` against the
//!   file's rendered length, with the clamp that keeps the offset non-negative;
//! * the **directory cursor** — the state a `getdents64(2)` caller resumes from,
//!   including the `.`/`..` seeds, the "skip to the next occupied slot" advance
//!   (`fd`/`tid` + 3), the per-entry `d_ino`/`d_off`/`d_reclen`/`d_type` layout
//!   decision and the budget test that decides where a batch stops.
//!
//! The adapter answers only "what entry sits at this cursor" (a lookup); every
//! ordering and layout decision above lives here, so a `getdents64` batch is
//! emitted by Rust even though the bytes are produced into a C buffer.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};

/// `vfs_dirent64_t` is `__attribute__((packed))`: `d_ino` (8), `d_off` (8),
/// `d_reclen` (2), `d_type` (1), then `d_name`.  `offsetof(d_name)` is 19 and
/// `sizeof` (with the `d_name[1]` member) is 20; the two are deliberately not
/// the same, and the pre-migration code used each in a different variant, so
/// both constants are carried here.
pub const DIRENT_NAME_OFF: usize = 19;
pub const DIRENT_SIZE: usize = 20;

const DT_DIR: u8 = 4;
const DT_REG: u8 = 8;
const DT_LNK: u8 = 10;

/// `EINVAL` / `EBADF` as returned by the offset entry points.
pub const PROCFS_EINVAL: i64 = -22;

/// Which directory a cursor belongs to.  The variant selects the advance rule
/// (`+3` over a sparse key space vs `+1` over an index space) and the per-entry
/// `d_ino`/`d_off`/`d_type` policy.
pub const PROCFS_DIR_FD: u32 = 0;
pub const PROCFS_DIR_FDINFO: u32 = 1;
pub const PROCFS_DIR_TASK: u32 = 2;
pub const PROCFS_DIR_STATIC: u32 = 3;

/// Rust mirror of `procfs_dir_entry_t` (see `fs/procfs_state.h`).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcfsDirEntry {
    /// Sparse key the cursor advances from (`fd` number, or raw `pid`).
    pub key: u64,
    /// Id rendered into `d_name`/`d_ino` when `name` is null.
    pub shown: i64,
    /// Literal NUL-terminated name, or null to format `shown` in decimal.
    pub name: *const u8,
    /// `DT_*` the lookup already knows (static entries); 0 lets the engine pick.
    pub type_hint: u8,
}

impl ProcfsDirEntry {
    const EMPTY: ProcfsDirEntry = ProcfsDirEntry {
        key: 0,
        shown: 0,
        name: core::ptr::null(),
        type_hint: 0,
    };
}

/// `int (*)(void *ctx, uint64_t cursor, procfs_dir_entry_t *out)`.
///
/// Returns 1 when an entry sits at `cursor`, 0 at end of directory, or a
/// negative errno.  Called only for cursors past the `.`/`..` seed in the
/// sparse variants, and for every cursor in the index variant.
pub type ProcfsDirNext =
    unsafe extern "C" fn(ctx: *mut c_void, cursor: u64, out: *mut ProcfsDirEntry) -> c_int;

#[inline]
fn decimal(mut v: u64, buf: &mut [u8; 24]) -> usize {
    if v == 0 {
        buf[0] = b'0';
        return 1;
    }
    let mut tmp = [0u8; 20];
    let mut n = 0usize;
    while v > 0 {
        tmp[n] = b'0' + (v % 10) as u8;
        v /= 10;
        n += 1;
    }
    for i in 0..n {
        buf[i] = tmp[n - 1 - i];
    }
    n
}

#[inline]
unsafe fn c_strlen(p: *const u8) -> usize {
    let mut n = 0usize;
    // SAFETY: the adapter hands over a NUL-terminated string.
    while unsafe { *p.add(n) } != 0 {
        n += 1;
    }
    n
}

#[inline]
fn dirent_reclen(base: usize, namelen: usize) -> usize {
    (base + namelen + 1 + 7) & !7usize
}

#[inline]
unsafe fn emit_dirent(
    at: *mut u8,
    d_ino: u64,
    d_off: i64,
    reclen: usize,
    d_type: u8,
    name: &[u8],
) {
    // SAFETY: the adapter guarantees `at` has room for `reclen` bytes.
    unsafe {
        core::ptr::write_unaligned(at as *mut u64, d_ino);
        core::ptr::write_unaligned(at.add(8) as *mut i64, d_off);
        core::ptr::write_unaligned(at.add(16) as *mut u16, reclen as u16);
        *at.add(18) = d_type;
        core::ptr::copy_nonoverlapping(name.as_ptr(), at.add(DIRENT_NAME_OFF), name.len());
        *at.add(DIRENT_NAME_OFF + name.len()) = 0;
    }
}

/// Emit one `getdents64` batch for a directory cursor.
///
/// Owns the loop: it decides which entry comes next (`find` supplies the
/// identity, the `.`/`..` seed is generated here), lays out the record, tests
/// it against the remaining byte budget, advances the cursor and stops when the
/// buffer is full or the directory ends.  Writes the resumed cursor to
/// `out_cursor` and returns the bytes emitted (or a negative errno).
///
/// # Safety
/// `out` must be valid for `count` bytes; `next` must be a live callback and
/// `ctx` whatever it expects; `out_cursor` must be non-null and writable.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_readdir_plan(
    mode: u32,
    start: u64,
    count: usize,
    ctx: *mut c_void,
    next: Option<ProcfsDirNext>,
    out: *mut u8,
    out_cursor: *mut u64,
) -> i64 {
    let index_mode = mode == PROCFS_DIR_STATIC;
    let base = if index_mode { DIRENT_SIZE } else { DIRENT_NAME_OFF };
    let mut cursor = start;
    let mut total = 0usize;
    let mut name = [0u8; 24];

    while total < count {
        let mut e = ProcfsDirEntry::EMPTY;
        let namelen: usize;
        let d_ino: u64;
        let d_type: u8;
        let next_cursor: u64;

        if !index_mode && cursor < 2 {
            // The two seed entries are pure cursor state, not lookups.
            name[0] = b'.';
            namelen = if cursor == 0 {
                1
            } else {
                name[1] = b'.';
                2
            };
            d_ino = cursor + 1;
            d_type = DT_DIR;
            next_cursor = cursor + 1;
        } else {
            let find = match next {
                Some(f) => f,
                None => break,
            };
            // SAFETY: `find` is the adapter's lookup for this directory and
            // `ctx` is whatever it was created with.
            let r = unsafe { find(ctx, cursor, &mut e) };
            if r < 0 {
                // SAFETY: out_cursor is non-null by contract.
                unsafe { *out_cursor = cursor };
                return r as i64;
            }
            if r == 0 {
                break;
            }
            if e.name.is_null() {
                namelen = decimal(e.shown.max(0) as u64, &mut name);
            } else {
                // SAFETY: the adapter's lookup returns a NUL-terminated name.
                namelen = unsafe { c_strlen(e.name) };
                if namelen > name.len() {
                    // SAFETY: out_cursor is non-null by contract.
                    unsafe { *out_cursor = cursor };
                    return -36; // -ENAMETOOLONG
                }
                // SAFETY: namelen bytes are readable at e.name.
                unsafe { core::ptr::copy_nonoverlapping(e.name, name.as_mut_ptr(), namelen) };
            }

            if index_mode {
                d_ino = cursor;
                d_type = if e.type_hint == 0 { DT_REG } else { e.type_hint };
                next_cursor = cursor + 1;
            } else if mode == PROCFS_DIR_TASK {
                d_ino = if e.shown > 0 { e.shown as u64 } else { cursor + 1 };
                d_type = DT_DIR;
                next_cursor = e.key + 3;
            } else {
                d_ino = e.key + 1;
                d_type = if mode == PROCFS_DIR_FD { DT_LNK } else { DT_REG };
                next_cursor = e.key + 3;
            }
        }

        let reclen = dirent_reclen(base, namelen);
        if total + reclen > count {
            break;
        }
        // The sparse variants resume from the cursor; the index variant reports
        // a byte offset, matching the two pre-migration loops.
        let d_off = if index_mode {
            (total + reclen) as i64
        } else {
            next_cursor as i64
        };
        // SAFETY: `total + reclen <= count`, so the record fits in `out`.
        unsafe { emit_dirent(out.add(total), d_ino, d_off, reclen, d_type, &name[..namelen]) };
        total += reclen;
        cursor = next_cursor;
    }

    // SAFETY: out_cursor is non-null by contract.
    unsafe { *out_cursor = cursor };
    total as i64
}

/// `lseek(2)` arithmetic against a rendered procfs file.
///
/// Returns the new offset (clamped at 0) or `-EINVAL` for an unknown `whence`.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_seek_plan(
    content_len: u64,
    cur: u64,
    off: i64,
    whence: c_int,
) -> i64 {
    let new_off: i64 = match whence {
        0 => off,                          // SEEK_SET
        1 => cur as i64 + off,             // SEEK_CUR
        2 => content_len as i64 + off,     // SEEK_END
        _ => return PROCFS_EINVAL,
    };
    if new_off < 0 {
        0
    } else {
        new_off
    }
}

/// Decide how many bytes a `read(2)` consumes from the buffered content path
/// and where the offset lands.  Returns the byte count (0 at or past EOF) and
/// writes the resumed offset through `out_offset`.
///
/// # Safety
/// `out_offset` must be non-null and writable.
#[no_mangle]
pub unsafe extern "C" fn a20_procfs_read_plan(
    content_len: u64,
    offset: u64,
    count: usize,
    out_offset: *mut u64,
) -> i64 {
    if offset >= content_len {
        // SAFETY: out_offset is non-null by contract.
        unsafe { *out_offset = offset };
        return 0;
    }
    let avail = content_len - offset;
    let n = if (count as u64) < avail {
        count as u64
    } else {
        avail
    };
    // SAFETY: out_offset is non-null by contract.
    unsafe { *out_offset = offset + n };
    n as i64
}

#[cfg(test)]
mod tests {
    use super::*;

    const SEEKSET: c_int = 0;
    const SEEKCUR: c_int = 1;
    const SEEKEND: c_int = 2;

    #[test]
    fn dirent_layout_matches_c() {
        assert_eq!(DIRENT_NAME_OFF, 19);
        assert_eq!(DIRENT_SIZE, 20);
        assert_eq!(dirent_reclen(DIRENT_NAME_OFF, 1), 24); // ".", fd/task
        assert_eq!(dirent_reclen(DIRENT_NAME_OFF, 2), 24); // ".."
        assert_eq!(dirent_reclen(DIRENT_SIZE, 1), 24); // ".", index
        assert_eq!(dirent_reclen(DIRENT_NAME_OFF, 10), 32); // 19+11 -> 32
    }

    #[test]
    fn seek_clamps_and_rejects() {
        unsafe {
            assert_eq!(a20_procfs_seek_plan(100, 10, 5, SEEKSET), 5);
            assert_eq!(a20_procfs_seek_plan(100, 10, 5, SEEKCUR), 15);
            assert_eq!(a20_procfs_seek_plan(100, 10, -5, SEEKEND), 95);
            assert_eq!(a20_procfs_seek_plan(100, 10, -100, SEEKSET), 0);
            assert_eq!(a20_procfs_seek_plan(100, 10, -100, SEEKCUR), 0);
            assert_eq!(a20_procfs_seek_plan(100, 10, 0, 9), PROCFS_EINVAL);
        }
    }

    #[test]
    fn read_plan_short_reads_and_eof() {
        unsafe {
            let mut off = 0u64;
            assert_eq!(a20_procfs_read_plan(10, 0, 4, &mut off), 4);
            assert_eq!(off, 4);
            // Short read: only the tail is available.
            assert_eq!(a20_procfs_read_plan(10, 8, 100, &mut off), 2);
            assert_eq!(off, 10);
            // EOF is sticky and does not move the offset.
            assert_eq!(a20_procfs_read_plan(10, 10, 100, &mut off), 0);
            assert_eq!(off, 10);
            assert_eq!(a20_procfs_read_plan(10, 99, 100, &mut off), 0);
            assert_eq!(off, 99);
        }
    }

    /// A directory of occupied fd slots {3, 7} in fd mode.
    unsafe extern "C" fn fd_next(_ctx: *mut c_void, cursor: u64, out: *mut ProcfsDirEntry) -> c_int {
        let mut fd = cursor as i64 - 2;
        while fd < 10 {
            if fd == 3 || fd == 7 {
                break;
            }
            fd += 1;
        }
        if fd >= 10 {
            return 0;
        }
        unsafe {
            (*out).key = fd as u64;
            (*out).shown = fd;
            (*out).name = core::ptr::null();
            (*out).type_hint = 0;
        }
        1
    }

    #[test]
    fn readdir_seeds_dots_and_advances_over_sparse_keys() {
        unsafe {
            let mut buf = [0u8; 256];
            let mut cursor = 0u64;
            let n = a20_procfs_readdir_plan(
                PROCFS_DIR_FD,
                0,
                buf.len(),
                core::ptr::null_mut(),
                Some(fd_next),
                buf.as_mut_ptr(),
                &mut cursor,
            );
            assert!(n > 0);
            // Four records: "." ".." "3" "7"; cursor resumes past fd 7.
            assert_eq!(cursor, 10);
            // d_type: dots are DT_DIR, fd slots are DT_LNK.
            assert_eq!(buf[18], DT_DIR);
            assert_eq!(buf[24 + 18], DT_DIR);
            let third = dirent_reclen(DIRENT_NAME_OFF, 1);
            assert_eq!(buf[2 * third + 18], DT_LNK);
            assert_eq!(*buf.as_ptr().add(2 * third + DIRENT_NAME_OFF), b'3');
            // d_ino for fd 3 is fd+1.
            assert_eq!(core::ptr::read_unaligned(buf.as_ptr().add(2 * third) as *const u64), 4);
            assert_eq!(core::ptr::read_unaligned(buf.as_ptr().add(3 * third) as *const u64), 8);
        }
    }

    #[test]
    fn readdir_stops_at_budget_and_resumes() {
        unsafe {
            let mut buf = [0u8; 48]; // exactly two 24-byte records
            let mut cursor = 0u64;
            let n = a20_procfs_readdir_plan(
                PROCFS_DIR_FD,
                0,
                buf.len(),
                core::ptr::null_mut(),
                Some(fd_next),
                buf.as_mut_ptr(),
                &mut cursor,
            );
            assert_eq!(n, 48);
            assert_eq!(cursor, 2); // stopped after "..", nothing else fit
            let n2 = a20_procfs_readdir_plan(
                PROCFS_DIR_FD,
                cursor,
                buf.len(),
                core::ptr::null_mut(),
                Some(fd_next),
                buf.as_mut_ptr(),
                &mut cursor,
            );
            assert_eq!(n2, 48);
            assert_eq!(cursor, 10);
        }
    }

    #[test]
    fn readdir_index_mode_uses_byte_offsets() {
        unsafe extern "C" fn idx_next(
            ctx: *mut c_void,
            cursor: u64,
            out: *mut ProcfsDirEntry,
        ) -> c_int {
            let names = ctx as *const *const u8;
            let p = *names.add(cursor as usize);
            if p.is_null() {
                return 0;
            }
            unsafe {
                (*out).name = p;
                (*out).type_hint = if cursor <= 1 { DT_DIR } else { DT_REG };
            }
            1
        }
        static DOT: &[u8] = b".\0";
        static DOTDOT: &[u8] = b"..\0";
        static VER: &[u8] = b"version\0";
        let names: [*const u8; 4] = [
            DOT.as_ptr(),
            DOTDOT.as_ptr(),
            VER.as_ptr(),
            core::ptr::null(),
        ];
        unsafe {
            let mut buf = [0u8; 256];
            let mut cursor = 0u64;
            let n = a20_procfs_readdir_plan(
                PROCFS_DIR_STATIC,
                0,
                buf.len(),
                names.as_ptr() as *mut c_void,
                Some(idx_next),
                buf.as_mut_ptr(),
                &mut cursor,
            );
            assert_eq!(cursor, 3);
            assert_eq!(n, 24 + 24 + 32);
            // d_ino is the index and d_off the byte offset in this variant.
            assert_eq!(core::ptr::read_unaligned(buf.as_ptr() as *const u64), 0);
            assert_eq!(core::ptr::read_unaligned(buf.as_ptr().add(8) as *const i64), 24);
            assert_eq!(core::ptr::read_unaligned(buf.as_ptr().add(24) as *const u64), 1);
            assert_eq!(core::ptr::read_unaligned(buf.as_ptr().add(32) as *const i64), 48);
            assert_eq!(core::ptr::read_unaligned(buf.as_ptr().add(48) as *const u64), 2);
            assert_eq!(core::ptr::read_unaligned(buf.as_ptr().add(56) as *const i64), 80);
        }
    }
}
