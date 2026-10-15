//! Bounded Rust-owned open/unlink lifetime state for the VNFS inode registry.
//! The C adapter owns vnode references; this module decides when the adapter
//! may release the registry reference after unlink and last close.

const MAX_OPEN_INODES: usize = 512;
const MAX_LIVE_TOKENS: usize = 512;
const RECENT_CLOSED_TOKENS: usize = 64;
const ERR_NOENT: i64 = -2;
const ERR_AGAIN: i64 = -11;
const ERR_INVALID: i64 = -22;
const ERR_NOSPC: i64 = -28;
const ERR_OVERFLOW: i64 = -24;
const ERR_INCONSISTENT: i64 = -117;

#[derive(Clone, Copy)]
struct Slot {
    ino: u64,
    opens: u32,
    unlinked: bool,
    occupied: bool,
}

impl Slot {
    const EMPTY: Self = Self { ino: 0, opens: 0, unlinked: false, occupied: false };
}

#[derive(Clone, Copy)]
struct LiveToken {
    token: u64, // 0 = empty slot
    ino: u64,
}

impl LiveToken {
    const EMPTY: Self = Self { token: 0, ino: 0 };
}

#[derive(Clone, Copy)]
struct RecentToken {
    token: u64, // 0 = empty slot
    ino: u64,
}

impl RecentToken {
    const EMPTY: Self = Self { token: 0, ino: 0 };
}

/// Verdict for a CLOSE carrying a token
/// (docs/rust/ufs-close-token-design-r1.md).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum TokenLookup {
    /// Garbage close: the dispatcher answers ENOENT.
    Unknown,
    /// Matches a live open instance: consume it exactly as a legacy close.
    Live,
    /// Already accepted: success with no side effects.
    Replay,
}

/// Result of the shared production CLOSE decision. A replay is acknowledged
/// without invoking the backend or touching open counts a second time.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CloseOutcome {
    Consumed { release_orphan: bool },
    Replayed,
}

fn mix_token(epoch: u64, serial: u64) -> u64 {
    let mut x = epoch ^ serial.wrapping_mul(0x9e37_79b9_7f4a_7c15);
    x ^= x >> 30;
    x = x.wrapping_mul(0xbf58_476d_1ce4_e5b9);
    x ^= x >> 27;
    x = x.wrapping_mul(0x94d0_49bb_1331_11eb);
    x ^= x >> 31;
    x // bijective in serial for a fixed epoch; zero is skipped by issue
}

pub struct Lifetimes {
    slots: [Slot; MAX_OPEN_INODES],
    quarantined: bool,
    tokens_enabled: bool,
    token_epoch: u64,
    token_serial: u64,
    live_tokens: [LiveToken; MAX_LIVE_TOKENS],
    recent_tokens: [RecentToken; RECENT_CLOSED_TOKENS],
}

impl Lifetimes {
    pub const fn new() -> Self {
        Self {
            slots: [Slot::EMPTY; MAX_OPEN_INODES],
            quarantined: false,
            tokens_enabled: false,
            token_epoch: 0,
            token_serial: 0,
            live_tokens: [LiveToken::EMPTY; MAX_LIVE_TOKENS],
            recent_tokens: [RecentToken::EMPTY; RECENT_CLOSED_TOKENS],
        }
    }

    /// Stop requests after allocation metadata could not be restored to a
    /// known state. This is sticky for the current service generation;
    /// restart-time recovery remains a separate on-disk contract.
    pub fn quarantine(&mut self) { self.quarantined = true; }

    pub fn is_quarantined(&self) -> bool { self.quarantined }

    /// Per-boot nonce mixed into issued tokens; collisions across service
    /// restarts are irrelevant (a restart tears down the mount channel), so
    /// the monotonic clock is sufficient.
    pub fn seed_token_epoch(&mut self, epoch: u64) { self.token_epoch = epoch; }

    /// The INIT handshake enables token issue for this mount channel only
    /// when the client advertised UFS_CLOSE_TOKEN_CAPABILITY.
    pub fn enable_close_tokens(&mut self) { self.tokens_enabled = true; }

    /// Match token issuance to the current INIT handshake, including a later
    /// legacy INIT on the same service generation.
    pub fn set_close_tokens_enabled(&mut self, enabled: bool) {
        self.tokens_enabled = enabled;
    }

    pub fn close_tokens_enabled(&self) -> bool { self.tokens_enabled }

    /// Issue a nonzero token for one accepted OPEN. Returns 0 when tokens
    /// are not negotiated or the live table is full; that open then keeps
    /// the legacy ambiguous-close behavior.
    pub fn issue_token(&mut self, ino: u64) -> u64 {
        if !self.tokens_enabled || self.token_epoch == 0 { return 0; }
        let Some(slot) = self.live_tokens.iter_mut().find(|slot| slot.token == 0)
        else { return 0 };
        let token = loop {
            let Some(serial) = self.token_serial.checked_add(1) else { return 0 };
            self.token_serial = serial;
            let candidate = mix_token(self.token_epoch, serial);
            if candidate != 0 { break candidate; }
        };
        slot.token = token;
        slot.ino = ino;
        slot.token
    }

    /// Production OPEN decision shared with the host dispatcher harness.
    /// The backend callback runs after the Rust open count is reserved; a
    /// failed callback rolls that count back and issues no token.
    pub fn open_with_backend<F>(&mut self, ino: u64, backend_open: F) -> Result<u64, i64>
    where F: FnOnce() -> i64 {
        let state_rc = self.opened(ino);
        if state_rc != 0 { return Err(state_rc); }
        let token = if self.tokens_enabled { self.issue_token(ino) } else { 0 };
        if self.tokens_enabled && token == 0 {
            let _ = self.closed(ino);
            return Err(ERR_AGAIN);
        }
        let backend_rc = backend_open();
        if backend_rc != 0 {
            let _ = self.closed(ino);
            self.forget_live_token(token);
            return Err(backend_rc);
        }
        Ok(token)
    }

    /// Production CLOSE decision shared with the host dispatcher harness.
    /// The token lookup and exactly-once backend/state transition live here;
    /// orphan cleanup remains the caller's responsibility after a successful
    /// final close.
    pub fn close_with_backend<F>(&mut self, ino: u64, token: u64,
                                 backend_close: F) -> Result<CloseOutcome, i64>
    where F: FnOnce() -> i64 {
        if self.tokens_enabled && token == 0 { return Err(ERR_NOENT); }
        if token != 0 {
            match self.token_lookup(ino, token) {
                TokenLookup::Replay => {
                    return Ok(CloseOutcome::Replayed);
                }
                TokenLookup::Unknown => return Err(ERR_NOENT),
                TokenLookup::Live => {}
            }
            if !self.recent_tokens.iter().any(|slot| slot.token == 0) {
                // Do not consume the backend open or evict an accepted
                // outcome while a client may still need its replay ACK.
                return Err(ERR_AGAIN);
            }
        }
        if !self.is_open(ino) { return Err(ERR_NOENT); }
        let backend_rc = backend_close();
        if backend_rc != 0 { return Err(backend_rc); }
        match self.closed(ino) {
            Err(error) => Err(error),
            Ok(release_orphan) => {
                if token != 0 { self.token_accepted(ino, token); }
                Ok(CloseOutcome::Consumed { release_orphan })
            }
        }
    }

    pub fn token_lookup(&self, ino: u64, token: u64) -> TokenLookup {
        if token == 0 { return TokenLookup::Unknown; }
        for slot in self.live_tokens.iter() {
            if slot.token == token {
                return if slot.ino == ino { TokenLookup::Live } else { TokenLookup::Unknown };
            }
        }
        if let Some(slot) = self.recent_tokens.iter().find(|slot| slot.token == token) {
            return if slot.ino == ino { TokenLookup::Replay }
                   else { TokenLookup::Unknown };
        }
        TokenLookup::Unknown
    }

    /// Move a live token to the confirm-retained table after its close was
    /// accepted. Capacity is checked before the backend callback; accepted
    /// outcomes cannot be evicted until the client confirms receipt.
    pub fn token_accepted(&mut self, ino: u64, token: u64) {
        for slot in self.live_tokens.iter_mut() {
            if slot.token == token {
                *slot = LiveToken::EMPTY;
                break;
            }
        }
        if let Some(slot) = self.recent_tokens.iter_mut().find(|slot| slot.token == 0) {
            *slot = RecentToken { token, ino };
        }
    }

    /// Retire an accepted CLOSE outcome after the client received its ACK.
    /// Unknown confirmations are idempotent success (the prior CONFIRM ACK
    /// may have been lost); live or wrong-inode tokens cannot be retired.
    pub fn confirm_close(&mut self, ino: u64, token: u64) -> Result<(), i64> {
        if token == 0 { return Ok(()); }
        if let Some(slot) = self.live_tokens.iter().find(|slot| slot.token == token) {
            return Err(if slot.ino == ino { ERR_AGAIN } else { ERR_INVALID });
        }
        if let Some(slot) = self.recent_tokens.iter_mut().find(|slot| slot.token == token) {
            if slot.ino != ino { return Err(ERR_INVALID); }
            *slot = RecentToken::EMPTY;
        }
        Ok(())
    }

    /// Drop live tokens of an inode whose open count reached zero, so a
    /// leaked entry cannot outlive the open instance it named.
    fn purge_live_tokens(&mut self, ino: u64) {
        for slot in self.live_tokens.iter_mut() {
            if slot.token != 0 && slot.ino == ino { *slot = LiveToken::EMPTY; }
        }
    }

    fn forget_live_token(&mut self, token: u64) {
        if token == 0 { return; }
        if let Some(slot) = self.live_tokens.iter_mut().find(|slot| slot.token == token) {
            *slot = LiveToken::EMPTY;
        }
    }

    /// Apply the dispatcher policy to a status returned by a backend
    /// operation. Production dispatch uses this after every opcode result.
    pub fn observe_result(&mut self, result: i64, inconsistent: i64) -> i64 {
        if result == inconsistent { self.quarantine(); }
        result
    }

    pub fn request_allowed(&self, opcode: u32, init_opcode: u32) -> bool {
        !self.quarantined || opcode == init_opcode
    }

    fn find(&self, ino: u64) -> Option<usize> {
        self.slots.iter().position(|slot| slot.occupied && slot.ino == ino)
    }

    pub fn is_open(&self, ino: u64) -> bool {
        self.find(ino).is_some_and(|index| self.slots[index].opens != 0)
    }

    /// A graceful service shutdown may release backend vnode ownership only
    /// after every negotiated OPEN has a matching CLOSE.
    pub fn all_closed(&self) -> bool {
        self.slots.iter().all(|slot| !slot.occupied || slot.opens == 0)
    }

    #[cfg(ufsd_test_close_orphan)]
    pub fn open_count(&self, ino: u64) -> u32 {
        self.find(ino).map_or(0, |index| self.slots[index].opens)
    }

    pub fn opened(&mut self, ino: u64) -> i64 {
        if let Some(index) = self.find(ino) {
            if self.slots[index].unlinked { return ERR_NOENT; }
            let Some(next) = self.slots[index].opens.checked_add(1) else { return ERR_OVERFLOW };
            self.slots[index].opens = next;
            return 0;
        }
        let Some(slot) = self.slots.iter_mut().find(|slot| !slot.occupied) else { return ERR_NOSPC };
        *slot = Slot { ino, opens: 1, unlinked: false, occupied: true };
        0
    }

    /// Mark the name removed. Return true when the adapter may drop its inode
    /// registry vnode now; otherwise keep it until the last open file closes.
    pub fn unlinked(&mut self, ino: u64) -> bool {
        let Some(index) = self.find(ino) else { return true };
        self.slots[index].unlinked = true;
        false
    }

    /// Return whether the final close released an unlinked inode from the
    /// adapter registry. A close without a corresponding OPEN is invalid.
    pub fn closed(&mut self, ino: u64) -> Result<bool, i64> {
        let Some(index) = self.find(ino) else { return Err(ERR_NOENT) };
        if self.slots[index].opens == 0 { return Err(ERR_NOENT); }
        self.slots[index].opens -= 1;
        if self.slots[index].opens != 0 { return Ok(false); }
        let release = self.slots[index].unlinked;
        if !release { self.slots[index] = Slot::EMPTY; }
        self.purge_live_tokens(ino);
        Ok(release)
    }

    /// Drop the pending slot only after the adapter confirms the on-disk
    /// orphan transaction completed. A failed close cleanup remains retryable
    /// through SYNC and durable through mount-time orphan recovery.
    pub fn reaped(&mut self, ino: u64) -> Result<(), i64> {
        let Some(index) = self.find(ino) else { return Err(ERR_NOENT) };
        if !self.slots[index].unlinked || self.slots[index].opens != 0 {
            return Err(ERR_NOENT);
        }
        self.slots[index] = Slot::EMPTY;
        self.purge_live_tokens(ino);
        Ok(())
    }

    pub fn pending(&self, cursor: &mut usize) -> Option<u64> {
        while *cursor < self.slots.len() {
            let slot = self.slots[*cursor];
            *cursor += 1;
            if slot.occupied && slot.unlinked && slot.opens == 0 { return Some(slot.ino); }
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unlinked_inode_remains_live_until_last_close() {
        let mut state = Lifetimes::new();
        assert_eq!(state.opened(7), 0);
        assert_eq!(state.opened(7), 0);
        assert!(!state.unlinked(7));
        assert_eq!(state.closed(7), Ok(false));
        assert_eq!(state.closed(7), Ok(true));
        assert_eq!(state.closed(7), Err(ERR_NOENT));
    }

    #[test]
    fn ordinary_unlink_releases_map_and_reused_ino_starts_clean() {
        let mut state = Lifetimes::new();
        assert!(state.unlinked(3));
        assert_eq!(state.opened(3), 0);
        assert_eq!(state.closed(3), Ok(false));
        assert!(state.unlinked(3));
    }

    #[test]
    fn capacity_and_open_count_are_bounded() {
        let mut state = Lifetimes::new();
        for ino in 1..=MAX_OPEN_INODES as u64 { assert_eq!(state.opened(ino), 0); }
        assert_eq!(state.opened(MAX_OPEN_INODES as u64 + 1), ERR_NOSPC);
    }

    #[test]
    fn failed_final_cleanup_stays_pending_until_confirmed_reaped() {
        let mut state = Lifetimes::new();
        assert_eq!(state.opened(19), 0);
        assert!(!state.unlinked(19));
        assert_eq!(state.closed(19), Ok(true));
        let mut cursor = 0;
        assert_eq!(state.pending(&mut cursor), Some(19));
        assert_eq!(state.opened(19), ERR_NOENT);
        assert_eq!(state.reaped(19), Ok(()));
        let mut cursor = 0;
        assert_eq!(state.pending(&mut cursor), None);
    }

    #[test]
    fn close_token_duplicate_close_is_idempotent_replay_without_double_consume() {
        let mut state = Lifetimes::new();
        state.seed_token_epoch(0x1234);
        state.enable_close_tokens();
        assert_eq!(state.opened(7), 0);
        let token = state.issue_token(7);
        assert_ne!(token, 0);
        assert_eq!(state.token_lookup(7, token), TokenLookup::Live);
        // Wrong ino for a live token is garbage, not a match.
        assert_eq!(state.token_lookup(9, token), TokenLookup::Unknown);
        // First close: consume the open exactly once, then record acceptance.
        assert_eq!(state.closed(7), Ok(false));
        state.token_accepted(7, token);
        assert_eq!(state.token_lookup(7, token), TokenLookup::Replay);
        assert_eq!(state.token_lookup(9, token), TokenLookup::Unknown);
        assert_eq!(state.token_lookup(0, token), TokenLookup::Unknown);
        // Replay: the dispatcher answers success from the retained outcome
        // and never calls closed() again; confirmation then retires it.
        assert_eq!(state.closed(7), Err(ERR_NOENT));
        assert_eq!(state.token_lookup(7, token), TokenLookup::Replay);
        assert_eq!(state.confirm_close(7, token), Ok(()));
        assert_eq!(state.token_lookup(7, token), TokenLookup::Unknown);
        // A reopened inode gets a fresh token; the old one stays a replay.
        assert_eq!(state.opened(7), 0);
        let next = state.issue_token(7);
        assert_ne!(next, 0);
        assert_ne!(next, token);
        assert_eq!(state.token_lookup(7, token), TokenLookup::Unknown);
    }

    #[test]
    fn production_dispatch_helpers_open_then_ack_lost_close_by_same_token() {
        let mut state = Lifetimes::new();
        state.seed_token_epoch(0x8a55);
        state.set_close_tokens_enabled(true);
        let mut opens = 0;
        let token = state.open_with_backend(41, || { opens += 1; 0 }).unwrap();
        assert_eq!(opens, 1);
        assert_ne!(token, 0); // value returned for OPEN response out2

        // The first CLOSE was consumed and accepted, but imagine its reply
        // was lost. Re-dispatching the exact token must ACK without invoking
        // the backend a second time or decrementing state again.
        let mut closes = 0;
        assert_eq!(state.close_with_backend(41, token, || { closes += 1; 0 }),
                   Ok(CloseOutcome::Consumed { release_orphan: false }));
        assert_eq!(state.close_with_backend(41, token, || { closes += 1; 0 }),
                   Ok(CloseOutcome::Replayed));
        assert_eq!(closes, 1);
        assert!(!state.is_open(41));
        assert_eq!(state.confirm_close(41, token), Ok(()));
        // A lost CONFIRM ACK is safe: unknown confirmation is success.
        assert_eq!(state.confirm_close(41, token), Ok(()));
        assert_eq!(state.token_lookup(41, token), TokenLookup::Unknown);

        // A wrong-inode replay or unknown token fails before backend effects.
        assert_eq!(state.close_with_backend(42, token, || { closes += 1; 0 }),
                   Err(ERR_NOENT));
        assert_eq!(state.close_with_backend(41, 0xdead_beef,
                   || { closes += 1; 0 }), Err(ERR_NOENT));
        assert_eq!(closes, 1);
        let live = state.open_with_backend(43, || 0).unwrap();
        assert_eq!(state.confirm_close(43, live), Err(ERR_AGAIN));
        assert_eq!(state.confirm_close(44, live), Err(ERR_INVALID));
    }

    #[test]
    fn production_dispatch_helpers_preserve_legacy_and_backend_failures() {
        let mut state = Lifetimes::new();
        state.seed_token_epoch(0x88);
        state.set_close_tokens_enabled(false);
        assert_eq!(state.open_with_backend(6, || -5), Err(-5));
        assert!(!state.is_open(6)); // failed OPEN rolled back
        assert_eq!(state.open_with_backend(6, || 0), Ok(0)); // legacy response
        assert_eq!(state.close_with_backend(6, 0, || -5), Err(-5));
        assert!(state.is_open(6)); // failed CLOSE remains retryable
        assert_eq!(state.close_with_backend(6, 0, || 0),
                   Ok(CloseOutcome::Consumed { release_orphan: false }));
    }

    #[test]
    fn close_token_unknown_token_and_legacy_close_are_unchanged() {
        let mut state = Lifetimes::new();
        state.seed_token_epoch(0x1234);
        state.enable_close_tokens();
        assert_eq!(state.opened(5), 0);
        let token = state.issue_token(5);
        assert_ne!(token, 0);
        // Unknown token: garbage close stays an error and consumes nothing.
        assert_eq!(state.token_lookup(5, 0xdead_beef), TokenLookup::Unknown);
        assert_eq!(state.token_lookup(5, 0), TokenLookup::Unknown);
        assert!(state.is_open(5));
        // Legacy no-token close path is bit-exact with the pre-token behavior.
        assert_eq!(state.closed(5), Ok(false));
        assert!(!state.is_open(5));
        assert_eq!(state.closed(5), Err(ERR_NOENT));
        // The final legacy close purged the orphaned live token entry.
        assert_eq!(state.token_lookup(5, token), TokenLookup::Unknown);
    }

    #[test]
    fn close_token_issue_requires_negotiation_and_live_table_has_room() {
        let mut state = Lifetimes::new();
        state.seed_token_epoch(0x77);
        assert_eq!(state.opened(1), 0);
        assert_eq!(state.issue_token(1), 0); // not negotiated: legacy open
        state.enable_close_tokens();
        let mut epochless = Lifetimes::new();
        epochless.enable_close_tokens();
        assert_eq!(epochless.issue_token(1), 0); // no epoch: no tokens
        epochless.seed_token_epoch(0);
        assert_eq!(epochless.issue_token(1), 0); // unavailable clock stays fail-closed
        state.token_serial = u64::MAX;
        assert_eq!(state.issue_token(1), 0); // serial exhaustion never wraps
        state.token_serial = 0;
        for ino in 1..=MAX_LIVE_TOKENS as u64 {
            assert_eq!(state.opened(ino), 0);
            assert_ne!(state.issue_token(ino), 0);
        }
        // A second open of a live inode needs no new slot; the full token
        // table degrades it to token 0 (legacy), never an error.
        assert_eq!(state.opened(1), 0);
        assert_eq!(state.issue_token(1), 0);
    }

    #[test]
    fn token_mode_open_fails_before_backend_when_token_capacity_is_full() {
        let mut state = Lifetimes::new();
        state.seed_token_epoch(0xface);
        state.set_close_tokens_enabled(true);
        let mut backend_calls = 0;
        for _ in 0..MAX_LIVE_TOKENS {
            assert_ne!(state.open_with_backend(77, || { backend_calls += 1; 0 }).unwrap(), 0);
        }
        assert_eq!(backend_calls, MAX_LIVE_TOKENS);
        assert_eq!(state.open_with_backend(77, || { backend_calls += 1; 0 }), Err(ERR_AGAIN));
        assert_eq!(backend_calls, MAX_LIVE_TOKENS);
        assert!(state.is_open(77));
    }

    #[test]
    fn close_token_recent_table_never_evicts_unconfirmed_results() {
        let mut state = Lifetimes::new();
        state.seed_token_epoch(0x55);
        state.enable_close_tokens();
        let mut first = 0;
        let mut last = 0;
        for ino in 1..=RECENT_CLOSED_TOKENS as u64 {
            assert_eq!(state.opened(ino), 0);
            let token = state.issue_token(ino);
            assert_ne!(token, 0);
            if first == 0 { first = token; }
            last = token;
            assert_eq!(state.closed(ino), Ok(false));
            state.token_accepted(ino, token);
        }
        assert_eq!(state.token_lookup(1, first), TokenLookup::Replay);
        assert_eq!(state.token_lookup(RECENT_CLOSED_TOKENS as u64, last), TokenLookup::Replay);
        // A new close is rejected before the backend consumes its open when
        // all tombstones are awaiting confirmation.
        assert_eq!(state.opened(1000), 0);
        let extra = state.issue_token(1000);
        assert_ne!(extra, 0);
        let mut called = false;
        assert_eq!(state.close_with_backend(1000, extra, || { called = true; 0 }),
                   Err(ERR_AGAIN));
        assert!(!called && state.is_open(1000));
        assert_eq!(state.confirm_close(1, first), Ok(()));
        assert_eq!(state.close_with_backend(1000, extra, || { called = true; 0 }),
                   Ok(CloseOutcome::Consumed { release_orphan: false }));
        assert!(called);
    }

    #[test]
    fn inconsistent_release_quarantine_is_sticky_and_does_not_clear_pending() {
        let mut state = Lifetimes::new();
        assert_eq!(state.opened(31), 0);
        assert!(!state.unlinked(31));
        assert_eq!(state.closed(31), Ok(true));
        // Model a mutating C adapter callback whose compensating write also
        // failed; the same result path is used by dispatch_request.
        assert_eq!(state.observe_result(-117, ERR_INCONSISTENT), -117);
        assert!(state.is_quarantined());
        assert!(state.request_allowed(1, 1));
        assert!(!state.request_allowed(2, 1));
        assert_eq!(ERR_INCONSISTENT, -117);
        let mut cursor = 0;
        assert_eq!(state.pending(&mut cursor), Some(31));
        // The service must not acknowledge a reap after fatal metadata
        // uncertainty; only a confirmed adapter success can clear the slot.
        assert_eq!(state.pending(&mut cursor), None);
    }
}
