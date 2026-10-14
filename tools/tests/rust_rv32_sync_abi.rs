#![no_std]

use core::ffi::c_void;
use core::mem::{align_of, offset_of, size_of};

#[repr(C)]
struct WaitQueueEntry {
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
struct ProcWaitToken {
    task: *mut c_void,
    seq: u64,
    prepare_error: i32,
}

#[repr(C)]
struct RwMutexState {
    readers: u32,
    waiting_writers: u32,
    writer: i32,
    owner: *mut c_void,
}

#[repr(C)]
struct MutexState {
    locked: i32,
    depth: i32,
    owner: *mut c_void,
}

const _: () = {
    assert!(align_of::<WaitQueueEntry>() == 8);
    assert!(offset_of!(WaitQueueEntry, next) == 0);
    assert!(offset_of!(WaitQueueEntry, prev) == 4);
    assert!(offset_of!(WaitQueueEntry, queue) == 8);
    assert!(offset_of!(WaitQueueEntry, task) == 12);
    assert!(offset_of!(WaitQueueEntry, wait_seq) == 16);
    assert!(offset_of!(WaitQueueEntry, flags) == 24);
    assert!(offset_of!(WaitQueueEntry, key) == 28);
    assert!(offset_of!(WaitQueueEntry, priv_data) == 32);
    assert!(offset_of!(WaitQueueEntry, linked) == 36);
    assert!(size_of::<WaitQueueEntry>() == 40);

    assert!(align_of::<ProcWaitToken>() == 8);
    assert!(offset_of!(ProcWaitToken, task) == 0);
    assert!(offset_of!(ProcWaitToken, seq) == 8);
    assert!(offset_of!(ProcWaitToken, prepare_error) == 16);
    assert!(size_of::<ProcWaitToken>() == 24);

    assert!(offset_of!(RwMutexState, readers) == 0);
    assert!(offset_of!(RwMutexState, waiting_writers) == 4);
    assert!(offset_of!(RwMutexState, writer) == 8);
    assert!(offset_of!(RwMutexState, owner) == 12);
    assert!(size_of::<RwMutexState>() == 16);

    assert!(offset_of!(MutexState, locked) == 0);
    assert!(offset_of!(MutexState, depth) == 4);
    assert!(offset_of!(MutexState, owner) == 8);
    assert!(size_of::<MutexState>() == 12);
};

#[no_mangle]
pub extern "C" fn a20_rv32_sync_rust_layout_probe() -> usize {
    size_of::<WaitQueueEntry>() + size_of::<ProcWaitToken>() +
        size_of::<RwMutexState>() + size_of::<MutexState>()
}
