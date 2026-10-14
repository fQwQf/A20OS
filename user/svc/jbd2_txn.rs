//! Rust-owned bounded registry for ext4's currently open JBD2 transaction.
//!
//! This owns transaction membership and its memory lifetime. The C ext4
//! journal remains responsible for block I/O, checksum encoding and replay.

use core::ffi::c_void;

#[cfg(not(test))]
unsafe extern "C" {
    fn a20_malloc(size: u64) -> *mut c_void;
    fn a20_free(ptr: *mut c_void);
}

#[cfg(test)]
unsafe fn alloc(size: usize) -> *mut c_void {
    use std::alloc::{alloc, Layout};
    let layout = Layout::from_size_align(size, 8).unwrap();
    unsafe { alloc(layout).cast() }
}

#[cfg(test)]
unsafe fn free(ptr: *mut c_void, size: usize) {
    use std::alloc::{dealloc, Layout};
    let layout = Layout::from_size_align(size, 8).unwrap();
    unsafe { dealloc(ptr.cast(), layout) };
}

#[cfg(not(test))]
unsafe fn alloc(size: usize) -> *mut c_void {
    unsafe { a20_malloc(size as u64) }
}

#[cfg(not(test))]
unsafe fn free(ptr: *mut c_void, _size: usize) {
    unsafe { a20_free(ptr) }
}

#[repr(C)]
struct JournalTxn {
    blocks: *mut u64,
    count: u32,
    capacity: u32,
    phase: u32,
}

const COLLECTING: u32 = 0;
const WRITING_LOG: u32 = 1;
const COMMITTED_NEEDS_CHECKPOINT: u32 = 2;
const CHECKPOINTED_NEEDS_LOG_CLEAR: u32 = 3;
const LOG_CLEARED_NEEDS_RECOVER_CLEAR: u32 = 4;
const FAULTED: u32 = 5;

const ENOSPC: i32 = -28;
const EINVAL: i32 = -22;

#[no_mangle]
pub extern "C" fn a20_jbd2_txn_new(capacity: u32) -> *mut c_void {
    if capacity == 0 || (capacity as usize) > usize::MAX / core::mem::size_of::<u64>() {
        return core::ptr::null_mut();
    }
    let bytes = capacity as usize * core::mem::size_of::<u64>();
    let blocks = unsafe { alloc(bytes) }.cast::<u64>();
    if blocks.is_null() {
        return core::ptr::null_mut();
    }
    let txn = unsafe { alloc(core::mem::size_of::<JournalTxn>()) }.cast::<JournalTxn>();
    if txn.is_null() {
        unsafe { free(blocks.cast(), bytes) };
        return core::ptr::null_mut();
    }
    unsafe {
        txn.write(JournalTxn { blocks, count: 0, capacity, phase: COLLECTING });
    }
    txn.cast()
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_free(ptr: *mut c_void) {
    let txn = ptr.cast::<JournalTxn>();
    if txn.is_null() { return; }
    let capacity = unsafe { (*txn).capacity as usize };
    let blocks = unsafe { (*txn).blocks };
    if !blocks.is_null() {
        unsafe { free(blocks.cast(), capacity * core::mem::size_of::<u64>()) };
    }
    unsafe { free(txn.cast(), core::mem::size_of::<JournalTxn>()) };
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_record(ptr: *mut c_void, block: u64) -> i32 {
    let txn = ptr.cast::<JournalTxn>();
    if txn.is_null() { return EINVAL; }
    let txn = unsafe { &mut *txn };
    if txn.phase != COLLECTING { return EINVAL; }
    for i in 0..txn.count as usize {
        if unsafe { *txn.blocks.add(i) } == block { return 0; }
    }
    if txn.count == txn.capacity { return ENOSPC; }
    unsafe { *txn.blocks.add(txn.count as usize) = block; }
    txn.count += 1;
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_count(ptr: *const c_void) -> u32 {
    if ptr.is_null() { return 0; }
    unsafe { (*ptr.cast::<JournalTxn>()).count }
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_block(ptr: *const c_void, index: u32,
                                              out: *mut u64) -> i32 {
    if ptr.is_null() || out.is_null() { return EINVAL; }
    let txn = unsafe { &*ptr.cast::<JournalTxn>() };
    if index >= txn.count { return EINVAL; }
    unsafe { *out = *txn.blocks.add(index as usize); }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_clear(ptr: *mut c_void) -> i32 {
    if ptr.is_null() { return EINVAL; }
    let txn = unsafe { &mut *ptr.cast::<JournalTxn>() };
    if txn.phase != LOG_CLEARED_NEEDS_RECOVER_CLEAR { return EINVAL; }
    txn.count = 0;
    txn.phase = COLLECTING;
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_phase(ptr: *const c_void) -> u32 {
    if ptr.is_null() { return u32::MAX; }
    unsafe { (*ptr.cast::<JournalTxn>()).phase }
}

/// Start a transaction commit. Returns 0 when log writing is needed, 1 when
/// the durable log must be checkpointed, and a negative errno on invalid state.
#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_start_commit(ptr: *mut c_void) -> i32 {
    if ptr.is_null() { return EINVAL; }
    let txn = unsafe { &mut *ptr.cast::<JournalTxn>() };
    match txn.phase {
        FAULTED => -5,
        COLLECTING if txn.count != 0 => { txn.phase = WRITING_LOG; 0 }
        WRITING_LOG => 0,
        COMMITTED_NEEDS_CHECKPOINT => 1,
        CHECKPOINTED_NEEDS_LOG_CLEAR => 1,
        LOG_CLEARED_NEEDS_RECOVER_CLEAR => 1,
        _ => EINVAL,
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_poison(ptr: *mut c_void) -> i32 {
    if ptr.is_null() { return EINVAL; }
    let txn = unsafe { &mut *ptr.cast::<JournalTxn>() };
    if txn.phase != COLLECTING { return EINVAL; }
    txn.phase = FAULTED;
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_mark_committed(ptr: *mut c_void) -> i32 {
    if ptr.is_null() { return EINVAL; }
    let txn = unsafe { &mut *ptr.cast::<JournalTxn>() };
    if txn.phase != WRITING_LOG { return EINVAL; }
    txn.phase = COMMITTED_NEEDS_CHECKPOINT;
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_mark_checkpointed(ptr: *mut c_void) -> i32 {
    if ptr.is_null() { return EINVAL; }
    let txn = unsafe { &mut *ptr.cast::<JournalTxn>() };
    if txn.phase != COMMITTED_NEEDS_CHECKPOINT { return EINVAL; }
    txn.phase = CHECKPOINTED_NEEDS_LOG_CLEAR;
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_jbd2_txn_mark_log_cleared(ptr: *mut c_void) -> i32 {
    if ptr.is_null() { return EINVAL; }
    let txn = unsafe { &mut *ptr.cast::<JournalTxn>() };
    if txn.phase != CHECKPOINTED_NEEDS_LOG_CLEAR { return EINVAL; }
    txn.phase = LOG_CLEARED_NEEDS_RECOVER_CLEAR;
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn owns_unique_ordered_bounded_block_set_and_clear() {
        let txn = a20_jbd2_txn_new(2);
        assert!(!txn.is_null());
        unsafe {
            assert_eq!(a20_jbd2_txn_record(txn, 41), 0);
            assert_eq!(a20_jbd2_txn_record(txn, 41), 0);
            assert_eq!(a20_jbd2_txn_record(txn, 99), 0);
            assert_eq!(a20_jbd2_txn_count(txn), 2);
            assert_eq!(a20_jbd2_txn_record(txn, 100), ENOSPC);
            let mut block = 0;
            assert_eq!(a20_jbd2_txn_block(txn, 0, &mut block), 0);
            assert_eq!(block, 41);
            assert_eq!(a20_jbd2_txn_block(txn, 1, &mut block), 0);
            assert_eq!(block, 99);
            assert_eq!(a20_jbd2_txn_start_commit(txn), 0);
            assert_eq!(a20_jbd2_txn_mark_committed(txn), 0);
            assert_eq!(a20_jbd2_txn_mark_checkpointed(txn), 0);
            assert_eq!(a20_jbd2_txn_mark_log_cleared(txn), 0);
            assert_eq!(a20_jbd2_txn_clear(txn), 0);
            assert_eq!(a20_jbd2_txn_count(txn), 0);
            a20_jbd2_txn_free(txn);
        }
    }

    #[test]
    fn durable_commit_retries_checkpoint_without_rewriting_or_dropping_blocks() {
        let txn = a20_jbd2_txn_new(2);
        unsafe {
            assert_eq!(a20_jbd2_txn_record(txn, 7), 0);
            assert_eq!(a20_jbd2_txn_start_commit(txn), 0);
            assert_eq!(a20_jbd2_txn_start_commit(txn), 0); // pre-commit I/O retry
            assert_eq!(a20_jbd2_txn_mark_committed(txn), 0);
            assert_eq!(a20_jbd2_txn_start_commit(txn), 1); // checkpoint-only retry
            assert_eq!(a20_jbd2_txn_count(txn), 1);
            assert_eq!(a20_jbd2_txn_mark_checkpointed(txn), 0);
            assert_eq!(a20_jbd2_txn_mark_log_cleared(txn), 0);
            assert_eq!(a20_jbd2_txn_clear(txn), 0);
            assert_eq!(a20_jbd2_txn_count(txn), 0);
            a20_jbd2_txn_free(txn);
        }
    }

    #[test]
    fn poisoned_partial_transaction_keeps_its_registry_and_cannot_commit() {
        let txn = a20_jbd2_txn_new(2);
        unsafe {
            assert_eq!(a20_jbd2_txn_record(txn, 5), 0);
            assert_eq!(a20_jbd2_txn_poison(txn), 0);
            assert_eq!(a20_jbd2_txn_record(txn, 6), EINVAL);
            assert_eq!(a20_jbd2_txn_start_commit(txn), -5);
            assert_eq!(a20_jbd2_txn_count(txn), 1);
            a20_jbd2_txn_free(txn);
        }
    }
}
