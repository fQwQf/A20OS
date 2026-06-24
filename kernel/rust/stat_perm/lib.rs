#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use core::ffi::{c_int, c_void};
use core::ptr;

use a20rust_support::lock::IrqSaveSpinLock;

use ffi::{
    kstat_t, vfs_cred_t, CAP_DAC_OVERRIDE, CAP_DAC_READ_SEARCH, CAP_FOWNER, EACCES, EINVAL,
    ENOSPC, EPERM, F_OK, LINUX_UTIME_NOW, LINUX_UTIME_OMIT, MAX_GROUPS, R_OK, S_IFCHR, S_IFIFO,
    S_IFMT, S_IFREG, S_ISVTX, S_IXGRP, S_IXOTH, S_IXUSR, VFS_TIME_META_MAX, W_OK, X_OK,
};

/// Opaque `vnode_t`. Field access is only through C helpers.
#[repr(C)]
pub struct vnode_t {
    _opaque: [u8; 0],
}

impl vnode_t {
    unsafe fn key(&self) -> (usize, u64) {
        let mut mnt: *mut c_void = ptr::null_mut();
        let mut ino: u64 = 0;
        ffi::a20_vnode_key(self as *const _ as *mut _, &mut mnt, &mut ino);
        (mnt as usize, ino)
    }
}

#[derive(Clone, Copy)]
struct TimeMeta {
    mnt: usize,
    ino: u64,
    atime: u64,
    atime_nsec: u64,
    mtime: u64,
    mtime_nsec: u64,
    ctime: u64,
    ctime_nsec: u64,
}

impl TimeMeta {
    const fn new(mnt: usize, ino: u64) -> Self {
        Self {
            mnt,
            ino,
            atime: 0,
            atime_nsec: 0,
            mtime: 0,
            mtime_nsec: 0,
            ctime: 0,
            ctime_nsec: 0,
        }
    }
}

static TIME_META_TABLE: IrqSaveSpinLock<[Option<TimeMeta>; VFS_TIME_META_MAX]> =
    IrqSaveSpinLock::new([None; VFS_TIME_META_MAX]);

fn now_realtime() -> (u64, u64) {
    let mut now = [0u64; 2];
    unsafe { ffi::a20_timekeeping_get_realtime(now.as_mut_ptr()) };
    (now[0], now[1])
}

fn with_time_meta<F, R>(vn: &vnode_t, f: F) -> Option<R>
where
    F: FnOnce(&mut TimeMeta) -> R,
{
    let (mnt, ino) = unsafe { vn.key() };
    let mut guard = TIME_META_TABLE.lock();
    for slot in guard.iter_mut() {
        if let Some(e) = slot {
            if e.mnt == mnt && e.ino == ino {
                return Some(f(e));
            }
        }
    }
    for slot in guard.iter_mut() {
        if slot.is_none() {
            *slot = Some(TimeMeta::new(mnt, ino));
            return Some(f(slot.as_mut().unwrap()));
        }
    }
    None
}

fn task_cred(t: *mut c_void) -> vfs_cred_t {
    let mut cred: vfs_cred_t = unsafe { core::mem::zeroed() };
    if !t.is_null() {
        unsafe { ffi::a20_proc_get_cred(t, &mut cred) };
    }
    cred
}

fn ids_in_group(t: *mut c_void, primary_gid: u32, gid: u32) -> bool {
    if primary_gid == gid {
        return true;
    }
    if t.is_null() {
        return gid == 0;
    }
    let cred = task_cred(t);
    let n = core::cmp::min(cred.ngroups as usize, MAX_GROUPS);
    for i in 0..n {
        if cred.groups[i] as u32 == gid {
            return true;
        }
    }
    false
}

#[no_mangle]
pub unsafe extern "C" fn fill_char_kstat(st: *mut kstat_t) {
    if st.is_null() {
        return;
    }
    unsafe {
        ptr::write_bytes(st as *mut u8, 0, core::mem::size_of::<kstat_t>());
        (*st).st_mode = S_IFCHR | 0o666;
        (*st).st_uid = 0;
        (*st).st_gid = 0;
        (*st).st_nlink = 1;
        (*st).st_blksize = 4096;
    }
}

#[no_mangle]
pub unsafe extern "C" fn fill_pipe_kstat(st: *mut kstat_t) {
    if st.is_null() {
        return;
    }
    let cur = unsafe { ffi::a20_proc_current() };
    let cred = task_cred(cur);
    unsafe {
        ptr::write_bytes(st as *mut u8, 0, core::mem::size_of::<kstat_t>());
        (*st).st_mode = S_IFIFO | 0o600;
        (*st).st_uid = if cur.is_null() { 0 } else { cred.uid as u32 };
        (*st).st_gid = if cur.is_null() { 0 } else { cred.gid as u32 };
        (*st).st_nlink = 1;
        (*st).st_blksize = 4096;
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_task_in_group(t: *mut c_void, gid: u32) -> c_int {
    if t.is_null() {
        return (gid == 0) as c_int;
    }
    let cred = task_cred(t);
    if (cred.fsgid as u32) == gid || (cred.egid as u32) == gid || (cred.gid as u32) == gid {
        return 1;
    }
    let n = core::cmp::min(cred.ngroups as usize, MAX_GROUPS);
    for i in 0..n {
        if cred.groups[i] as u32 == gid {
            return 1;
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn vfs_mode_has_perm_ids(
    st_mode: u32,
    file_uid: u32,
    file_gid: u32,
    uid: u32,
    gid: u32,
    mask: c_int,
) -> c_int {
    if mask == F_OK {
        return 0;
    }

    let cur = unsafe { ffi::a20_proc_current() };

    if unsafe { ffi::a20_proc_has_cap(cur, CAP_DAC_OVERRIDE) } != 0 {
        if (mask & X_OK) != 0
            && (st_mode & S_IFMT) == S_IFREG
            && (st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0
        {
            return -EACCES;
        }
        return 0;
    }

    if (mask & W_OK) == 0 && unsafe { ffi::a20_proc_has_cap(cur, CAP_DAC_READ_SEARCH) } != 0 {
        if (mask & X_OK) != 0
            && (st_mode & S_IFMT) == S_IFREG
            && (st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0
        {
            return -EACCES;
        }
        return 0;
    }

    let mut shift = 0;
    if uid == file_uid {
        shift = 6;
    } else if ids_in_group(cur, gid, file_gid) {
        shift = 3;
    }

    let mut need = 0;
    if (mask & R_OK) != 0 {
        need |= 4;
    }
    if (mask & W_OK) != 0 {
        need |= 2;
    }
    if (mask & X_OK) != 0 {
        need |= 1;
    }

    if ((st_mode >> shift) & need) == need {
        0
    } else {
        -EACCES
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_mode_has_perm_ids_nocap(
    st_mode: u32,
    _file_uid: u32,
    file_gid: u32,
    uid: u32,
    gid: u32,
    mask: c_int,
) -> c_int {
    if mask == F_OK {
        return 0;
    }

    let mut shift = 0;
    if uid == _file_uid {
        shift = 6;
    } else if uid == 0 || gid == file_gid {
        shift = 3;
    }

    let mut need = 0;
    if (mask & R_OK) != 0 {
        need |= 4;
    }
    if (mask & W_OK) != 0 {
        need |= 2;
    }
    if (mask & X_OK) != 0 {
        need |= 1;
    }

    if uid == 0 && (mask & X_OK) == 0 {
        return 0;
    }
    if uid == 0 && (mask & X_OK) != 0 {
        if (st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0 {
            return 0;
        }
        return -EACCES;
    }

    if ((st_mode >> shift) & need) == need {
        0
    } else {
        -EACCES
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_vnode_stat(vn: *mut vnode_t, st: *mut kstat_t) -> c_int {
    if vn.is_null() || st.is_null() {
        return -EINVAL;
    }

    let r = unsafe { ffi::a20_vnode_stat_op(vn as *mut c_void, st) };
    if r < 0 {
        return r;
    }

    let vn_ref = unsafe { &*vn };
    let mut found = false;
    {
        let guard = TIME_META_TABLE.lock();
        let (mnt, ino) = unsafe { vn_ref.key() };
        for slot in guard.iter() {
            if let Some(e) = slot {
                if e.mnt == mnt && e.ino == ino {
                    unsafe {
                        (*st).st_atime = e.atime;
                        (*st).st_atime_nsec = e.atime_nsec;
                        (*st).st_mtime = e.mtime;
                        (*st).st_mtime_nsec = e.mtime_nsec;
                        (*st).st_ctime = e.ctime;
                        (*st).st_ctime_nsec = e.ctime_nsec;
                    }
                    found = true;
                    break;
                }
            }
        }
    }

    if !found {
        unsafe {
            if (*st).st_atime == 0 && (*st).st_mtime == 0 && (*st).st_ctime == 0 {
                let (sec, nsec) = now_realtime();
                (*st).st_atime = sec;
                (*st).st_atime_nsec = nsec;
                (*st).st_mtime = sec;
                (*st).st_mtime_nsec = nsec;
                (*st).st_ctime = sec;
                (*st).st_ctime_nsec = nsec;
            }
        }
    }

    0
}

#[no_mangle]
pub unsafe extern "C" fn vfs_vnode_permission(vn: *mut vnode_t, mask: c_int) -> c_int {
    if vn.is_null() {
        return -EINVAL;
    }
    let mut st: kstat_t = unsafe { core::mem::zeroed() };
    let r = unsafe { vfs_vnode_stat(vn, &mut st) };
    if r < 0 {
        return r;
    }
    unsafe { vfs_mode_has_perm(st.st_mode, st.st_uid, st.st_gid, mask) }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_mode_has_perm(st_mode: u32, uid: u32, gid: u32, mask: c_int) -> c_int {
    let cur = unsafe { ffi::a20_proc_current() };
    let cred = task_cred(cur);
    let fsuid = if cur.is_null() { 0 } else { cred.fsuid as u32 };
    let fsgid = if cur.is_null() { 0 } else { cred.fsgid as u32 };
    unsafe { vfs_mode_has_perm_ids(st_mode, uid, gid, fsuid, fsgid, mask) }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_current_owns(vn: *mut vnode_t) -> c_int {
    if vn.is_null() {
        return 0;
    }
    let cur = unsafe { ffi::a20_proc_current() };
    if unsafe { ffi::a20_proc_has_cap(cur, CAP_FOWNER) } != 0 {
        return 1;
    }
    let mut st: kstat_t = unsafe { core::mem::zeroed() };
    if unsafe { vfs_vnode_stat(vn, &mut st) } < 0 {
        return 0;
    }
    if cur.is_null() {
        return 0;
    }
    let cred = task_cred(cur);
    if (cred.fsuid as u32) == st.st_uid {
        1
    } else {
        0
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_sticky_may_remove(
    dir: *mut vnode_t,
    victim: *mut vnode_t,
) -> c_int {
    let cur = unsafe { ffi::a20_proc_current() };
    if dir.is_null() || victim.is_null() {
        return 0;
    }
    if unsafe { ffi::a20_proc_has_cap(cur, CAP_FOWNER) } != 0 {
        return 0;
    }

    let mut dst: kstat_t = unsafe { core::mem::zeroed() };
    let mut vst: kstat_t = unsafe { core::mem::zeroed() };
    if unsafe { vfs_vnode_stat(dir, &mut dst) } < 0 {
        return 0;
    }
    if (dst.st_mode & S_ISVTX) == 0 {
        return 0;
    }
    if unsafe { vfs_vnode_stat(victim, &mut vst) } < 0 {
        return 0;
    }

    let cred = task_cred(cur);
    let fsuid = if cur.is_null() { 0 } else { cred.fsuid as u32 };
    if fsuid == dst.st_uid || fsuid == vst.st_uid {
        0
    } else {
        -EPERM
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_drop_time_meta(vn: *mut vnode_t) {
    if vn.is_null() {
        return;
    }
    let (mnt, ino) = unsafe { (*vn).key() };
    let mut guard = TIME_META_TABLE.lock();
    for slot in guard.iter_mut() {
        if let Some(e) = slot {
            if e.mnt == mnt && e.ino == ino {
                *slot = None;
                return;
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn vfs_touch_mtime(vn: *mut vnode_t) {
    if vn.is_null() {
        return;
    }
    let (sec, nsec) = now_realtime();
    let vn_ref = unsafe { &*vn };
    let _ = with_time_meta(vn_ref, |e| {
        if e.atime == 0 && e.atime_nsec == 0 {
            e.atime = sec;
            e.atime_nsec = nsec;
        }
        e.mtime = sec;
        e.mtime_nsec = nsec;
        e.ctime = sec;
        e.ctime_nsec = nsec;
    });
}

#[no_mangle]
pub unsafe extern "C" fn vfs_set_times(vn: *mut vnode_t, times: *const u64) -> c_int {
    if vn.is_null() {
        return -EINVAL;
    }

    let (sec, nsec) = now_realtime();
    let mut atime = sec;
    let mut atime_nsec = nsec;
    let mut mtime = sec;
    let mut mtime_nsec = nsec;

    if !times.is_null() {
        let t = unsafe { core::slice::from_raw_parts(times, 4) };
        let need_old = t[1] == LINUX_UTIME_OMIT || t[3] == LINUX_UTIME_OMIT;
        let mut st: kstat_t = unsafe { core::mem::zeroed() };
        if need_old {
            if unsafe { vfs_vnode_stat(vn, &mut st) } < 0 {
                return -EINVAL;
            }
        }

        atime = if t[1] == LINUX_UTIME_OMIT {
            st.st_atime
        } else if t[1] == LINUX_UTIME_NOW {
            sec
        } else {
            t[0]
        };
        atime_nsec = if t[1] == LINUX_UTIME_OMIT {
            st.st_atime_nsec
        } else if t[1] == LINUX_UTIME_NOW {
            nsec
        } else {
            t[1]
        };

        mtime = if t[3] == LINUX_UTIME_OMIT {
            st.st_mtime
        } else if t[3] == LINUX_UTIME_NOW {
            sec
        } else {
            t[2]
        };
        mtime_nsec = if t[3] == LINUX_UTIME_OMIT {
            st.st_mtime_nsec
        } else if t[3] == LINUX_UTIME_NOW {
            nsec
        } else {
            t[3]
        };
    }

    if atime_nsec >= 1_000_000_000 || mtime_nsec >= 1_000_000_000 {
        return -EINVAL;
    }

    let vn_ref = unsafe { &*vn };
    let updated = with_time_meta(vn_ref, |e| {
        e.atime = atime;
        e.atime_nsec = atime_nsec;
        e.mtime = mtime;
        e.mtime_nsec = mtime_nsec;
        e.ctime = sec;
        e.ctime_nsec = nsec;
    });

    if updated.is_none() {
        return -ENOSPC;
    }
    0
}
