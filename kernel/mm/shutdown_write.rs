#![no_std]

//! Admission state for shared-file writable access during filesystem shutdown.
//!
//! This module owns only the admission/generation decisions. Callers must
//! bracket each operation that can grant a writable file mapping or write
//! through a pinned shared-file leaf. The count is an admission/lifetime
//! barrier, not a lock: callers may be preempted or allocate while counted,
//! but must not wait for filesystem I/O, shutdown completion, or any resource
//! whose release depends on shutdown. Waiting to enter must happen in a
//! schedulable context with MM/page-table/page-cache/VFS locks released.
//! Page-table edits, mm references, cache dirtying, and TLB shootdowns remain
//! C primitives.

#[cfg(test)]
extern crate std;

use core::sync::atomic::{AtomicU32, Ordering};

const CLOSED: u32 = 1 << 31;
const COMMITTED: u32 = 1 << 30;
const COUNT_MASK: u32 = COMMITTED - 1;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum AdmissionError {
    Closed,
    Overflow,
    InvalidToken,
    Busy,
}

struct Gate {
    state: AtomicU32,
    generation: AtomicU32,
    control: AtomicU32,
}

impl Gate {
    const fn new() -> Self {
        Self {
            state: AtomicU32::new(0),
            generation: AtomicU32::new(0),
            control: AtomicU32::new(0),
        }
    }

    fn with_control<T>(&self, f: impl FnOnce() -> T) -> Result<T, AdmissionError> {
        self.control.compare_exchange(
            0, 1, Ordering::Acquire, Ordering::Relaxed,
        ).map_err(|_| AdmissionError::Busy)?;
        let result = f();
        self.control.store(0, Ordering::Release);
        Ok(result)
    }

    fn enter(&self) -> Result<(), AdmissionError> {
        let mut old = self.state.load(Ordering::Acquire);
        loop {
            if old & CLOSED != 0 {
                return Err(AdmissionError::Closed);
            }
            let count = old & COUNT_MASK;
            if count == COUNT_MASK {
                return Err(AdmissionError::Overflow);
            }
            match self.state.compare_exchange_weak(
                old,
                old + 1,
                Ordering::AcqRel,
                Ordering::Acquire,
            ) {
                Ok(_) => return Ok(()),
                Err(now) => old = now,
            }
        }
    }

    fn leave(&self) -> Result<(), AdmissionError> {
        let mut old = self.state.load(Ordering::Acquire);
        loop {
            if old & COUNT_MASK == 0 || old & COMMITTED != 0 {
                return Err(AdmissionError::InvalidToken);
            }
            match self.state.compare_exchange_weak(
                old,
                old - 1,
                Ordering::Release,
                Ordering::Acquire,
            ) {
                Ok(_) => return Ok(()),
                Err(now) => old = now,
            }
        }
    }

    fn begin(&self) -> Result<u64, AdmissionError> {
        self.with_control(|| {
            if self.generation.load(Ordering::Acquire) == u32::MAX {
                return Err(AdmissionError::Overflow);
            }
            let mut old = self.state.load(Ordering::Acquire);
            loop {
                if old & CLOSED != 0 {
                    return Err(AdmissionError::Closed);
                }
                match self.state.compare_exchange_weak(
                    old,
                    old | CLOSED,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                ) {
                    Ok(_) => break,
                    Err(now) => old = now,
                }
            }

            let generation = self.generation.fetch_add(1, Ordering::AcqRel) + 1;
            // The outer VFS transaction waits for drain in a schedulable context.
            Ok(generation as u64)
        })?
    }

    fn drained(&self, token: u64) -> Result<bool, AdmissionError> {
        self.with_control(|| {
            if !self.valid_token(token) {
                return Err(AdmissionError::InvalidToken);
            }
            Ok(self.state.load(Ordering::Acquire) & COUNT_MASK == 0)
        })?
    }

    fn abort(&self, token: u64) -> Result<(), AdmissionError> {
        self.with_control(|| {
            if !self.valid_token(token) || self.state.load(Ordering::Acquire) != CLOSED {
                return Err(AdmissionError::InvalidToken);
            }
            self.state.compare_exchange(
                CLOSED, 0, Ordering::AcqRel, Ordering::Acquire,
            ).map(|_| ()).map_err(|_| AdmissionError::InvalidToken)
        })?
    }

    fn commit(&self, token: u64) -> Result<(), AdmissionError> {
        self.with_control(|| {
            if !self.valid_token(token) || self.state.load(Ordering::Acquire) != CLOSED {
                return Err(AdmissionError::InvalidToken);
            }
            self.state.compare_exchange(
                CLOSED, CLOSED | COMMITTED, Ordering::AcqRel, Ordering::Acquire,
            ).map(|_| ()).map_err(|_| AdmissionError::InvalidToken)
        })?
    }

    fn valid_token(&self, token: u64) -> bool {
        token != 0
            && token <= u32::MAX as u64
            && self.generation.load(Ordering::Acquire) == token as u32
            && self.state.load(Ordering::Acquire) & CLOSED != 0
    }
}

static SHARED_WRITE_GATE: Gate = Gate::new();

/// Admit a short shared-file write or writable-PTE grant. Returns 0 if the
/// caller must leave the operation unperformed while shutdown is quiescing.
#[no_mangle]
pub extern "C" fn a20_mm_shared_write_enter_rust() -> i32 {
    match SHARED_WRITE_GATE.enter() {
        Ok(()) => 1,
        Err(AdmissionError::Closed) => 0,
        Err(AdmissionError::Overflow) => -75,
        Err(_) => -22,
    }
}

#[no_mangle]
pub extern "C" fn a20_mm_shared_write_leave_rust() -> i32 {
    if SHARED_WRITE_GATE.leave().is_ok() { 0 } else { -22 }
}

#[no_mangle]
pub extern "C" fn a20_mm_shared_write_is_committed_rust() -> i32 {
    i32::from(SHARED_WRITE_GATE.state.load(Ordering::Acquire) & COMMITTED != 0)
}

#[no_mangle]
pub extern "C" fn a20_mm_shared_write_is_closed_rust() -> i32 {
    i32::from(SHARED_WRITE_GATE.state.load(Ordering::Acquire) & CLOSED != 0)
}

/// Close admission and return this transaction's generation token. The caller
/// separately waits for `drained` in a schedulable context, then revokes PTEs.
#[no_mangle]
pub extern "C" fn a20_mm_shared_write_begin_rust(token_out: *mut u64) -> i32 {
    if token_out.is_null() {
        return -22;
    }
    match SHARED_WRITE_GATE.begin() {
        Ok(token) => {
            // SAFETY: the C wrapper validates a writable aligned token pointer.
            unsafe { token_out.write(token) };
            0
        }
        Err(AdmissionError::Closed) => -16,
        Err(AdmissionError::Overflow) => -75,
        Err(AdmissionError::Busy) => -16,
        Err(AdmissionError::InvalidToken) => -22,
    }
}

#[no_mangle]
pub extern "C" fn a20_mm_shared_write_drained_rust(token: u64) -> i32 {
    match SHARED_WRITE_GATE.drained(token) {
        Ok(true) => 1,
        Ok(false) => 0,
        Err(AdmissionError::Busy) => 0,
        Err(_) => -22,
    }
}

#[no_mangle]
pub extern "C" fn a20_mm_shared_write_abort_rust(token: u64) -> i32 {
    match SHARED_WRITE_GATE.abort(token) {
        Ok(()) => 0,
        Err(AdmissionError::Busy) => -16,
        Err(_) => -22,
    }
}

#[no_mangle]
pub extern "C" fn a20_mm_shared_write_commit_rust(token: u64) -> i32 {
    match SHARED_WRITE_GATE.commit(token) {
        Ok(()) => 0,
        Err(AdmissionError::Busy) => -16,
        Err(_) => -22,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;
    use std::thread;
    use std::vec::Vec;

    #[test]
    fn closes_admission_and_reopens_only_on_abort() {
        let gate = Gate::new();
        assert_eq!(gate.enter(), Ok(()));
        gate.leave().unwrap();
        let token = gate.begin().unwrap();
        assert_eq!(gate.enter(), Err(AdmissionError::Closed));
        assert_eq!(gate.abort(token), Ok(()));
        assert_eq!(gate.enter(), Ok(()));
        gate.leave().unwrap();
        assert_eq!(gate.abort(token), Err(AdmissionError::InvalidToken));
    }

    #[test]
    fn admission_retry_observes_abort_between_closed_check_and_wait() {
        let gate = Gate::new();
        let token = gate.begin().unwrap();
        assert_eq!(gate.enter(), Err(AdmissionError::Closed));
        gate.abort(token).unwrap();
        /* The C wait wrapper retries if abort reopens between the failed
         * enter and its closed-state check; it must not report overflow. */
        assert_eq!(gate.enter(), Ok(()));
        gate.leave().unwrap();
    }

    #[test]
    fn begin_drains_operations_that_entered_before_close() {
        let gate = Arc::new(Gate::new());
        gate.enter().unwrap();
        let worker_gate = gate.clone();
        let worker = thread::spawn(move || {
            while worker_gate.state.load(Ordering::Acquire) & CLOSED == 0 {
                thread::yield_now();
            }
            worker_gate.leave().unwrap();
        });
        let token = gate.begin().unwrap();
        assert!(!gate.drained(token).unwrap());
        worker.join().unwrap();
        assert_ne!(token, 0);
        assert!(gate.drained(token).unwrap());
        assert_eq!(gate.state.load(Ordering::Acquire), CLOSED);
    }

    #[test]
    fn abort_and_commit_cannot_reopen_after_commit_wins() {
        use std::sync::Barrier;
        let gate = Arc::new(Gate::new());
        let token = gate.begin().unwrap();
        let barrier = Arc::new(Barrier::new(3));
        let mut threads = Vec::new();
        for commit in [false, true] {
            let gate = gate.clone();
            let barrier = barrier.clone();
            threads.push(thread::spawn(move || {
                barrier.wait();
                if commit { gate.commit(token) } else { gate.abort(token) }
            }));
        }
        barrier.wait();
        let results: Vec<_> = threads.into_iter().map(|t| t.join().unwrap()).collect();
        assert_eq!(results.iter().filter(|r| r.is_ok()).count(), 1);
        if gate.state.load(Ordering::Acquire) & COMMITTED != 0 {
            assert_eq!(gate.enter(), Err(AdmissionError::Closed));
        } else {
            assert_eq!(gate.state.load(Ordering::Acquire), 0);
        }
    }

    #[test]
    fn commit_is_terminal_and_tokens_do_not_cross_transactions() {
        let gate = Gate::new();
        let first = gate.begin().unwrap();
        assert_eq!(gate.commit(first), Ok(()));
        assert_eq!(gate.abort(first), Err(AdmissionError::InvalidToken));
        assert_eq!(gate.begin(), Err(AdmissionError::Closed));

        let retryable = Gate::new();
        let old = retryable.begin().unwrap();
        retryable.abort(old).unwrap();
        let new = retryable.begin().unwrap();
        assert_ne!(old, new);
        assert_eq!(retryable.commit(old), Err(AdmissionError::InvalidToken));
        retryable.abort(new).unwrap();
    }

    #[test]
    fn generation_exhaustion_preserves_inflight_count_and_admission() {
        let gate = Gate::new();
        gate.generation.store(u32::MAX, Ordering::Release);
        gate.enter().unwrap();
        assert_eq!(gate.begin(), Err(AdmissionError::Overflow));
        assert_eq!(gate.state.load(Ordering::Acquire), 1);
        gate.leave().unwrap();
        assert_eq!(gate.state.load(Ordering::Acquire), 0);
    }

    #[test]
    fn contended_control_returns_instead_of_spinning() {
        let gate = Gate::new();
        gate.control.store(1, Ordering::Release);
        assert_eq!(gate.begin(), Err(AdmissionError::Busy));
        assert_eq!(gate.state.load(Ordering::Acquire), 0);
        gate.control.store(0, Ordering::Release);
        let token = gate.begin().unwrap();
        gate.abort(token).unwrap();
    }
}
