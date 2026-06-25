#![no_std]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::IrqSaveSpinLock;
use core::ffi::{c_int, c_void};
use core::ptr;

use ffi::{
    fs_flock_t, wait_queue_t, task_t, vfile_t, EAGAIN, EBADF, EDEADLK, EINVAL, ENOLCK,
    ERESTARTSYS, F_RDLCK, F_UNLCK, F_WRLCK, FS_LOCK_OWNER_OFD, FS_LOCK_OWNER_PID, LOCK_EX,
    LOCK_NB, LOCK_SH, LOCK_UN, SEEK_CUR, SEEK_END, SEEK_SET,
};

const FS_FILE_LOCK_MAX: usize = 256;
const FS_LOCK_MAX_RETRIES: usize = 1024;
const FS_FLOCK_MAX_RETRIES: usize = 1024;
const RANGE_EOF: i64 = 0x7fff_ffff_ffff_ffffu64 as i64;

#[derive(Clone, Copy)]
struct FileLock {
    used: c_int,
    owner_kind: c_int,
    key: usize,
    owner: usize,
    lock_type: i16,
    start: i64,
    end: i64,
}

impl FileLock {
    const fn empty() -> Self {
        Self {
            used: 0,
            owner_kind: 0,
            key: 0,
            owner: 0,
            lock_type: 0,
            start: 0,
            end: 0,
        }
    }
}

#[derive(Clone, Copy)]
struct BsdFlock {
    used: c_int,
    key: usize,
    owner: *mut vfile_t,
    lock_type: c_int,
}

impl BsdFlock {
    const fn empty() -> Self {
        Self {
            used: 0,
            key: 0,
            owner: ptr::null_mut(),
            lock_type: 0,
        }
    }
}

struct LockState {
    waiters: wait_queue_t,
    waiters_initialized: bool,
    file_locks: [FileLock; FS_FILE_LOCK_MAX],
    bsd_flocks: [BsdFlock; FS_FILE_LOCK_MAX],
}

impl LockState {
    const fn new() -> Self {
        Self {
            waiters: wait_queue_t {
                lock: ffi::spinlock_t { locked: 0 },
                head: ptr::null_mut(),
            },
            waiters_initialized: false,
            file_locks: [FileLock::empty(); FS_FILE_LOCK_MAX],
            bsd_flocks: [BsdFlock::empty(); FS_FILE_LOCK_MAX],
        }
    }
}

static LOCK_STATE: IrqSaveSpinLock<LockState> = IrqSaveSpinLock::new(LockState::new());

unsafe impl Send for LockState {}

fn ensure_waiters_init(state: &mut LockState) {
    if !state.waiters_initialized {
        unsafe { ffi::wait_queue_init(&mut state.waiters) };
        state.waiters_initialized = true;
    }
}

fn file_key(vf: *mut vfile_t) -> usize {
    unsafe { ffi::a20_locks_file_key(vf) }
}

fn file_size(vf: *mut vfile_t) -> i64 {
    unsafe { ffi::a20_locks_file_size(vf) }
}

fn file_offset(vf: *mut vfile_t) -> i64 {
    unsafe { ffi::a20_locks_file_offset(vf) }
}

fn lock_range(vf: *mut vfile_t, lk: &fs_flock_t) -> Result<(i64, i64), c_int> {
    let base = match lk.l_whence as c_int {
        SEEK_SET => 0,
        SEEK_CUR => file_offset(vf),
        SEEK_END => file_size(vf),
        _ => return Err(-EINVAL),
    };

    let mut start = base.wrapping_add(lk.l_start);
    let end = if lk.l_len == 0 {
        RANGE_EOF
    } else if lk.l_len > 0 {
        start.wrapping_add(lk.l_len).wrapping_sub(1)
    } else {
        let end = start.wrapping_sub(1);
        start = start.wrapping_add(lk.l_len);
        end
    };

    if start < 0 {
        return Err(-EINVAL);
    }
    Ok((start, end))
}

fn overlaps(a0: i64, a1: i64, b0: i64, b1: i64) -> bool {
    a0 <= b1 && b0 <= a1
}

fn conflicts(
    held: &FileLock,
    key: usize,
    owner_kind: c_int,
    owner: usize,
    lock_type: i16,
    start: i64,
    end: i64,
) -> bool {
    if held.used == 0 || held.key != key {
        return false;
    }
    if held.owner_kind == owner_kind && held.owner == owner {
        return false;
    }
    if !overlaps(held.start, held.end, start, end) {
        return false;
    }
    if held.lock_type == F_RDLCK && lock_type == F_RDLCK {
        return false;
    }
    true
}

fn range_len_from_end(start: i64, end: i64) -> i64 {
    if end == RANGE_EOF {
        0
    } else {
        end.wrapping_sub(start).wrapping_add(1)
    }
}

fn fs_lock_wait(waiters: *mut wait_queue_t) -> c_int {
    let cur: *mut task_t = unsafe { ffi::proc_current() };
    if cur.is_null() {
        unsafe { ffi::proc_yield() };
        return 0;
    }
    if unsafe { ffi::signal_task_has_unblocked(cur) } != 0 {
        return -ERESTARTSYS;
    }

    let mut entry = ffi::wait_queue_entry_t {
        next: ptr::null_mut(),
        prev: ptr::null_mut(),
        task: cur as *mut c_void,
    };

    unsafe {
        ffi::wait_queue_prepare(waiters, &mut entry);
        ffi::sched();
        ffi::wait_queue_finish(waiters, &mut entry);
    }

    if unsafe { ffi::signal_task_has_unblocked(cur) } != 0 {
        -ERESTARTSYS
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn fs_locks_get(
    vf: *mut vfile_t,
    lk: *mut fs_flock_t,
    owner_kind: c_int,
    owner: usize,
) -> c_int {
    if lk.is_null() {
        return -EINVAL;
    }

    let req = unsafe { &mut *lk };
    if req.l_type != F_RDLCK && req.l_type != F_WRLCK && req.l_type != F_UNLCK {
        return -EINVAL;
    }

    let (start, end) = match lock_range(vf, req) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let key = file_key(vf);

    let mut guard = LOCK_STATE.lock();
    ensure_waiters_init(&mut guard);
    for held in guard.file_locks.iter() {
        if !conflicts(held, key, owner_kind, owner, req.l_type, start, end) {
            continue;
        }
        req.l_type = held.lock_type;
        req.l_whence = SEEK_SET as i16;
        req.l_start = held.start;
        req.l_len = range_len_from_end(held.start, held.end);
        req.l_pid = if held.owner_kind == FS_LOCK_OWNER_PID {
            held.owner as c_int
        } else {
            -1
        };
        return 0;
    }

    req.l_type = F_UNLCK;
    0
}

#[no_mangle]
pub extern "C" fn fs_locks_set(
    vf: *mut vfile_t,
    lk: *const fs_flock_t,
    owner_kind: c_int,
    owner: usize,
    wait: c_int,
) -> c_int {
    if lk.is_null() {
        return -EINVAL;
    }

    let req = unsafe { &*lk };
    if req.l_type != F_RDLCK && req.l_type != F_WRLCK && req.l_type != F_UNLCK {
        return -EINVAL;
    }

    let (start, end) = match lock_range(vf, req) {
        Ok(v) => v,
        Err(e) => return e,
    };
    let key = file_key(vf);

    let mut prev_blocker = 0usize;
    let mut deadlock_retries = 0usize;

    'retry: loop {
        if deadlock_retries >= FS_LOCK_MAX_RETRIES {
            return -EDEADLK;
        }
        deadlock_retries += 1;

        let mut guard = LOCK_STATE.lock();
        ensure_waiters_init(&mut guard);
        let mut changed = false;

        if req.l_type != F_UNLCK {
            for i in 0..FS_FILE_LOCK_MAX {
                if !conflicts(
                    &guard.file_locks[i],
                    key,
                    owner_kind,
                    owner,
                    req.l_type,
                    start,
                    end,
                ) {
                    continue;
                }
                if wait == 0 {
                    return -EAGAIN;
                }
                let blocker = guard.file_locks[i].owner;
                if blocker == prev_blocker && blocker != 0 {
                    for held in guard.file_locks.iter() {
                        if held.used != 0
                            && held.owner_kind == owner_kind
                            && held.owner == owner
                            && held.key != key
                            && deadlock_retries > 4
                        {
                            return -EDEADLK;
                        }
                    }
                }
                prev_blocker = blocker;
                let waiters = &mut guard.waiters as *mut wait_queue_t;
                drop(guard);
                let r = fs_lock_wait(waiters);
                if r < 0 {
                    return r;
                }
                continue 'retry;
            }
        }

        let mut temp_count = 0usize;
        let mut temp_start = [0i64; FS_FILE_LOCK_MAX];
        let mut temp_end = [0i64; FS_FILE_LOCK_MAX];
        let mut temp_type = [0i16; FS_FILE_LOCK_MAX];

        for held in guard.file_locks.iter_mut() {
            if held.used != 0
                && held.key == key
                && held.owner_kind == owner_kind
                && held.owner == owner
            {
                changed = true;
                temp_start[temp_count] = held.start;
                temp_end[temp_count] = held.end;
                temp_type[temp_count] = held.lock_type;
                temp_count += 1;
                held.used = 0;
            }
        }

        let mut new_count = 0usize;
        let mut new_start = [0i64; FS_FILE_LOCK_MAX * 2];
        let mut new_end = [0i64; FS_FILE_LOCK_MAX * 2];
        let mut new_type = [0i16; FS_FILE_LOCK_MAX * 2];

        for i in 0..temp_count {
            let s_l = temp_start[i];
            let e_l = temp_end[i];
            let t_l = temp_type[i];

            if e_l < start || s_l > end {
                new_start[new_count] = s_l;
                new_end[new_count] = e_l;
                new_type[new_count] = t_l;
                new_count += 1;
            } else {
                if s_l < start {
                    new_start[new_count] = s_l;
                    new_end[new_count] = start.wrapping_sub(1);
                    new_type[new_count] = t_l;
                    new_count += 1;
                }
                if e_l > end {
                    new_start[new_count] = end.wrapping_add(1);
                    new_end[new_count] = e_l;
                    new_type[new_count] = t_l;
                    new_count += 1;
                }
            }
        }

        if req.l_type != F_UNLCK {
            new_start[new_count] = start;
            new_end[new_count] = end;
            new_type[new_count] = req.l_type;
            new_count += 1;
        }

        for i in 0..new_count.saturating_sub(1) {
            for j in (i + 1)..new_count {
                if new_start[i] > new_start[j] {
                    new_start.swap(i, j);
                    new_end.swap(i, j);
                    new_type.swap(i, j);
                }
            }
        }

        let mut merged_count = 0usize;
        let mut merged_start = [0i64; FS_FILE_LOCK_MAX * 2];
        let mut merged_end = [0i64; FS_FILE_LOCK_MAX * 2];
        let mut merged_type = [0i16; FS_FILE_LOCK_MAX * 2];

        for i in 0..new_count {
            if merged_count > 0
                && merged_type[merged_count - 1] == new_type[i]
                && (merged_end[merged_count - 1] == RANGE_EOF
                    || merged_end[merged_count - 1].wrapping_add(1) >= new_start[i])
            {
                if new_end[i] > merged_end[merged_count - 1] {
                    merged_end[merged_count - 1] = new_end[i];
                }
            } else {
                merged_start[merged_count] = new_start[i];
                merged_end[merged_count] = new_end[i];
                merged_type[merged_count] = new_type[i];
                merged_count += 1;
            }
        }

        let mut free_slots = 0usize;
        for held in guard.file_locks.iter() {
            if held.used == 0 {
                free_slots += 1;
            }
        }

        if merged_count > free_slots {
            for i in 0..temp_count {
                for held in guard.file_locks.iter_mut() {
                    if held.used == 0 {
                        *held = FileLock {
                            used: 1,
                            owner_kind,
                            key,
                            owner,
                            lock_type: temp_type[i],
                            start: temp_start[i],
                            end: temp_end[i],
                        };
                        break;
                    }
                }
            }
            return -ENOLCK;
        }

        let mut write_idx = 0usize;
        for held in guard.file_locks.iter_mut() {
            if write_idx >= merged_count {
                break;
            }
            if held.used == 0 {
                *held = FileLock {
                    used: 1,
                    owner_kind,
                    key,
                    owner,
                    lock_type: merged_type[write_idx],
                    start: merged_start[write_idx],
                    end: merged_end[write_idx],
                };
                write_idx += 1;
            }
        }

        if changed || req.l_type == F_UNLCK {
            unsafe { ffi::wait_queue_wake_all(&mut guard.waiters) };
        }
        return 0;
    }
}

#[no_mangle]
pub extern "C" fn fs_locks_release_process(pid: c_int) {
    let mut guard = LOCK_STATE.lock();
    ensure_waiters_init(&mut guard);
    let mut changed = false;
    for held in guard.file_locks.iter_mut() {
        if held.used != 0 && held.owner_kind == FS_LOCK_OWNER_PID && held.owner == pid as usize {
            held.used = 0;
            changed = true;
        }
    }
    if changed {
        unsafe { ffi::wait_queue_wake_all(&mut guard.waiters) };
    }
}

#[no_mangle]
pub extern "C" fn fs_locks_release_file(vf: *mut vfile_t, owner: usize) {
    let key = file_key(vf);
    let cur_pid = unsafe { ffi::a20_locks_current_pid() };
    let mut guard = LOCK_STATE.lock();
    ensure_waiters_init(&mut guard);
    let mut changed = false;

    for i in 0..FS_FILE_LOCK_MAX {
        if guard.file_locks[i].used != 0
            && guard.file_locks[i].owner_kind == FS_LOCK_OWNER_OFD
            && guard.file_locks[i].owner == owner
        {
            guard.file_locks[i].used = 0;
            changed = true;
        }
        if guard.file_locks[i].used != 0
            && guard.file_locks[i].key == key
            && guard.file_locks[i].owner_kind == FS_LOCK_OWNER_PID
            && guard.file_locks[i].owner == cur_pid as usize
        {
            guard.file_locks[i].used = 0;
            changed = true;
        }
        if guard.bsd_flocks[i].used != 0
            && guard.bsd_flocks[i].key == key
            && guard.bsd_flocks[i].owner == vf
        {
            guard.bsd_flocks[i].used = 0;
            changed = true;
        }
    }

    if changed {
        unsafe { ffi::wait_queue_wake_all(&mut guard.waiters) };
    }
}

#[no_mangle]
pub extern "C" fn fs_flocks_apply(vf: *mut vfile_t, operation: c_int) -> c_int {
    if vf.is_null() {
        return -EBADF;
    }

    let op = operation & (LOCK_SH | LOCK_EX | LOCK_UN);
    if (operation & !(LOCK_SH | LOCK_EX | LOCK_NB | LOCK_UN)) != 0 || op == 0 {
        return -EINVAL;
    }
    if (op & (op - 1)) != 0 {
        return -EINVAL;
    }

    let key = file_key(vf);
    if op == LOCK_UN {
        let mut guard = LOCK_STATE.lock();
        ensure_waiters_init(&mut guard);
        let mut changed = false;
        for flock in guard.bsd_flocks.iter_mut() {
            if flock.used != 0 && flock.key == key && flock.owner == vf {
                flock.used = 0;
                changed = true;
            }
        }
        if changed {
            unsafe { ffi::wait_queue_wake_all(&mut guard.waiters) };
        }
        return 0;
    }

    let lock_type = if op == LOCK_EX { F_WRLCK as c_int } else { F_RDLCK as c_int };
    let mut retries = 0usize;

    'flock_retry: loop {
        if retries >= FS_FLOCK_MAX_RETRIES {
            return -EDEADLK;
        }
        retries += 1;

        let mut guard = LOCK_STATE.lock();
        ensure_waiters_init(&mut guard);
        for i in 0..FS_FILE_LOCK_MAX {
            let flock = &guard.bsd_flocks[i];
            if flock.used == 0 || flock.key != key || flock.owner == vf {
                continue;
            }
            if flock.lock_type == F_RDLCK as c_int && lock_type == F_RDLCK as c_int {
                continue;
            }
            if (operation & LOCK_NB) != 0 {
                return -EAGAIN;
            }
            let waiters = &mut guard.waiters as *mut wait_queue_t;
            drop(guard);
            let r = fs_lock_wait(waiters);
            if r < 0 {
                return r;
            }
            continue 'flock_retry;
        }

        for flock in guard.bsd_flocks.iter_mut() {
            if flock.used != 0 && flock.key == key && flock.owner == vf {
                flock.lock_type = lock_type;
                unsafe { ffi::wait_queue_wake_all(&mut guard.waiters) };
                return 0;
            }
        }

        for flock in guard.bsd_flocks.iter_mut() {
            if flock.used == 0 {
                *flock = BsdFlock {
                    used: 1,
                    key,
                    owner: vf,
                    lock_type,
                };
                unsafe { ffi::wait_queue_wake_all(&mut guard.waiters) };
                return 0;
            }
        }
        return -ENOLCK;
    }
}
