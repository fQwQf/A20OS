#![no_std]

//! SysV semaphore operation transactions on the existing C set layout.
//!
//! Rust owns operation validation, atomic value updates, wait counters and the
//! wait/recheck/wake protocol. C retains the registry, global lock, usercopy,
//! clock and one-shot park primitives.

use core::ffi::c_void;
use core::mem::size_of;

const SYSV_SEM_MAX: usize = 32;
const SYSV_SEM_PER_SET: usize = 64;
const IPC_NOWAIT: i16 = 0o4000;
const IPC_CREAT: i32 = 0o1000;
const IPC_EXCL: i32 = 0o2000;
const IPC_64_BIT: i32 = 0x100;
const IPC_RMID: i32 = 0;
const IPC_SET: i32 = 1;
const IPC_STAT: i32 = 2;
const IPC_INFO: i32 = 3;
const GETPID: i32 = 11;
const GETVAL: i32 = 12;
const GETALL: i32 = 13;
const GETNCNT: i32 = 14;
const GETZCNT: i32 = 15;
const SETVAL: i32 = 16;
const SETALL: i32 = 17;
const SEM_STAT: i32 = 18;
const SEM_INFO: i32 = 19;
const SEM_STAT_ANY: i32 = 20;

const EINTR: i32 = 4;
const EAGAIN: i32 = 11;
const EFAULT: i32 = 14;
const EINVAL: i32 = 22;
const EFBIG: i32 = 27;
const EEXIST: i32 = 17;
const ENOENT: i32 = 2;
const ENOSPC: i32 = 28;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct SemSet {
    namespace_ino: u64,
    used: i32,
    key: i32,
    nsems: i32,
    uid: u32,
    gid: u32,
    cuid: u32,
    cgid: u32,
    mode: u32,
    val: [u16; SYSV_SEM_PER_SET],
    ncnt: [u16; SYSV_SEM_PER_SET],
    zcnt: [u16; SYSV_SEM_PER_SET],
    last_pid: i32,
    _waiters: [usize; if usize::BITS == 64 { 12 } else { 19 }],
}

const _: () = {
    assert!(core::mem::offset_of!(SemSet, used) == 8);
    assert!(core::mem::offset_of!(SemSet, mode) == 36);
    assert!(core::mem::offset_of!(SemSet, val) == 40);
    assert!(core::mem::offset_of!(SemSet, ncnt) == 168);
    assert!(core::mem::offset_of!(SemSet, zcnt) == 296);
    assert!(core::mem::offset_of!(SemSet, last_pid) == 424);
    assert!(core::mem::size_of::<SemSet>() == if usize::BITS == 64 { 528 } else { 504 });
};

#[repr(C)]
#[derive(Clone, Copy)]
struct SemBuf {
    sem_num: u16,
    sem_op: i16,
    sem_flg: i16,
}

const _: () = assert!(size_of::<SemBuf>() == 6);

unsafe extern "C" {
    fn a20_sysv_sem_txn_lock(context: *mut c_void) -> u64;
    fn a20_sysv_sem_txn_unlock(context: *mut c_void, flags: u64);
    fn a20_sysv_sem_txn_namespace(context: *mut c_void) -> u64;
    fn a20_sysv_sem_txn_pid(context: *mut c_void) -> i32;
    fn a20_sysv_sem_txn_uid(context: *mut c_void) -> u32;
    fn a20_sysv_sem_txn_gid(context: *mut c_void) -> u32;
    fn a20_sysv_sem_txn_waitq_init(set: *mut c_void);
    fn a20_sysv_sem_txn_copy_to_user(
        context: *mut c_void,
        dst: *mut c_void,
        src: *const c_void,
        len: usize,
    ) -> i32;
    fn a20_sysv_sem_txn_wake(context: *mut c_void, set: *mut c_void);
    fn a20_sysv_sem_txn_signal_pending(context: *mut c_void) -> bool;
    fn a20_sysv_sem_txn_now(context: *mut c_void) -> u64;
    fn a20_sysv_sem_txn_copy_from_user(
        context: *mut c_void,
        dst: *mut c_void,
        src: *const c_void,
        len: usize,
    ) -> i32;
    fn a20_sysv_sem_txn_wait_prepare(context: *mut c_void, deadline: u64) -> *mut c_void;
    fn a20_sysv_sem_txn_wait_link(
        context: *mut c_void,
        waiter: *mut c_void,
        set: *mut c_void,
    ) -> bool;
    fn a20_sysv_sem_txn_wait_finish(waiter: *mut c_void, linked: bool, commit: bool);
}

#[repr(C)]
#[derive(Default, Copy, Clone)]
struct IpcPerm64 {
    key: i32,
    uid: u32,
    gid: u32,
    cuid: u32,
    cgid: u32,
    mode: u32,
    seq: i32,
    unused1: u64,
    unused2: u64,
}

#[repr(C)]
#[derive(Default, Copy, Clone)]
struct Semid64Ds {
    perm: IpcPerm64,
    otime: i64,
    ctime: i64,
    nsems: u64,
    unused3: u64,
    unused4: u64,
}

#[repr(C)]
#[derive(Default, Copy, Clone)]
struct SemInfo {
    semmap: i32,
    semmni: i32,
    semmns: i32,
    semmnu: i32,
    semmsl: i32,
    semopm: i32,
    semume: i32,
    semusz: i32,
    semvmx: i32,
    semaem: i32,
}

const _: () = {
    assert!(size_of::<IpcPerm64>() == 48);
    assert!(size_of::<Semid64Ds>() == 88);
    assert!(size_of::<SemInfo>() == 40);
};

/// Resolve or create a semaphore set while Rust owns namespace/key/id
/// uniqueness and initializes the ABI-visible set metadata.
#[no_mangle]
pub unsafe extern "C" fn a20_sysv_sem_get_txn(
    sets: *mut SemSet,
    set_count: usize,
    context: *mut c_void,
    key: i32,
    nsems: i32,
    semflg: i32,
) -> i32 {
    if sets.is_null() || set_count != SYSV_SEM_MAX || nsems < 0 || nsems as usize > SYSV_SEM_PER_SET {
        return -EINVAL;
    }
    let flags = unsafe { a20_sysv_sem_txn_lock(context) };
    let namespace = unsafe { a20_sysv_sem_txn_namespace(context) };
    for index in 0..set_count {
        let set = unsafe { &*sets.add(index) };
        if set.used != 0 && set.namespace_ino == namespace && key != 0 && set.key == key {
            let result = if semflg & IPC_CREAT != 0 && semflg & IPC_EXCL != 0 {
                -EEXIST
            } else if nsems > 0 && nsems > set.nsems {
                -EINVAL
            } else {
                index as i32
            };
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            return result;
        }
    }
    if key != 0 && semflg & IPC_CREAT == 0 {
        unsafe { a20_sysv_sem_txn_unlock(context, flags) };
        return -ENOENT;
    }
    if nsems <= 0 {
        unsafe { a20_sysv_sem_txn_unlock(context, flags) };
        return -EINVAL;
    }
    let uid = unsafe { a20_sysv_sem_txn_uid(context) };
    let gid = unsafe { a20_sysv_sem_txn_gid(context) };
    for index in 0..set_count {
        let set = unsafe { &mut *sets.add(index) };
        if set.used == 0 {
            set.namespace_ino = namespace;
            set.used = 1;
            set.key = key;
            set.nsems = nsems;
            set.uid = uid;
            set.gid = gid;
            set.cuid = uid;
            set.cgid = gid;
            set.mode = (semflg as u32) & 0o777;
            set.val = [0; SYSV_SEM_PER_SET];
            set.ncnt = [0; SYSV_SEM_PER_SET];
            set.zcnt = [0; SYSV_SEM_PER_SET];
            set.last_pid = 0;
            set._waiters = [0; if usize::BITS == 64 { 12 } else { 19 }];
            unsafe { a20_sysv_sem_txn_waitq_init(set as *mut SemSet as *mut c_void) };
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            return index as i32;
        }
    }
    unsafe { a20_sysv_sem_txn_unlock(context, flags) };
    -ENOSPC
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_sem_control_txn(
    sets: *mut SemSet,
    set_count: usize,
    context: *mut c_void,
    semid: i32,
    semnum: i32,
    command: i32,
    arg: *mut c_void,
) -> i32 {
    if sets.is_null() || set_count != SYSV_SEM_MAX {
        return -EINVAL;
    }
    let cmd = command & !IPC_64_BIT;
    let flags = unsafe { a20_sysv_sem_txn_lock(context) };
    if cmd == IPC_INFO || cmd == SEM_INFO {
        let mut info = SemInfo {
            semmni: SYSV_SEM_MAX as i32,
            semmns: (SYSV_SEM_MAX * SYSV_SEM_PER_SET) as i32,
            semmsl: SYSV_SEM_PER_SET as i32,
            semopm: SYSV_SEM_PER_SET as i32,
            semusz: 0,
            semvmx: 32767,
            ..SemInfo::default()
        };
        let namespace = unsafe { a20_sysv_sem_txn_namespace(context) };
        let mut highest = -1;
        for index in 0..set_count {
            let set = unsafe { &*sets.add(index) };
            if set.used != 0 && set.namespace_ino == namespace {
                highest = index as i32;
                info.semusz += 1;
            }
        }
        if cmd == IPC_INFO {
            info.semusz = 1;
        }
        unsafe { a20_sysv_sem_txn_unlock(context, flags) };
        if arg.is_null() || unsafe {
            a20_sysv_sem_txn_copy_to_user(context, arg, &info as *const SemInfo as *const c_void, size_of::<SemInfo>())
        } < 0 {
            return -EFAULT;
        }
        return if highest < 0 { 0 } else { highest };
    }
    let namespace = unsafe { a20_sysv_sem_txn_namespace(context) };
    if semid < 0 || semid as usize >= set_count {
        unsafe { a20_sysv_sem_txn_unlock(context, flags) };
        return -EINVAL;
    }
    let set = unsafe { &mut *sets.add(semid as usize) };
    if set.used == 0 || set.namespace_ino != namespace {
        unsafe { a20_sysv_sem_txn_unlock(context, flags) };
        return -EINVAL;
    }
    if semnum < 0 || (semnum >= set.nsems && !matches!(cmd, IPC_RMID | IPC_STAT | SEM_STAT | SEM_STAT_ANY | SEM_INFO)) {
        unsafe { a20_sysv_sem_txn_unlock(context, flags) };
        return -EINVAL;
    }
    match cmd {
        IPC_RMID => {
            set.used = 0;
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            unsafe { a20_sysv_sem_txn_wake(context, set as *mut SemSet as *mut c_void) };
            0
        }
        GETVAL => { let value = set.val[semnum as usize] as i32; unsafe { a20_sysv_sem_txn_unlock(context, flags) }; value }
        GETPID => { let value = set.last_pid; unsafe { a20_sysv_sem_txn_unlock(context, flags) }; value }
        GETNCNT => { let value = set.ncnt[semnum as usize] as i32; unsafe { a20_sysv_sem_txn_unlock(context, flags) }; value }
        GETZCNT => { let value = set.zcnt[semnum as usize] as i32; unsafe { a20_sysv_sem_txn_unlock(context, flags) }; value }
        SETVAL => {
            set.val[semnum as usize] = arg as usize as u16;
            set.last_pid = unsafe { a20_sysv_sem_txn_pid(context) };
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            unsafe { a20_sysv_sem_txn_wake(context, set as *mut SemSet as *mut c_void) };
            0
        }
        GETALL => {
            if arg.is_null() { unsafe { a20_sysv_sem_txn_unlock(context, flags) }; return -EINVAL; }
            let count = set.nsems as usize;
            let values = set.val;
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            if unsafe { a20_sysv_sem_txn_copy_to_user(context, arg, values.as_ptr().cast(), count * size_of::<u16>()) } < 0 { -EFAULT } else { 0 }
        }
        SETALL => {
            if arg.is_null() { unsafe { a20_sysv_sem_txn_unlock(context, flags) }; return -EINVAL; }
            let count = set.nsems as usize;
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            let mut values = [0u16; SYSV_SEM_PER_SET];
            if unsafe { a20_sysv_sem_txn_copy_from_user(context, values.as_mut_ptr().cast(), arg, count * size_of::<u16>()) } < 0 { return -EFAULT; }
            let flags = unsafe { a20_sysv_sem_txn_lock(context) };
            let set = unsafe { &mut *sets.add(semid as usize) };
            if set.used == 0 || set.namespace_ino != unsafe { a20_sysv_sem_txn_namespace(context) } { unsafe { a20_sysv_sem_txn_unlock(context, flags) }; return -EINVAL; }
            set.val[..count].copy_from_slice(&values[..count]);
            set.last_pid = unsafe { a20_sysv_sem_txn_pid(context) };
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            unsafe { a20_sysv_sem_txn_wake(context, set as *mut SemSet as *mut c_void) };
            0
        }
        IPC_SET => {
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            if arg.is_null() { return -EFAULT; }
            let mut ds = Semid64Ds::default();
            if unsafe { a20_sysv_sem_txn_copy_from_user(context, &mut ds as *mut Semid64Ds as *mut c_void, arg, size_of::<Semid64Ds>()) } < 0 { return -EFAULT; }
            let flags = unsafe { a20_sysv_sem_txn_lock(context) };
            let set = unsafe { &mut *sets.add(semid as usize) };
            if set.used == 0 || set.namespace_ino != unsafe { a20_sysv_sem_txn_namespace(context) } { unsafe { a20_sysv_sem_txn_unlock(context, flags) }; return -EINVAL; }
            set.uid = ds.perm.uid;
            set.gid = ds.perm.gid;
            set.mode = (set.mode & !0o777) | (ds.perm.mode & 0o777);
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            0
        }
        IPC_STAT | SEM_STAT | SEM_STAT_ANY => {
            let ds = Semid64Ds {
                perm: IpcPerm64 { key: set.key, uid: set.uid, gid: set.gid, cuid: set.cuid, cgid: set.cgid, mode: set.mode, seq: semid, ..IpcPerm64::default() },
                nsems: set.nsems as u64,
                ..Semid64Ds::default()
            };
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            if arg.is_null() || unsafe { a20_sysv_sem_txn_copy_to_user(context, arg, &ds as *const Semid64Ds as *const c_void, size_of::<Semid64Ds>()) } < 0 { return -EFAULT; }
            if cmd == IPC_STAT { 0 } else { semid }
        }
        _ => { unsafe { a20_sysv_sem_txn_unlock(context, flags) }; -EINVAL }
    }
}

fn sem_ops_can_apply(set: &SemSet, ops: &[SemBuf]) -> Result<Option<(usize, bool)>, i32> {
    for op in ops {
        let index = op.sem_num as usize;
        if index >= set.nsems as usize {
            return Err(-EFBIG);
        }
        if op.sem_op < 0 {
            let amount = -(op.sem_op as i32) as u16;
            if set.val[index] < amount {
                return Ok(Some((index, false)));
            }
        } else if op.sem_op == 0 && set.val[index] != 0 {
            return Ok(Some((index, true)));
        }
    }
    Ok(None)
}

unsafe fn sem_ops_apply(set: &mut SemSet, ops: &[SemBuf], pid: i32) {
    for op in ops {
        let index = op.sem_num as usize;
        if op.sem_op < 0 {
            let amount = -(op.sem_op as i32) as u16;
            set.val[index] = set.val[index].wrapping_sub(amount);
        } else {
            set.val[index] = set.val[index].wrapping_add(op.sem_op as u16);
        }
    }
    set.last_pid = pid;
}

unsafe fn sem_finish_prepared(waiter: *mut c_void) {
    if !waiter.is_null() {
        unsafe { a20_sysv_sem_txn_wait_finish(waiter, false, false) };
    }
}

/// Execute one semop/semtimedop transaction. The C registry keeps each set and
/// its wait queue alive; this function owns every semaphore-state transition.
#[no_mangle]
pub unsafe extern "C" fn a20_sysv_sem_timedop_txn(
    sets: *mut SemSet,
    set_count: usize,
    context: *mut c_void,
    semid: i32,
    sops: *const c_void,
    nsops: usize,
    deadline: u64,
) -> i32 {
    if sets.is_null()
        || set_count != SYSV_SEM_MAX
        || sops.is_null()
        || nsops == 0
        || nsops > SYSV_SEM_PER_SET
    {
        return -EINVAL;
    }
    let mut ops = [SemBuf {
        sem_num: 0,
        sem_op: 0,
        sem_flg: 0,
    }; SYSV_SEM_PER_SET];
    let copied = unsafe {
        a20_sysv_sem_txn_copy_from_user(
            context,
            ops.as_mut_ptr().cast(),
            sops,
            nsops * size_of::<SemBuf>(),
        )
    };
    if copied < 0 {
        return if copied == -1 { -EFAULT } else { copied };
    }
    let ops = &ops[..nsops];
    let mut waiter = core::ptr::null_mut();

    loop {
        let flags = unsafe { a20_sysv_sem_txn_lock(context) };
        let namespace = unsafe { a20_sysv_sem_txn_namespace(context) };
        let valid = semid >= 0 && (semid as usize) < set_count && unsafe {
            let set = &*sets.add(semid as usize);
            set.used != 0 && set.namespace_ino == namespace
        };
        if !valid {
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            unsafe { sem_finish_prepared(waiter) };
            return -EINVAL;
        }
        let set = unsafe { &mut *sets.add(semid as usize) };
        let can_apply = match sem_ops_can_apply(set, ops) {
            Err(error) => {
                unsafe { a20_sysv_sem_txn_unlock(context, flags) };
                unsafe { sem_finish_prepared(waiter) };
                return error;
            }
            Ok(result) => result,
        };
        let Some((wait_sem, wait_zero)) = can_apply else {
            let pid = unsafe { a20_sysv_sem_txn_pid(context) };
            unsafe { sem_ops_apply(set, ops, pid) };
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            unsafe { sem_finish_prepared(waiter) };
            unsafe { a20_sysv_sem_txn_wake(context, set as *mut SemSet as *mut c_void) };
            return 0;
        };

        let nowait = ops.iter().any(|op| op.sem_flg & IPC_NOWAIT != 0);
        if nowait {
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            unsafe { sem_finish_prepared(waiter) };
            return -EAGAIN;
        }
        if unsafe { a20_sysv_sem_txn_signal_pending(context) } {
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            unsafe { sem_finish_prepared(waiter) };
            return -EINTR;
        }
        if deadline != 0 {
            let now = unsafe { a20_sysv_sem_txn_now(context) };
            if now.wrapping_sub(deadline) as i64 >= 0 {
                unsafe { a20_sysv_sem_txn_unlock(context, flags) };
                unsafe { sem_finish_prepared(waiter) };
                return -EAGAIN;
            }
        }

        if waiter.is_null() {
            unsafe { a20_sysv_sem_txn_unlock(context, flags) };
            waiter = unsafe { a20_sysv_sem_txn_wait_prepare(context, deadline) };
            if waiter.is_null() {
                return -EAGAIN;
            }
            continue;
        }

        let linked = unsafe { a20_sysv_sem_txn_wait_link(context, waiter, set as *mut SemSet as *mut c_void) };
        if wait_zero {
            set.zcnt[wait_sem] = set.zcnt[wait_sem].wrapping_add(1);
        } else {
            set.ncnt[wait_sem] = set.ncnt[wait_sem].wrapping_add(1);
        }
        unsafe { a20_sysv_sem_txn_unlock(context, flags) };
        unsafe { a20_sysv_sem_txn_wait_finish(waiter, linked, true) };
        waiter = core::ptr::null_mut();

        let flags = unsafe { a20_sysv_sem_txn_lock(context) };
        let set = unsafe { &mut *sets.add(semid as usize) };
        if wait_zero {
            if set.zcnt[wait_sem] > 0 {
                set.zcnt[wait_sem] -= 1;
            }
        } else if set.ncnt[wait_sem] > 0 {
            set.ncnt[wait_sem] -= 1;
        }
        unsafe { a20_sysv_sem_txn_unlock(context, flags) };
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::boxed::Box;

    impl SemSet {
        const fn empty() -> Self {
            Self {
                namespace_ino: 0,
                used: 0,
                key: 0,
                nsems: 0,
                uid: 0,
                gid: 0,
                cuid: 0,
                cgid: 0,
                mode: 0,
                val: [0; SYSV_SEM_PER_SET],
                ncnt: [0; SYSV_SEM_PER_SET],
                zcnt: [0; SYSV_SEM_PER_SET],
                last_pid: 0,
                _waiters: [0; if usize::BITS == 64 { 12 } else { 19 }],
            }
        }
    }

    struct MockSem {
        sets: *mut SemSet,
        locked: bool,
        namespace: u64,
        pid: i32,
        uid: u32,
        gid: u32,
        signal: bool,
        now: u64,
        copy_error: i32,
        prepares: usize,
        links: usize,
        finishes: usize,
        wakes: usize,
        fail_prepare: bool,
        apply_on_finish: Option<(usize, u16)>,
        remove_on_finish: bool,
    }

    impl MockSem {
        fn new(sets: &mut [SemSet; SYSV_SEM_MAX]) -> Self {
            Self {
                sets: sets.as_mut_ptr(),
                locked: false,
                namespace: 7,
                pid: 44,
                uid: 1000,
                gid: 100,
                signal: false,
                now: 0,
                copy_error: 0,
                prepares: 0,
                links: 0,
                finishes: 0,
                wakes: 0,
                fail_prepare: false,
                apply_on_finish: None,
                remove_on_finish: false,
            }
        }
    }

    struct MockWaiter {
        context: *mut c_void,
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_lock(context: *mut c_void) -> u64 {
        let mock = unsafe { &mut *(context as *mut MockSem) };
        assert!(!mock.locked);
        mock.locked = true;
        0xa5
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_unlock(context: *mut c_void, flags: u64) {
        let mock = unsafe { &mut *(context as *mut MockSem) };
        assert!(mock.locked);
        assert_eq!(flags, 0xa5);
        mock.locked = false;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_namespace(context: *mut c_void) -> u64 {
        unsafe { (*(context as *mut MockSem)).namespace }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_pid(context: *mut c_void) -> i32 {
        unsafe { (*(context as *mut MockSem)).pid }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_uid(context: *mut c_void) -> u32 {
        unsafe { (*(context as *mut MockSem)).uid }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_gid(context: *mut c_void) -> u32 {
        unsafe { (*(context as *mut MockSem)).gid }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_waitq_init(_set: *mut c_void) {}

    #[test]
    fn semget_namespace_key_creation_and_exclusion() {
        let mut sets = [SemSet::empty(); SYSV_SEM_MAX];
        let mut mock = MockSem::new(&mut sets);
        let context = &mut mock as *mut MockSem as *mut c_void;
        unsafe {
            assert_eq!(a20_sysv_sem_get_txn(mock.sets, SYSV_SEM_MAX, context, 0x1234, 3, IPC_CREAT | 0o640), 0);
            assert_eq!(a20_sysv_sem_get_txn(mock.sets, SYSV_SEM_MAX, context, 0x1234, 2, 0), 0);
            assert_eq!(a20_sysv_sem_get_txn(mock.sets, SYSV_SEM_MAX, context, 0x1234, 3, IPC_CREAT | IPC_EXCL), -EEXIST);
            assert_eq!(a20_sysv_sem_get_txn(mock.sets, SYSV_SEM_MAX, context, 0x1234, 4, 0), -EINVAL);
            assert_eq!(a20_sysv_sem_get_txn(mock.sets, SYSV_SEM_MAX, context, 0x5678, 1, 0), -ENOENT);
        }
        assert_eq!(sets[0].namespace_ino, mock.namespace);
        assert_eq!((sets[0].uid, sets[0].gid, sets[0].mode), (1000, 100, 0o640));
        assert!(!mock.locked);
    }

    #[test]
    fn semctl_metadata_values_arrays_and_remove() {
        let mut sets = [SemSet::empty(); SYSV_SEM_MAX];
        let mut mock = MockSem::new(&mut sets);
        let context = &mut mock as *mut MockSem as *mut c_void;
        let id = unsafe { a20_sysv_sem_get_txn(mock.sets, SYSV_SEM_MAX, context, 0x2468, 2, IPC_CREAT | 0o600) };
        assert_eq!(id, 0);
        unsafe {
            assert_eq!(a20_sysv_sem_control_txn(mock.sets, SYSV_SEM_MAX, context, id, 1, SETVAL, 23usize as *mut c_void), 0);
            assert_eq!(a20_sysv_sem_control_txn(mock.sets, SYSV_SEM_MAX, context, id, 1, GETVAL, core::ptr::null_mut()), 23);
            assert_eq!(a20_sysv_sem_control_txn(mock.sets, SYSV_SEM_MAX, context, id, 0, GETPID, core::ptr::null_mut()), 44);
        }
        let mut values = [0u16; 2];
        unsafe { assert_eq!(a20_sysv_sem_control_txn(mock.sets, SYSV_SEM_MAX, context, id, 0, GETALL, values.as_mut_ptr().cast()), 0) };
        assert_eq!(values, [0, 23]);
        let replacement = [8u16, 13u16];
        unsafe { assert_eq!(a20_sysv_sem_control_txn(mock.sets, SYSV_SEM_MAX, context, id, 0, SETALL, replacement.as_ptr() as *mut c_void), 0) };
        assert_eq!(&sets[0].val[..2], &replacement);
        let mut ds = Semid64Ds::default();
        unsafe { assert_eq!(a20_sysv_sem_control_txn(mock.sets, SYSV_SEM_MAX, context, id, 0, IPC_STAT, (&mut ds as *mut Semid64Ds).cast()), 0) };
        assert_eq!((ds.perm.key, ds.perm.uid, ds.perm.gid, ds.perm.mode, ds.nsems), (0x2468, 1000, 100, 0o600, 2));
        unsafe { assert_eq!(a20_sysv_sem_control_txn(mock.sets, SYSV_SEM_MAX, context, id, 0, IPC_RMID, core::ptr::null_mut()), 0) };
        assert_eq!(mock.wakes, 3);
        unsafe { assert_eq!(a20_sysv_sem_control_txn(mock.sets, SYSV_SEM_MAX, context, id, 0, GETVAL, core::ptr::null_mut()), -EINVAL) };
        assert!(!mock.locked);
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_signal_pending(context: *mut c_void) -> bool {
        unsafe { (*(context as *mut MockSem)).signal }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_now(context: *mut c_void) -> u64 {
        unsafe { (*(context as *mut MockSem)).now }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_copy_from_user(
        context: *mut c_void,
        dst: *mut c_void,
        src: *const c_void,
        len: usize,
    ) -> i32 {
        let mock = unsafe { &mut *(context as *mut MockSem) };
        assert!(!mock.locked);
        if mock.copy_error != 0 {
            return mock.copy_error;
        }
        unsafe { core::ptr::copy_nonoverlapping(src as *const u8, dst as *mut u8, len) };
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_copy_to_user(
        context: *mut c_void,
        dst: *mut c_void,
        src: *const c_void,
        len: usize,
    ) -> i32 {
        let mock = unsafe { &mut *(context as *mut MockSem) };
        assert!(!mock.locked);
        if mock.copy_error != 0 {
            return mock.copy_error;
        }
        unsafe { core::ptr::copy_nonoverlapping(src as *const u8, dst as *mut u8, len) };
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_wait_prepare(
        context: *mut c_void,
        _deadline: u64,
    ) -> *mut c_void {
        let mock = unsafe { &mut *(context as *mut MockSem) };
        assert!(!mock.locked);
        mock.prepares += 1;
        if mock.fail_prepare {
            return core::ptr::null_mut();
        }
        Box::into_raw(Box::new(MockWaiter { context })).cast()
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_wait_link(
        context: *mut c_void,
        _waiter: *mut c_void,
        _set: *mut c_void,
    ) -> bool {
        let mock = unsafe { &mut *(context as *mut MockSem) };
        assert!(mock.locked);
        mock.links += 1;
        true
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_wait_finish(
        waiter: *mut c_void,
        linked: bool,
        commit: bool,
    ) {
        let waiter = unsafe { Box::from_raw(waiter as *mut MockWaiter) };
        let mock = unsafe { &mut *(waiter.context as *mut MockSem) };
        assert!(!mock.locked);
        mock.finishes += 1;
        if linked && commit {
            if let Some((index, value)) = mock.apply_on_finish.take() {
                unsafe { (*mock.sets).val[index] = value };
            }
            if mock.remove_on_finish {
                unsafe { (*mock.sets).used = 0 };
            }
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_sem_txn_wake(context: *mut c_void, _set: *mut c_void) {
        let mock = unsafe { &mut *(context as *mut MockSem) };
        assert!(!mock.locked);
        mock.wakes += 1;
    }

    fn op(num: u16, value: i16, flags: i16) -> SemBuf {
        SemBuf {
            sem_num: num,
            sem_op: value,
            sem_flg: flags,
        }
    }

    #[test]
    fn semop_commits_atomic_values_and_rejects_invalid_input() {
        let mut sets = [SemSet::empty(); SYSV_SEM_MAX];
        sets[0].used = 1;
        sets[0].namespace_ino = 7;
        sets[0].nsems = 2;
        sets[0].val[0] = 1;
        sets[0].val[1] = 3;
        let mut mock = MockSem::new(&mut sets);
        let context = &mut mock as *mut MockSem as *mut c_void;
        let mut operations = [op(0, -1, 0), op(1, 2, 0)];
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 2, 0), 0);
        }
        assert_eq!((sets[0].val[0], sets[0].val[1], sets[0].last_pid), (0, 5, 44));
        assert_eq!(mock.wakes, 1);

        operations = [op(0, -1, IPC_NOWAIT), op(1, 0, 0)];
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 2, 0), -EAGAIN);
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, [op(2, 1, 0)].as_mut_ptr().cast(), 1, 0), -EFBIG);
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 1, operations.as_mut_ptr().cast(), 1, 0), -EINVAL);
        }
        assert_eq!((sets[0].val[0], sets[0].val[1]), (0, 5));
        assert!(!mock.locked);
    }

    #[test]
    fn semop_wait_wake_restores_wait_counters() {
        let mut sets = [SemSet::empty(); SYSV_SEM_MAX];
        sets[0].used = 1;
        sets[0].namespace_ino = 7;
        sets[0].nsems = 1;
        let mut mock = MockSem::new(&mut sets);
        mock.apply_on_finish = Some((0, 1));
        let context = &mut mock as *mut MockSem as *mut c_void;
        let mut operations = [op(0, -1, 0)];
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 1, 0), 0);
        }
        assert_eq!((sets[0].val[0], sets[0].ncnt[0]), (0, 0));
        assert_eq!((mock.prepares, mock.links, mock.finishes, mock.wakes), (1, 1, 1, 1));

        sets[0].val[0] = 1;
        operations = [op(0, 0, 0)];
        mock.apply_on_finish = Some((0, 0));
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 1, 0), 0);
        }
        assert_eq!((sets[0].val[0], sets[0].zcnt[0]), (0, 0));
        assert!(!mock.locked);
    }

    #[test]
    fn semop_fault_signal_deadline_prepare_and_remove_paths() {
        let mut sets = [SemSet::empty(); SYSV_SEM_MAX];
        sets[0].used = 1;
        sets[0].namespace_ino = 7;
        sets[0].nsems = 1;
        let mut mock = MockSem::new(&mut sets);
        let context = &mut mock as *mut MockSem as *mut c_void;
        let mut operations = [op(0, -1, 0)];

        mock.copy_error = -EFAULT;
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 1, 0), -EFAULT);
        }
        mock.copy_error = 0;
        mock.signal = true;
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 1, 0), -EINTR);
        }
        mock.signal = false;
        mock.now = 6;
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 1, 5), -EAGAIN);
        }
        mock.now = 0;
        mock.fail_prepare = true;
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 1, 0), -EAGAIN);
        }
        mock.fail_prepare = false;
        mock.remove_on_finish = true;
        unsafe {
            assert_eq!(a20_sysv_sem_timedop_txn(sets.as_mut_ptr(), 32, context, 0, operations.as_mut_ptr().cast(), 1, 0), -EINVAL);
        }
        assert_eq!(sets[0].ncnt[0], 0);
        assert!(!mock.locked);
    }
}
