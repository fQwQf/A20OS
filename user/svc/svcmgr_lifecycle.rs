//! Heap-free service lifecycle policy shared with the freestanding manager.
//!
//! This module owns manager state and decisions; syscall/resource operations
//! remain behind `svcmgr_rust_abi` adapters in the production binary.

pub const RESTART_BUDGET: u32 = 5;
pub const RESTART_WINDOW_MS: u64 = 30_000;
pub const PING_PERIOD_MS: u64 = 2_000;
pub const PING_DEADLINE_MS: u64 = 1_500;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum State {
    Boot,
    Up,
    PendingPing,
    Retiring,
    Dead,
    Failed,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum ExitDecision {
    CleanExit,
    Restart,
    RetireBudget,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Lifecycle {
    pub state: State,
    pub generation: u32,
    pub restarts: u32,
    pub window_restarts: u32,
    pub window_start_ms: u64,
    pub last_ok_ms: u64,
    pub ping_sent_ms: u64,
}

impl Lifecycle {
    pub const fn boot() -> Self {
        Self {
            state: State::Boot,
            generation: 0,
            restarts: 0,
            window_restarts: 0,
            window_start_ms: 0,
            last_ok_ms: 0,
            ping_sent_ms: 0,
        }
    }

    /// Return the next event generation, or None rather than wrapping into a
    /// stale event's identity space.
    pub fn next_generation(&self) -> Option<u32> {
        self.generation
            .checked_add(1)
            .filter(|generation| *generation != 0)
    }

    /// Install one freshly spawned generation after both event watches work.
    pub fn spawned(&mut self, generation: u32, now_ms: u64) -> bool {
        if self.next_generation() != Some(generation) {
            self.state = State::Failed;
            return false;
        }
        self.generation = generation;
        self.state = State::Up;
        self.last_ok_ms = now_ms;
        self.ping_sent_ms = 0;
        true
    }

    pub fn current_generation(&self, generation: u32) -> bool {
        generation != 0 && generation == self.generation
    }

    pub fn exit(&mut self, exit_code: i32, now_ms: u64) -> ExitDecision {
        self.state = State::Dead;
        self.ping_sent_ms = 0;
        if exit_code == 0 {
            return ExitDecision::CleanExit;
        }
        if self.window_start_ms == 0
            || now_ms.saturating_sub(self.window_start_ms) > RESTART_WINDOW_MS
        {
            self.window_start_ms = now_ms;
            self.window_restarts = 0;
        }
        self.window_restarts = self.window_restarts.saturating_add(1);
        if self.window_restarts > RESTART_BUDGET {
            self.state = State::Failed;
            ExitDecision::RetireBudget
        } else {
            self.restarts = self.restarts.saturating_add(1);
            ExitDecision::Restart
        }
    }

    pub fn ping_due(&self, now_ms: u64) -> bool {
        self.state == State::Up && now_ms.saturating_sub(self.last_ok_ms) > PING_PERIOD_MS
    }

    pub fn ping_sent(&mut self, now_ms: u64) {
        if self.state == State::Up {
            self.state = State::PendingPing;
            self.ping_sent_ms = now_ms;
        }
    }

    pub fn ping_expired(&self, now_ms: u64) -> bool {
        self.state == State::PendingPing
            && now_ms.saturating_sub(self.ping_sent_ms) > PING_DEADLINE_MS
    }

    pub fn pong(&mut self, now_ms: u64) {
        if self.state == State::PendingPing {
            self.state = State::Up;
            self.last_ok_ms = now_ms;
            self.ping_sent_ms = 0;
        }
    }

    pub fn retirement_started(&mut self) {
        self.state = State::Retiring;
        self.ping_sent_ms = 0;
    }

    pub fn retired(&mut self) {
        self.state = State::Failed;
        self.ping_sent_ms = 0;
    }

    /// Complete a nonblocking retirement. Health timeouts feed the confirmed
    /// exit into the ordinary restart budget exactly once; spawn rollback and
    /// manager shutdown retire without creating a replacement.
    pub fn complete_retirement(
        &mut self,
        restart_after_exit: bool,
        exit_code: i32,
        now_ms: u64,
    ) -> Option<ExitDecision> {
        // Retiring is a one-shot completion state. A duplicate/stale EXIT or
        // a poll after the completion was already consumed must not spend the
        // restart budget a second time.
        if self.state != State::Retiring {
            return None;
        }
        if restart_after_exit {
            Some(self.exit(exit_code, now_ms))
        } else {
            self.retired();
            None
        }
    }
}

// Top byte is a disjoint event kind, low 8 bits are service index, and the
// intervening 32 bits carry the complete generation without truncation.
const EXIT_TAG_KIND: u64 = 0xa1u64 << 56;
const PONG_TAG_KIND: u64 = 0xa2u64 << 56;
const TAG_KIND_MASK: u64 = 0xffu64 << 56;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub enum EventTag {
    Exit { index: u8, generation: u32 },
    Pong { index: u8, generation: u32 },
    Other,
}

pub fn exit_tag(index: u8, generation: u32) -> u64 {
    EXIT_TAG_KIND | ((generation as u64) << 8) | index as u64
}

pub fn pong_tag(index: u8, generation: u32) -> u64 {
    PONG_TAG_KIND | ((generation as u64) << 8) | index as u64
}

pub fn decode_tag(tag: u64) -> EventTag {
    let index = (tag & 0xff) as u8;
    let generation = ((tag >> 8) & 0xffff_ffff) as u32;
    match tag & TAG_KIND_MASK {
        EXIT_TAG_KIND => EventTag::Exit { index, generation },
        PONG_TAG_KIND => EventTag::Pong { index, generation },
        _ => EventTag::Other,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn generations_make_queued_old_events_stale_after_restart() {
        let mut life = Lifecycle::boot();
        assert!(life.spawned(life.next_generation().unwrap(), 1));
        let first = life.generation;
        let old_exit = exit_tag(2, first);
        let old_pong = pong_tag(2, first);
        assert_eq!(
            decode_tag(old_exit),
            EventTag::Exit {
                index: 2,
                generation: first
            }
        );
        assert_eq!(
            decode_tag(old_pong),
            EventTag::Pong {
                index: 2,
                generation: first
            }
        );
        assert_eq!(life.exit(42, 2), ExitDecision::Restart);
        assert!(life.spawned(life.next_generation().unwrap(), 52));
        assert_ne!(life.generation, first);
        let EventTag::Exit { generation, .. } = decode_tag(old_exit) else {
            panic!()
        };
        assert!(!life.current_generation(generation));
        let EventTag::Pong { generation, .. } = decode_tag(old_pong) else {
            panic!()
        };
        assert!(!life.current_generation(generation));
    }

    #[test]
    fn clean_exit_does_not_consume_restart_budget() {
        let mut life = Lifecycle::boot();
        assert!(life.spawned(life.next_generation().unwrap(), 1));
        assert_eq!(life.exit(0, 2), ExitDecision::CleanExit);
        assert_eq!(life.window_restarts, 0);
        assert_eq!(life.restarts, 0);
        assert_eq!(life.state, State::Dead);
    }

    #[test]
    fn five_crashes_restart_and_sixth_retires() {
        let mut life = Lifecycle::boot();
        for n in 1..=5 {
            assert!(life.spawned(life.next_generation().unwrap(), n as u64 * 100));
            assert_eq!(life.exit(42, n * 100 + 1), ExitDecision::Restart);
        }
        assert!(life.spawned(life.next_generation().unwrap(), 600));
        assert_eq!(life.exit(42, 601), ExitDecision::RetireBudget);
        assert_eq!(life.state, State::Failed);
        assert_eq!(life.restarts, RESTART_BUDGET);
    }

    #[test]
    fn restart_budget_window_resets_after_thirty_seconds() {
        let mut life = Lifecycle::boot();
        for n in 1..=5 {
            assert!(life.spawned(life.next_generation().unwrap(), n * 10));
            assert_eq!(life.exit(42, n * 10 + 1), ExitDecision::Restart);
        }
        assert!(life.spawned(life.next_generation().unwrap(), 30_020));
        assert_eq!(life.exit(42, 30_021), ExitDecision::Restart);
        assert_eq!(life.window_restarts, 1);
    }

    #[test]
    fn ping_deadline_and_late_pong_are_stateful() {
        let mut life = Lifecycle::boot();
        assert!(life.spawned(life.next_generation().unwrap(), 100));
        assert!(!life.ping_due(2_100));
        assert!(life.ping_due(2_101));
        life.ping_sent(2_101);
        assert!(!life.ping_expired(3_601));
        assert!(life.ping_expired(3_602));
        life.pong(3_603);
        assert_eq!(life.state, State::Up);
        assert_eq!(life.last_ok_ms, 3_603);
    }

    #[test]
    fn generation_exhaustion_fails_closed_without_reusing_tags() {
        let mut life = Lifecycle {
            generation: u32::MAX,
            ..Lifecycle::boot()
        };
        assert_eq!(life.next_generation(), None);
        assert!(!life.spawned(1, 10));
        assert_eq!(life.generation, u32::MAX);
        assert_eq!(life.state, State::Failed);
    }

    #[test]
    fn event_tag_preserves_full_generation_and_rejects_other_kinds() {
        let generation = u32::MAX - 7;
        assert_eq!(
            decode_tag(exit_tag(2, generation)),
            EventTag::Exit {
                index: 2,
                generation
            }
        );
        assert_eq!(
            decode_tag(pong_tag(1, generation)),
            EventTag::Pong {
                index: 1,
                generation
            }
        );
        assert_eq!(decode_tag(0), EventTag::Other);
    }

    #[test]
    fn health_retirement_exit_consumes_budget_once_but_rollback_does_not() {
        let mut health = Lifecycle::boot();
        assert!(health.spawned(health.next_generation().unwrap(), 1));
        health.retirement_started();
        assert_eq!(
            health.complete_retirement(true, 9, 2),
            Some(ExitDecision::Restart)
        );
        assert_eq!(health.window_restarts, 1);
        assert_eq!(health.complete_retirement(true, 9, 3), None);
        assert_eq!(health.window_restarts, 1);

        let mut rollback = Lifecycle::boot();
        assert!(rollback.spawned(rollback.next_generation().unwrap(), 1));
        rollback.retirement_started();
        assert_eq!(rollback.complete_retirement(false, 9, 2), None);
        assert_eq!(rollback.window_restarts, 0);
        assert_eq!(rollback.state, State::Failed);
    }
}
