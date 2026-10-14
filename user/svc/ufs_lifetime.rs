//! Bounded Rust-owned open/unlink lifetime state for the VNFS inode registry.
//! The C adapter owns vnode references; this module decides when the adapter
//! may release the registry reference after unlink and last close.

const MAX_OPEN_INODES: usize = 512;
const ERR_NOENT: i64 = -2;
const ERR_NOSPC: i64 = -28;
const ERR_OVERFLOW: i64 = -24;

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

pub struct Lifetimes {
    slots: [Slot; MAX_OPEN_INODES],
}

impl Lifetimes {
    pub const fn new() -> Self { Self { slots: [Slot::EMPTY; MAX_OPEN_INODES] } }

    fn find(&self, ino: u64) -> Option<usize> {
        self.slots.iter().position(|slot| slot.occupied && slot.ino == ino)
    }

    pub fn is_open(&self, ino: u64) -> bool {
        self.find(ino).is_some_and(|index| self.slots[index].opens != 0)
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
}
