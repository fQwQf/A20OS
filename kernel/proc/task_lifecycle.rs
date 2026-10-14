#![no_std]

//! Task construction/publication phase transitions.
//!
//! C owns the task-list, park and runqueue locks and performs resource setup.
//! This module decides which lifecycle edges are legal while those existing
//! locks are held. A task is not globally visible until INIT -> PUBLISHED.

const PHASE_ALLOCATED: u32 = 1;
const PHASE_INITIALIZING: u32 = 2;
const PHASE_PUBLISHED: u32 = 3;
const PHASE_RUNNABLE: u32 = 4;
const PHASE_DESTROYING: u32 = 5;

const EVENT_BEGIN_INIT: u32 = 1;
const EVENT_PUBLISH: u32 = 2;
const EVENT_READY: u32 = 3;
const EVENT_DESTROY: u32 = 4;

fn next_phase(phase: u32, event: u32) -> Option<u32> {
    match (phase, event) {
        (PHASE_ALLOCATED, EVENT_BEGIN_INIT) => Some(PHASE_INITIALIZING),
        (PHASE_INITIALIZING, EVENT_PUBLISH) => Some(PHASE_PUBLISHED),
        (PHASE_PUBLISHED, EVENT_READY) => Some(PHASE_RUNNABLE),
        // proc_make_ready can be called again for an already published task;
        // the scheduler still performs its ordinary state/runqueue checks.
        (PHASE_RUNNABLE, EVENT_READY) => Some(PHASE_RUNNABLE),
        (PHASE_ALLOCATED | PHASE_INITIALIZING | PHASE_PUBLISHED | PHASE_RUNNABLE,
         EVENT_DESTROY) => Some(PHASE_DESTROYING),
        _ => None,
    }
}

/// # Safety
/// `phase` must point to a live, aligned task lifecycle phase word. The caller
/// serializes access using the lock documented by the C adapter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn a20_task_lifecycle_advance(
    phase: *mut u32,
    event: u32,
) -> i32 {
    if phase.is_null() || (phase as usize) % core::mem::align_of::<u32>() != 0 {
        return -22;
    }
    let current = unsafe { core::ptr::read(phase) };
    let Some(next) = next_phase(current, event) else {
        return -22;
    };
    unsafe { core::ptr::write(phase, next) };
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn successful_task_publication_and_ready_path() {
        assert_eq!(next_phase(PHASE_ALLOCATED, EVENT_BEGIN_INIT), Some(PHASE_INITIALIZING));
        assert_eq!(next_phase(PHASE_INITIALIZING, EVENT_PUBLISH), Some(PHASE_PUBLISHED));
        assert_eq!(next_phase(PHASE_PUBLISHED, EVENT_READY), Some(PHASE_RUNNABLE));
        assert_eq!(next_phase(PHASE_RUNNABLE, EVENT_READY), Some(PHASE_RUNNABLE));
    }

    #[test]
    fn failed_construction_can_be_destroyed_from_each_owned_phase() {
        for phase in [PHASE_ALLOCATED, PHASE_INITIALIZING, PHASE_PUBLISHED, PHASE_RUNNABLE] {
            assert_eq!(next_phase(phase, EVENT_DESTROY), Some(PHASE_DESTROYING));
        }
    }

    #[test]
    fn publication_and_ready_cannot_be_skipped_or_reversed() {
        assert_eq!(next_phase(PHASE_ALLOCATED, EVENT_PUBLISH), None);
        assert_eq!(next_phase(PHASE_INITIALIZING, EVENT_READY), None);
        assert_eq!(next_phase(PHASE_DESTROYING, EVENT_READY), None);
        assert_eq!(next_phase(PHASE_RUNNABLE, EVENT_PUBLISH), None);
        assert_eq!(next_phase(PHASE_DESTROYING, EVENT_DESTROY), None);
    }

    #[test]
    fn ffi_updates_only_a_valid_phase_word() {
        let mut phase = PHASE_ALLOCATED;
        unsafe {
            assert_eq!(a20_task_lifecycle_advance(&mut phase, EVENT_BEGIN_INIT), 0);
            assert_eq!(phase, PHASE_INITIALIZING);
            assert_eq!(a20_task_lifecycle_advance(core::ptr::null_mut(), EVENT_PUBLISH), -22);
            assert_eq!(a20_task_lifecycle_advance(&mut phase, EVENT_READY), -22);
        }
        assert_eq!(phase, PHASE_INITIALIZING);
    }
}
