//! Service-watch control flow kept separate from filesystem teardown policy.
//!
//! A supervisor crash request is deliberate fault injection. It must preserve
//! the historical exit status and leave the mounted filesystem dirty; only a
//! closed UXFS peer is allowed to take the graceful unmount path.

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum WatchAction {
    Continue,
    CrashExit,
}

pub const SVCMGR_CRASH_EXIT: i32 = 42;

pub const fn action_for_supervision_result(result: i32) -> WatchAction {
    if result == 2 { WatchAction::CrashExit } else { WatchAction::Continue }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn crash_request_is_not_graceful_exit() {
        assert_eq!(action_for_supervision_result(2), WatchAction::CrashExit);
        assert_eq!(SVCMGR_CRASH_EXIT, 42);
    }

    #[test]
    fn ordinary_supervision_results_keep_serving() {
        for result in [-1, 0, 1, 3] {
            assert_eq!(action_for_supervision_result(result), WatchAction::Continue);
        }
    }
}
