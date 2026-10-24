#![no_std]

//! Futex wait admission and cancellation transaction.
//!
//! Rust owns the waiter key and the prepare/recheck/link/park/unlink lifetime.
//! C bridges only the MM mapping pin, wait-queue primitives, usercopy, and
//! scheduler park mechanism.

use core::ffi::c_void;
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

const EFAULT: i32 = 14;
const EINVAL: i32 = 22;
const ESRCH: i32 = 3;
const EAGAIN: i32 = 11;
const ERESTARTSYS: i32 = 512;
const PROC_WAIT_INTERRUPTIBLE: i32 = 1;
const PROC_WAKE_TIMEOUT: i32 = 2;
const ETIMEDOUT: i32 = 110;
const BUCKETS: usize = 64;

#[repr(C)]
#[derive(Clone, Copy)]
struct ProcWaitToken {
    task: *mut c_void,
    seq: u64,
    prepare_error: i32,
}

#[repr(C)]
pub struct WaitQueueEntry {
    next: *mut WaitQueueEntry,
    prev: *mut WaitQueueEntry,
    queue: *mut c_void,
    task: *mut c_void,
    wait_seq: u64,
    flags: u32,
    key: usize,
    priv_data: *mut c_void,
    linked: bool,
}

#[repr(C)]
struct WaitQueue {
    _opaque: [u8; 0],
}

#[repr(C)]
struct FutexWaitKey {
    vkey: usize,
    pkey: usize,
    mm: *mut c_void,
    bitset: u32,
}

#[repr(C)]
struct FutexWaiter {
    entry: WaitQueueEntry,
    key: FutexWaitKey,
}

#[repr(C)]
struct FutexWakeArg {
    mm: *mut c_void,
    vkey: usize,
    pkey: usize,
    bitset: u32,
}

#[repr(C)]
struct FutexRequeueArg {
    vkey2: usize,
    pkey2: usize,
    mm: *mut c_void,
}

struct LinkedWaiter {
    queue: *mut WaitQueue,
    entry: *mut WaitQueueEntry,
}

impl Drop for LinkedWaiter {
    fn drop(&mut self) {
        unsafe { a20_futex_wait_unlink(self.queue, self.entry) };
    }
}

struct MappingPin {
    task: *mut c_void,
    flags: u64,
}

impl Drop for MappingPin {
    fn drop(&mut self) {
        unsafe { a20_futex_mapping_unlock(self.task, self.flags) };
    }
}

struct BucketLock {
    queue: *mut WaitQueue,
    flags: u64,
}

impl BucketLock {
    unsafe fn lock(queue: *mut WaitQueue) -> Self {
        Self {
            queue,
            flags: unsafe { a20_futex_bucket_lock(queue) },
        }
    }
}

impl Drop for BucketLock {
    fn drop(&mut self) {
        unsafe { a20_futex_bucket_unlock(self.queue, self.flags) };
    }
}

struct ParkToken {
    token: ProcWaitToken,
    active: bool,
}

impl ParkToken {
    fn commit(mut self) -> i32 {
        let reason = unsafe { proc_park_commit(self.token) };
        unsafe { proc_park_finish(self.token) };
        self.active = false;
        reason
    }
}

impl Drop for ParkToken {
    fn drop(&mut self) {
        if self.active {
            unsafe {
                let _ = proc_park_cancel(self.token);
                proc_park_finish(self.token);
            }
        }
    }
}

unsafe extern "C" {
    fn a20_futex_current_task() -> *mut c_void;
    fn a20_futex_task_mm(task: *mut c_void) -> *mut c_void;
    fn a20_futex_preflight_word(addr: *const i32, out: *mut i32) -> i32;
    fn a20_futex_mapping_lock(
        task: *mut c_void,
        addr: *mut i32,
        word: *mut *mut i32,
        pkey: *mut usize,
        flags: *mut u64,
    ) -> i32;
    fn a20_futex_mapping_unlock(task: *mut c_void, flags: u64);
    fn a20_futex_bucket(index: u32) -> *mut WaitQueue;
    fn a20_futex_bucket_lock(queue: *mut WaitQueue) -> u64;
    fn a20_futex_bucket_unlock(queue: *mut WaitQueue, flags: u64);
    fn a20_futex_wait_link_locked(
        queue: *mut WaitQueue,
        entry: *mut WaitQueueEntry,
        token: ProcWaitToken,
    ) -> bool;
    fn a20_futex_wait_unlink(queue: *mut WaitQueue, entry: *mut WaitQueueEntry);
    fn proc_park_prepare(mode: i32, until: u64) -> ProcWaitToken;
    fn proc_park_cancel(token: ProcWaitToken) -> i32;
    fn proc_park_commit(token: ProcWaitToken) -> i32;
    fn proc_park_finish(token: ProcWaitToken);
    fn a20_futex_signal_unblocked(task: *mut c_void) -> bool;
    fn a20_futex_wake_is_interrupt(reason: i32) -> bool;
    fn a20_futex_entry_task_dead(entry: *const WaitQueueEntry) -> bool;
    fn a20_futex_requeue_matching(
        source: *mut WaitQueue,
        target: *mut WaitQueue,
        matcher: unsafe extern "C" fn(*const WaitQueueEntry, *mut c_void) -> bool,
        match_arg: *mut c_void,
        limit: u32,
        rekey: unsafe extern "C" fn(*mut WaitQueueEntry, *mut c_void),
        rekey_arg: *mut c_void,
    ) -> u32;
    fn timer_get_ticks() -> u64;
}

fn bucket_index(key: usize) -> usize {
    let mut h = (key as u64).wrapping_mul(0xC2B2_AE3D_27D4_EB4F);
    h ^= h >> 33;
    (h as usize) & (BUCKETS - 1)
}

#[no_mangle]
pub extern "C" fn a20_futex_bucket_index(vkey: usize, pkey: usize) -> u32 {
    bucket_index(if pkey != 0 { pkey } else { vkey }) as u32
}

#[no_mangle]
pub unsafe extern "C" fn a20_futex_wake_match(
    entry: *const WaitQueueEntry,
    opaque_arg: *mut c_void,
) -> bool {
    if entry.is_null() || opaque_arg.is_null() {
        return false;
    }
    let e = unsafe { &*entry };
    let Some(w) = (unsafe { e.priv_data.cast::<FutexWaitKey>().as_ref() }) else {
        return false;
    };
    let a = unsafe { &*opaque_arg.cast::<FutexWakeArg>() };
    if unsafe { a20_futex_entry_task_dead(entry) } || w.bitset & a.bitset == 0 {
        return false;
    }
    (w.mm == a.mm && w.vkey == a.vkey) || (a.pkey != 0 && w.pkey == a.pkey)
}

#[no_mangle]
pub unsafe extern "C" fn a20_futex_requeue_rekey(
    entry: *mut WaitQueueEntry,
    opaque_arg: *mut c_void,
) {
    if entry.is_null() || opaque_arg.is_null() {
        return;
    }
    let e = unsafe { &mut *entry };
    let Some(w) = (unsafe { e.priv_data.cast::<FutexWaitKey>().as_mut() }) else {
        return;
    };
    let r = unsafe { &*opaque_arg.cast::<FutexRequeueArg>() };
    e.key = r.vkey2;
    w.vkey = r.vkey2;
    w.pkey = r.pkey2;
    w.mm = r.mm;
}

/// Move matching waiters according to FUTEX_REQUEUE key policy. The C bridge
/// supplies the ordered two-queue lock operation; Rust owns which source
/// buckets are eligible and how each waiter is rekeyed.
#[no_mangle]
pub unsafe extern "C" fn a20_futex_requeue_transaction(
    source_index: u32,
    target_index: u32,
    match_arg: *mut c_void,
    rekey_arg: *mut c_void,
    limit: u32,
    sweep_other_buckets: bool,
) -> u32 {
    if limit == 0 || match_arg.is_null() || rekey_arg.is_null() {
        return 0;
    }
    let source = unsafe { a20_futex_bucket(source_index) };
    let target = unsafe { a20_futex_bucket(target_index) };
    let mut moved = unsafe {
        a20_futex_requeue_matching(
            source,
            target,
            a20_futex_wake_match,
            match_arg,
            limit,
            a20_futex_requeue_rekey,
            rekey_arg,
        )
    };
    if sweep_other_buckets {
        for index in 0..BUCKETS as u32 {
            if index == source_index || moved >= limit {
                continue;
            }
            let bucket = unsafe { a20_futex_bucket(index) };
            moved += unsafe {
                a20_futex_requeue_matching(
                    bucket,
                    target,
                    a20_futex_wake_match,
                    match_arg,
                    limit - moved,
                    a20_futex_requeue_rekey,
                    rekey_arg,
                )
            };
        }
    }
    moved
}

/// Implements the futex wait linearization and cleanup as one scoped state
/// transaction. A stack waiter remains alive across park/commit and Drop
/// unlinks it on normal wake, timeout, signal, or any early return.
#[no_mangle]
pub unsafe extern "C" fn a20_futex_wait_ticks_rust(
    addr: *mut i32,
    expected: i32,
    ticks: u64,
    bitset: u32,
    private: i32,
) -> i32 {
    if addr.is_null() {
        return -EFAULT;
    }
    if (addr as usize) & (core::mem::size_of::<i32>() - 1) != 0 || bitset == 0 {
        return -EINVAL;
    }
    let task = unsafe { a20_futex_current_task() };
    if task.is_null() {
        return -ESRCH;
    }
    let mut probe = 0i32;
    let preflight = unsafe { a20_futex_preflight_word(addr, &mut probe) };
    if preflight < 0 {
        return preflight;
    }
    if probe != expected {
        return -EAGAIN;
    }

    let until = if ticks == 0 {
        0
    } else {
        unsafe { timer_get_ticks() }.saturating_add(ticks)
    };
    let token = unsafe { proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, until) };
    if token.task.is_null() {
        return -EAGAIN;
    }
    let park = ParkToken {
        token,
        active: true,
    };

    let mut word: *mut i32 = ptr::null_mut();
    let mut pkey = 0usize;
    let mut mm_flags = 0u64;
    let map = unsafe { a20_futex_mapping_lock(task, addr, &mut word, &mut pkey, &mut mm_flags) };
    if map < 0 || word.is_null() {
        if map >= 0 {
            unsafe { a20_futex_mapping_unlock(task, mm_flags) };
        }
        return if map < 0 { map } else { -EFAULT };
    }
    let mapping = MappingPin {
        task,
        flags: mm_flags,
    };
    if unsafe { AtomicI32::from_ptr(word).load(Ordering::Acquire) } != expected {
        return -EAGAIN;
    }
    if unsafe { a20_futex_signal_unblocked(task) } {
        return -ERESTARTSYS;
    }

    let vkey = addr as usize;
    if private != 0 {
        pkey = 0;
    }
    let key = if pkey != 0 { pkey } else { vkey };
    let queue = unsafe { a20_futex_bucket(bucket_index(key) as u32) };
    let bucket_lock = unsafe { BucketLock::lock(queue) };
    if unsafe { AtomicI32::from_ptr(word).load(Ordering::Acquire) } != expected {
        return -EAGAIN;
    }
    if unsafe { a20_futex_signal_unblocked(task) } {
        return -ERESTARTSYS;
    }

    let mut waiter = FutexWaiter {
        entry: WaitQueueEntry {
            next: ptr::null_mut(),
            prev: ptr::null_mut(),
            queue: ptr::null_mut(),
            task: ptr::null_mut(),
            wait_seq: 0,
            flags: 0,
            key: vkey,
            priv_data: ptr::null_mut(),
            linked: false,
        },
        key: FutexWaitKey {
            vkey,
            pkey,
            mm: unsafe { a20_futex_task_mm(task) },
            bitset,
        },
    };
    waiter.entry.priv_data = (&mut waiter.key as *mut FutexWaitKey).cast();
    let linked = unsafe { a20_futex_wait_link_locked(queue, &mut waiter.entry, park.token) };
    drop(bucket_lock);
    drop(mapping);
    if !linked {
        return -ESRCH;
    }

    let linked_guard = LinkedWaiter {
        queue,
        entry: &mut waiter.entry,
    };
    let reason = park.commit();
    drop(linked_guard);

    if unsafe { a20_futex_wake_is_interrupt(reason) }
        || unsafe { a20_futex_signal_unblocked(task) }
    {
        return -ERESTARTSYS;
    }
    if reason == PROC_WAKE_TIMEOUT {
        return -ETIMEDOUT;
    }
    0
}
