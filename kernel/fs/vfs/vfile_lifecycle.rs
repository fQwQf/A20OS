// Rust-owned backend-close state transitions for live vfiles.
const EIO: i32 = -5;
const EALREADY: i32 = -114;
const EBUSY: i32 = -16;
const EINVAL: i32 = -22;

const OPEN: u32 = 0;
const RETRYABLE: u32 = 1;
const AMBIGUOUS: u32 = 2;
const DONE: u32 = 3;
const RUNNING: u32 = 4;

fn close_begin_impl(state: &core::sync::atomic::AtomicU32,
                    token_capable: i32) -> i32 {
    loop {
        let old = state.load(core::sync::atomic::Ordering::Acquire);
        match old {
            OPEN | RETRYABLE => {
                if state.compare_exchange(old, RUNNING,
                    core::sync::atomic::Ordering::AcqRel,
                    core::sync::atomic::Ordering::Acquire).is_ok() { return 0; }
            }
            AMBIGUOUS => {
                /* A backend carrying an idempotent close token may retry an
                 * un-acked sent CLOSE: the server dedups the replay. Without
                 * the capability AMBIGUOUS stays sticky -EIO. */
                if token_capable == 0 { return EIO; }
                if state.compare_exchange(old, RUNNING,
                    core::sync::atomic::Ordering::AcqRel,
                    core::sync::atomic::Ordering::Acquire).is_ok() { return 0; }
            }
            DONE => return EALREADY,
            RUNNING => return EBUSY,
            _ => return EINVAL,
        }
    }
}

/// 0 means invoke the backend, 1 means already closed, negative forbids retry.
#[no_mangle]
/// # Safety
/// `state` must point to a live, aligned `AtomicU32` field for the duration of
/// the call. The caller must hold that vfile's close mutex from begin through
/// finish, so only one backend-close callback is active for the state.
pub unsafe extern "C" fn a20_vfile_close_begin(state: *mut u32) -> i32 {
    if state.is_null() { return EINVAL; }
    let state = unsafe { &*(state as *const core::sync::atomic::AtomicU32) };
    close_begin_impl(state, 0)
}

/// Token-aware variant of `a20_vfile_close_begin`: identical except that from
/// AMBIGUOUS a nonzero `token_capable` allows one more attempt (RUNNING).
/// Passing `token_capable == 0` is bit-exact with `a20_vfile_close_begin`.
#[no_mangle]
/// # Safety
/// Same pointer/lifetime contract as `a20_vfile_close_begin`.
pub unsafe extern "C" fn a20_vfile_close_begin_token(state: *mut u32,
                                                     token_capable: i32) -> i32 {
    if state.is_null() { return EINVAL; }
    let state = unsafe { &*(state as *const core::sync::atomic::AtomicU32) };
    close_begin_impl(state, token_capable)
}

/// Record one backend attempt. `sent` is true only after request enqueue;
/// `acked` requires a matching successful response. A known pre-send failure
/// can retry. Any uncertain post-send result remains sticky unless the
/// backend carries an idempotent close token (see
/// `a20_vfile_close_begin_token`).
#[no_mangle]
/// # Safety
/// `state` must satisfy `a20_vfile_close_begin`'s pointer/lifetime contract,
/// and a successful begin for this attempt must still own the close mutex.
pub unsafe extern "C" fn a20_vfile_close_finish(
    state: *mut u32, status: i32, sent: i32, acked: i32, retry_safe: i32,
) -> i32 {
    if state.is_null() { return EINVAL; }
    let state = unsafe { &*(state as *const core::sync::atomic::AtomicU32) };
    if state.load(core::sync::atomic::Ordering::Acquire) != RUNNING { return EBUSY; }
    if status == 0 && acked != 0 {
        state.store(DONE, core::sync::atomic::Ordering::Release);
        return 0;
    }
    if sent == 0 && retry_safe != 0 {
        state.store(RETRYABLE, core::sync::atomic::Ordering::Release);
        return if status < 0 { status } else { EIO };
    }
    state.store(AMBIGUOUS, core::sync::atomic::Ordering::Release);
    if status < 0 { status } else { EIO }
}

#[no_mangle]
/// # Safety
/// `state` must point to a live, aligned `AtomicU32` field.
pub unsafe extern "C" fn a20_vfile_close_is_done(state: *const u32) -> i32 {
    if state.is_null() { return 0; }
    let state = unsafe { &*(state as *const core::sync::atomic::AtomicU32) };
    (state.load(core::sync::atomic::Ordering::Acquire) == DONE) as i32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pre_send_failure_retries_but_sent_failure_is_sticky() {
        unsafe {
            let s = core::sync::atomic::AtomicU32::new(OPEN);
            let p = s.as_ptr() as *mut u32;
            assert_eq!(a20_vfile_close_begin(p), 0);
            assert_eq!(a20_vfile_close_finish(p, -5, 0, 0, 1), -5);
            assert_eq!(s.load(core::sync::atomic::Ordering::Acquire), RETRYABLE);
            assert_eq!(a20_vfile_close_begin(p), 0);
            assert_eq!(a20_vfile_close_finish(p, -5, 1, 0, 0), -5);
            assert_eq!(s.load(core::sync::atomic::Ordering::Acquire), AMBIGUOUS);
            assert_eq!(a20_vfile_close_begin(p), -5);
        }
    }

    #[test]
    fn token_capable_ambiguous_close_retries_to_done() {
        unsafe {
            let s = core::sync::atomic::AtomicU32::new(OPEN);
            let p = s.as_ptr() as *mut u32;
            assert_eq!(a20_vfile_close_begin_token(p, 1), 0);
            // Sent but un-acked: ambiguous, yet retryable with a token.
            assert_eq!(a20_vfile_close_finish(p, -5, 1, 0, 0), -5);
            assert_eq!(s.load(core::sync::atomic::Ordering::Acquire), AMBIGUOUS);
            assert_eq!(a20_vfile_close_begin(p), EIO); // legacy begin stays sticky
            assert_eq!(a20_vfile_close_begin_token(p, 0), EIO); // no capability: sticky
            assert_eq!(a20_vfile_close_begin_token(p, 1), 0);
            assert_eq!(s.load(core::sync::atomic::Ordering::Acquire), RUNNING);
            // The replay is deduplicated server-side; the ACK completes it.
            assert_eq!(a20_vfile_close_finish(p, 0, 1, 1, 0), 0);
            assert_eq!(s.load(core::sync::atomic::Ordering::Acquire), DONE);
            assert_eq!(a20_vfile_close_is_done(p), 1);
            assert_eq!(a20_vfile_close_begin_token(p, 1), EALREADY);
        }
    }

    #[test]
    fn token_capable_retry_can_fail_ambiguous_again() {
        unsafe {
            let s = core::sync::atomic::AtomicU32::new(AMBIGUOUS);
            let p = s.as_ptr() as *mut u32;
            assert_eq!(a20_vfile_close_begin_token(p, 1), 0);
            // A retry that is itself sent-but-un-acked returns to AMBIGUOUS;
            // the token still permits a later attempt.
            assert_eq!(a20_vfile_close_finish(p, -5, 1, 0, 0), -5);
            assert_eq!(s.load(core::sync::atomic::Ordering::Acquire), AMBIGUOUS);
            assert_eq!(a20_vfile_close_begin_token(p, 1), 0);
            assert_eq!(a20_vfile_close_finish(p, 0, 1, 1, 0), 0);
            assert_eq!(s.load(core::sync::atomic::Ordering::Acquire), DONE);
        }
    }

    #[test]
    fn successful_ack_is_terminal_and_nonzero_status_is_not_ack_success() {
        unsafe {
            let s = core::sync::atomic::AtomicU32::new(OPEN);
            let p = s.as_ptr() as *mut u32;
            assert_eq!(a20_vfile_close_begin(p), 0);
            assert_eq!(a20_vfile_close_finish(p, 0, 1, 1, 0), 0);
            assert_eq!(s.load(core::sync::atomic::Ordering::Acquire), DONE);
            assert_eq!(a20_vfile_close_is_done(p), 1);
            assert_eq!(a20_vfile_close_begin(p), EALREADY);
            let failed = core::sync::atomic::AtomicU32::new(OPEN);
            let p = failed.as_ptr() as *mut u32;
            assert_eq!(a20_vfile_close_begin(p), 0);
            assert_eq!(a20_vfile_close_finish(p, -5, 1, 1, 0), -5);
            assert_eq!(failed.load(core::sync::atomic::Ordering::Acquire), AMBIGUOUS);
        }
    }
}
