//! Small C ABI bridge for the host wire test. It calls the same Rust lifetime
//! helpers used by the production UFSD dispatcher.
#![allow(dead_code)]

#[path = "../../user/svc/ufs_lifetime.rs"]
mod lifetime;

use core::sync::atomic::{AtomicU32, Ordering};
use std::sync::Mutex;

static STATE: Mutex<Option<lifetime::Lifetimes>> = Mutex::new(None);
static BACKEND_CLOSES: AtomicU32 = AtomicU32::new(0);

#[no_mangle]
pub extern "C" fn ufs_close_token_host_reset(epoch: u64) {
    let mut state = lifetime::Lifetimes::new();
    state.seed_token_epoch(epoch);
    state.set_close_tokens_enabled(true);
    *STATE.lock().unwrap() = Some(state);
    BACKEND_CLOSES.store(0, Ordering::SeqCst);
}

#[no_mangle]
pub extern "C" fn ufs_close_token_host_open(ino: u64) -> u64 {
    STATE.lock().unwrap().as_mut().unwrap()
        .open_with_backend(ino, || 0).unwrap()
}

/// Return 0 for first accepted close, 1 for a replay ACK, or negative errno.
#[no_mangle]
pub extern "C" fn ufs_close_token_host_close(ino: u64, token: u64) -> i64 {
    match STATE.lock().unwrap().as_mut().unwrap().close_with_backend(
        ino, token, || { BACKEND_CLOSES.fetch_add(1, Ordering::SeqCst); 0 }) {
        Ok(lifetime::CloseOutcome::Consumed { .. }) => 0,
        Ok(lifetime::CloseOutcome::Replayed) => 1,
        Err(error) => error,
    }
}

#[no_mangle]
pub extern "C" fn ufs_close_token_host_backend_closes() -> u32 {
    BACKEND_CLOSES.load(Ordering::SeqCst)
}

#[no_mangle]
pub extern "C" fn ufs_close_token_host_confirm(ino: u64, token: u64) -> i64 {
    match STATE.lock().unwrap().as_mut().unwrap().confirm_close(ino, token) {
        Ok(()) => 0,
        Err(error) => error,
    }
}
