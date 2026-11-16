//! Rust-owned ext4 orphan-chain transitions.
//!
//! The C adapter provides bounded inode/superblock reads and journalled
//! metadata writes plus the ext4 truncate/free primitive. This module owns
//! chain validation, link ordering, cycle bounds and the decision sequence.

use core::ffi::c_void;

const EINVAL: i32 = -22;
const EUCLEAN: i32 = -5;
const EINCONSISTENT: i32 = -117;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct InodeLink {
    links: u16,
    next: u32,
}

trait OrphanIo {
    fn inode_count(&self) -> u32;
    fn first_ino(&self) -> u32;
    fn head(&mut self) -> Result<u32, i32>;
    fn set_head(&mut self, ino: u32) -> Result<(), i32>;
    fn read_inode(&mut self, ino: u32) -> Result<InodeLink, i32>;
    fn write_inode(&mut self, ino: u32, state: InodeLink) -> Result<(), i32>;
    fn abort_transaction(&mut self);
    /// Perform the adapter's bounded truncate/free primitive selected by the
    /// Rust-owned link-count decision. The orphan node is removed only after
    /// this durable work succeeds.
    fn truncate_linked_inode(&mut self, ino: u32) -> Result<(), i32>;
    /// Truncate all extents and write the still-allocated zero-link inode.
    /// This runs while the inode remains reachable through the orphan list.
    fn truncate_unlinked_inode(&mut self, ino: u32) -> Result<(), i32>;
    /// Mark the already-empty inode deleted and free its inode bitmap bit.
    /// A reported failure must leave the inode allocated so the caller can
    /// restore its orphan-chain entry before returning.
    fn release_unlinked_inode(&mut self, ino: u32) -> Result<(), i32>;
}

fn valid_ino(io: &impl OrphanIo, ino: u32) -> bool {
    ino >= io.first_ino() && ino <= io.inode_count()
}

fn add_final_links<I: OrphanIo>(io: &mut I, ino: u32, links: u16) -> Result<(), i32> {
    if !valid_ino(io, ino) { return Err(EINVAL); }
    let mut head = io.head()?;
    let mut steps = 0u32;
    while head != 0 {
        if !valid_ino(io, head) { return Err(EUCLEAN); }
        if head == ino { return Err(EUCLEAN); }
        if steps >= io.inode_count() { return Err(EUCLEAN); }
        let entry = io.read_inode(head)?;
        head = entry.next;
        steps += 1;
    }

    let original = io.read_inode(ino)?;
    if original.links != links { return Err(EINVAL); }
    let mut victim = original;
    let old_head = io.head()?;
    victim.links = 0;
    victim.next = old_head;
    if let Err(error) = io.write_inode(ino, victim) {
        let restored = io.write_inode(ino, original);
        io.abort_transaction();
        return Err(if restored.is_err() { EINCONSISTENT } else { error });
    }
    if let Err(error) = io.set_head(ino) {
        // set_head callbacks may report failure after a partial write. Put
        // both links back before allowing another request to mutate the FS.
        let head_restore = io.set_head(old_head);
        let inode_restore = io.write_inode(ino, original);
        io.abort_transaction();
        return Err(if head_restore.is_err() || inode_restore.is_err() {
            EINCONSISTENT
        } else { error });
    }
    Ok(())
}

fn add_last_link<I: OrphanIo>(io: &mut I, ino: u32) -> Result<(), i32> {
    add_final_links(io, ino, 1)
}

fn find_link<I: OrphanIo>(io: &mut I, target: u32) -> Result<(u32, InodeLink), i32> {
    if !valid_ino(io, target) { return Err(EINVAL); }
    let mut prev = 0;
    let mut cur = io.head()?;
    let mut steps = 0u32;
    let mut found = None;
    while cur != 0 {
        if !valid_ino(io, cur) || steps >= io.inode_count() { return Err(EUCLEAN); }
        let node = io.read_inode(cur)?;
        if cur == target {
            if found.is_some() { return Err(EUCLEAN); }
            found = Some((prev, node));
        }
        prev = cur;
        cur = node.next;
        steps += 1;
    }
    found.ok_or(EUCLEAN)
}

fn unlink_node<I: OrphanIo>(io: &mut I, prev: u32, node: InodeLink) -> Result<(), i32> {
    if prev == 0 {
        io.set_head(node.next)
    } else {
        let mut predecessor = io.read_inode(prev)?;
        predecessor.next = node.next;
        io.write_inode(prev, predecessor)
    }
}

fn restore_node<I: OrphanIo>(io: &mut I, prev: u32, ino: u32,
                             node: InodeLink) -> Result<(), i32> {
    // Restore the inode link metadata before publishing a pointer to it.
    io.write_inode(ino, node)?;
    if prev == 0 {
        io.set_head(ino)
    } else {
        let mut predecessor = io.read_inode(prev)?;
        predecessor.next = ino;
        io.write_inode(prev, predecessor)
    }
}

fn reap_one<I: OrphanIo>(io: &mut I, ino: u32) -> Result<(), i32> {
    let (prev, node) = find_link(io, ino)?;
    // ext4's legacy orphan list also contains linked inodes interrupted during
    // truncate. Recovery must truncate them to their recorded i_size and keep
    // the inode; only zero-link entries are freed. If any adapter write fails,
    // the list node stays reachable so journal replay/recovery can retry.
    let finish = if node.links == 0 {
        io.truncate_unlinked_inode(ino)
    } else {
        io.truncate_linked_inode(ino)
    };
    if let Err(error) = finish {
        io.abort_transaction();
        return Err(error);
    }
    if let Err(error) = unlink_node(io, prev, node) {
        let restore = restore_node(io, prev, ino, node);
        io.abort_transaction();
        return Err(if restore.is_err() { EINCONSISTENT } else { error });
    }
    if node.links == 0 {
        if let Err(error) = io.release_unlinked_inode(ino) {
            if error == EINCONSISTENT {
                io.abort_transaction();
                return Err(error);
            }
            // Keep a zero-link inode discoverable after ordinary I/O errors.
            // Journaled filesystems additionally roll the failed transaction
            // back; no-journal crash atomicity remains a separate limitation.
            let restore = restore_node(io, prev, ino, node);
            io.abort_transaction();
            return Err(if restore.is_err() { EINCONSISTENT } else { error });
        }
    }
    Ok(())
}

/* Recovery runs while the filesystem is not yet published to clients, so no
 * concurrent unlink can change the chain after preflight. Reusing the current
 * head's already-validated next pointer avoids invoking reap_one's full-chain
 * search for every node (which otherwise makes a long orphan list O(n²)). */
fn reap_recovery_head<I: OrphanIo>(io: &mut I, ino: u32) -> Result<u32, i32> {
    let node = io.read_inode(ino)?;
    let finish = if node.links == 0 {
        io.truncate_unlinked_inode(ino)
    } else {
        io.truncate_linked_inode(ino)
    };
    if let Err(error) = finish {
        io.abort_transaction();
        return Err(error);
    }
    if let Err(error) = io.set_head(node.next) {
        let restore = restore_node(io, 0, ino, node);
        io.abort_transaction();
        return Err(if restore.is_err() { EINCONSISTENT } else { error });
    }
    if node.links == 0 {
        if let Err(error) = io.release_unlinked_inode(ino) {
            if error == EINCONSISTENT {
                io.abort_transaction();
                return Err(error);
            }
            let restore = restore_node(io, 0, ino, node);
            io.abort_transaction();
            return Err(if restore.is_err() { EINCONSISTENT } else { error });
        }
    }
    Ok(node.next)
}

fn recover<I: OrphanIo>(io: &mut I) -> Result<u32, i32> {
    // Validate the entire chain before changing any entry. Otherwise a corrupt
    // tail could be discovered only after earlier nodes had been reclaimed.
    let mut cur = io.head()?;
    let mut validated = 0u32;
    while cur != 0 {
        if !valid_ino(io, cur) || validated >= io.inode_count() {
            return Err(EUCLEAN);
        }
        let node = io.read_inode(cur)?;
        cur = node.next;
        validated += 1;
    }

    let mut count = 0u32;
    loop {
        let head = io.head()?;
        if head == 0 { return Ok(count); }
        if count >= io.inode_count() || !valid_ino(io, head) { return Err(EUCLEAN); }
        let next = reap_recovery_head(io, head)?;
        /* Verify the adapter's head update instead of trusting a second
         * potentially inconsistent read. */
        if io.head()? != next { return Err(EINCONSISTENT); }
        count += 1;
    }
}

#[repr(C)]
pub struct Ext4OrphanOps {
    pub context: *mut c_void,
    pub inode_count: u32,
    pub first_ino: u32,
    pub read_head: Option<unsafe extern "C" fn(*mut c_void, *mut u32) -> i32>,
    pub write_head: Option<unsafe extern "C" fn(*mut c_void, u32) -> i32>,
    pub read_inode: Option<unsafe extern "C" fn(*mut c_void, u32, *mut u16, *mut u32) -> i32>,
    pub write_inode: Option<unsafe extern "C" fn(*mut c_void, u32, u16, u32) -> i32>,
    pub truncate_linked_inode: Option<unsafe extern "C" fn(*mut c_void, u32) -> i32>,
    pub truncate_unlinked_inode: Option<unsafe extern "C" fn(*mut c_void, u32) -> i32>,
    pub release_unlinked_inode: Option<unsafe extern "C" fn(*mut c_void, u32) -> i32>,
    pub abort_transaction: Option<unsafe extern "C" fn(*mut c_void)>,
}

struct CallbackIo<'a>(&'a Ext4OrphanOps);

impl OrphanIo for CallbackIo<'_> {
    fn inode_count(&self) -> u32 { self.0.inode_count }
    fn first_ino(&self) -> u32 { self.0.first_ino }
    fn head(&mut self) -> Result<u32, i32> {
        let mut out = 0;
        let call = self.0.read_head.ok_or(EINVAL)?;
        let ret = unsafe { call(self.0.context, &mut out) };
        if ret < 0 { Err(ret) } else { Ok(out) }
    }
    fn set_head(&mut self, ino: u32) -> Result<(), i32> {
        let call = self.0.write_head.ok_or(EINVAL)?;
        let ret = unsafe { call(self.0.context, ino) };
        if ret < 0 { Err(ret) } else { Ok(()) }
    }
    fn read_inode(&mut self, ino: u32) -> Result<InodeLink, i32> {
        let mut links = 0;
        let mut next = 0;
        let call = self.0.read_inode.ok_or(EINVAL)?;
        let ret = unsafe { call(self.0.context, ino, &mut links, &mut next) };
        if ret < 0 { Err(ret) } else { Ok(InodeLink { links, next }) }
    }
    fn write_inode(&mut self, ino: u32, state: InodeLink) -> Result<(), i32> {
        let call = self.0.write_inode.ok_or(EINVAL)?;
        let ret = unsafe { call(self.0.context, ino, state.links, state.next) };
        if ret < 0 { Err(ret) } else { Ok(()) }
    }
    fn truncate_linked_inode(&mut self, ino: u32) -> Result<(), i32> {
        let call = self.0.truncate_linked_inode.ok_or(EINVAL)?;
        let ret = unsafe { call(self.0.context, ino) };
        if ret < 0 { Err(ret) } else { Ok(()) }
    }
    fn truncate_unlinked_inode(&mut self, ino: u32) -> Result<(), i32> {
        let call = self.0.truncate_unlinked_inode.ok_or(EINVAL)?;
        let ret = unsafe { call(self.0.context, ino) };
        if ret < 0 { Err(ret) } else { Ok(()) }
    }
    fn release_unlinked_inode(&mut self, ino: u32) -> Result<(), i32> {
        let call = self.0.release_unlinked_inode.ok_or(EINVAL)?;
        let ret = unsafe { call(self.0.context, ino) };
        if ret < 0 { Err(ret) } else { Ok(()) }
    }
    fn abort_transaction(&mut self) {
        if let Some(call) = self.0.abort_transaction {
            unsafe { call(self.0.context) };
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_ext4_orphan_add_last_link(ops: *const Ext4OrphanOps,
                                                         ino: u32) -> i32 {
    let Some(ops) = (unsafe { ops.as_ref() }) else { return EINVAL; };
    add_last_link(&mut CallbackIo(ops), ino).map_or_else(|e| e, |_| 0)
}

/// An empty directory loses its parent entry and its self link together.
#[no_mangle]
pub unsafe extern "C" fn a20_ext4_orphan_add_directory(ops: *const Ext4OrphanOps,
                                                       ino: u32) -> i32 {
    let Some(ops) = (unsafe { ops.as_ref() }) else { return EINVAL; };
    add_final_links(&mut CallbackIo(ops), ino, 2).map_or_else(|e| e, |_| 0)
}

#[no_mangle]
pub unsafe extern "C" fn a20_ext4_orphan_reap(ops: *const Ext4OrphanOps,
                                                ino: u32) -> i32 {
    let Some(ops) = (unsafe { ops.as_ref() }) else { return EINVAL; };
    reap_one(&mut CallbackIo(ops), ino).map_or_else(|e| e, |_| 0)
}

#[no_mangle]
pub unsafe extern "C" fn a20_ext4_orphan_recover(ops: *const Ext4OrphanOps,
                                                   out_count: *mut u32) -> i32 {
    if out_count.is_null() { return EINVAL; }
    let Some(ops) = (unsafe { ops.as_ref() }) else { return EINVAL; };
    match recover(&mut CallbackIo(ops)) {
        Ok(count) => { unsafe { out_count.write(count) }; 0 }
        Err(error) => error,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::BTreeMap;

    #[derive(Default)]
    struct Fs {
        head: u32,
        inodes: BTreeMap<u32, InodeLink>,
        freed: Vec<u32>,
        fail_reap: bool,
        partial_truncate_error: bool,
        fail_release: bool,
        fail_release_fatal: bool,
        fail_head_once: bool,
        fail_head_writes: u8,
        fail_inode_write_once: Option<u32>,
        fail_inode_write_count: Option<(u32, u8)>,
        events: Vec<&'static str>,
        preserved: Vec<u32>,
        aborts: u32,
        reads: u64,
    }
    impl OrphanIo for Fs {
        fn inode_count(&self) -> u32 { 128 }
        fn first_ino(&self) -> u32 { 1 }
        fn head(&mut self) -> Result<u32, i32> { Ok(self.head) }
        fn set_head(&mut self, ino: u32) -> Result<(), i32> {
            self.head = ino; self.events.push("head");
            if self.fail_head_once { self.fail_head_once = false; return Err(-5); }
            if self.fail_head_writes != 0 {
                self.fail_head_writes -= 1;
                return Err(-5);
            }
            Ok(())
        }
        fn read_inode(&mut self, ino: u32) -> Result<InodeLink, i32> {
            self.reads += 1;
            self.inodes.get(&ino).copied().ok_or(EUCLEAN)
        }
        fn write_inode(&mut self, ino: u32, state: InodeLink) -> Result<(), i32> {
            self.inodes.insert(ino, state);
            if self.fail_inode_write_once == Some(ino) {
                self.fail_inode_write_once = None;
                return Err(-5);
            }
            if let Some((target, remaining)) = self.fail_inode_write_count {
                if target == ino && remaining != 0 {
                    self.fail_inode_write_count = Some((target, remaining - 1));
                    return Err(-5);
                }
            }
            Ok(())
        }
        fn abort_transaction(&mut self) { self.aborts += 1; }
        fn truncate_linked_inode(&mut self, ino: u32) -> Result<(), i32> {
            if self.fail_reap { return Err(-5); }
            // Models the C adapter's truncate-to-i_size operation while
            // leaving the live linked inode allocated and readable.
            self.preserved.push(ino);
            self.events.push("truncate-linked");
            Ok(())
        }
        fn truncate_unlinked_inode(&mut self, _ino: u32) -> Result<(), i32> {
            if self.partial_truncate_error {
                self.events.push("truncate-partial-error");
                return Err(-5);
            }
            if self.fail_reap { return Err(-5); }
            self.events.push("truncate-unlinked");
            Ok(())
        }
        fn release_unlinked_inode(&mut self, ino: u32) -> Result<(), i32> {
            self.events.push("release");
            if self.fail_release {
                // Model mark-deleted already changing i_dtime before the
                // subsequent inode-bitmap/group-descriptor write fails.
                if let Some(node) = self.inodes.get_mut(&ino) { node.next = 0xfeed; }
                return Err(-5);
            }
            if self.fail_release_fatal {
                if let Some(node) = self.inodes.get_mut(&ino) { node.next = 0xfeed; }
                return Err(EINCONSISTENT);
            }
            self.inodes.remove(&ino);
            self.freed.push(ino);
            Ok(())
        }
    }

    #[test]
    fn unlink_sets_zero_links_and_prepends_orphan() {
        let mut fs = Fs { head: 12, ..Fs::default() };
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        fs.inodes.insert(13, InodeLink { links: 1, next: 0 });
        assert_eq!(add_last_link(&mut fs, 13), Ok(()));
        assert_eq!(fs.head, 13);
        assert_eq!(fs.inodes[&13], InodeLink { links: 0, next: 12 });
    }

    #[test]
    fn close_reaps_head_and_middle_without_losing_neighbors() {
        let mut fs = Fs { head: 13, ..Fs::default() };
        fs.inodes.insert(13, InodeLink { links: 0, next: 12 });
        fs.inodes.insert(12, InodeLink { links: 0, next: 11 });
        fs.inodes.insert(11, InodeLink { links: 0, next: 0 });
        assert_eq!(reap_one(&mut fs, 12), Ok(()));
        assert_eq!(fs.inodes[&13].next, 11);
        assert_eq!(reap_one(&mut fs, 13), Ok(()));
        assert_eq!(fs.head, 11);
        assert_eq!(fs.freed, [12, 13]);
    }

    #[test]
    fn mount_recovery_drains_chain_in_order() {
        let mut fs = Fs { head: 13, ..Fs::default() };
        fs.inodes.insert(13, InodeLink { links: 0, next: 12 });
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        assert_eq!(recover(&mut fs), Ok(2));
        assert_eq!(fs.head, 0);
        assert_eq!(fs.freed, [13, 12]);
    }

    #[test]
    fn malformed_chains_fail_closed_before_reclaiming_prefix() {
        let mut invalid = Fs { head: 129, ..Fs::default() };
        assert_eq!(recover(&mut invalid), Err(EUCLEAN));
        let mut cycle = Fs { head: 12, ..Fs::default() };
        cycle.inodes.insert(12, InodeLink { links: 0, next: 12 });
        assert_eq!(recover(&mut cycle), Err(EUCLEAN));
        let mut corrupt_tail = Fs { head: 12, ..Fs::default() };
        corrupt_tail.inodes.insert(12, InodeLink { links: 0, next: 129 });
        assert_eq!(recover(&mut corrupt_tail), Err(EUCLEAN));
        assert!(corrupt_tail.freed.is_empty());
    }

    #[test]
    fn linked_orphan_is_truncated_but_not_freed() {
        let mut fs = Fs { head: 12, ..Fs::default() };
        fs.inodes.insert(12, InodeLink { links: 2, next: 0 });
        assert_eq!(recover(&mut fs), Ok(1));
        assert_eq!(fs.head, 0);
        assert_eq!(fs.preserved, [12]);
        assert!(fs.inodes.contains_key(&12));
        assert!(fs.freed.is_empty());
    }

    #[test]
    fn failed_reclaim_keeps_orphan_head_retryable() {
        let mut fs = Fs { head: 12, fail_reap: true, ..Fs::default() };
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        assert_eq!(recover(&mut fs), Err(-5));
        assert_eq!(fs.head, 12);
        assert_eq!(fs.aborts, 1);
    }

    #[test]
    fn truncate_error_keeps_node_linked_for_retry() {
        let mut fs = Fs { head: 12, partial_truncate_error: true, ..Fs::default() };
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        assert_eq!(reap_one(&mut fs, 12), Err(-5));
        assert_eq!(fs.head, 12);
        assert_eq!(fs.inodes.get(&12), Some(&InodeLink { links: 0, next: 0 }));
        assert!(fs.freed.is_empty());
        fs.partial_truncate_error = false;
        assert_eq!(reap_one(&mut fs, 12), Ok(()));
        assert_eq!(fs.head, 0);
        assert!(fs.freed.contains(&12));
    }

    #[test]
    fn chain_update_failure_happens_before_inode_release() {
        let mut fs = Fs { head: 12, fail_head_once: true, ..Fs::default() };
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        assert_eq!(reap_one(&mut fs, 12), Err(-5));
        assert_eq!(fs.head, 12);
        assert!(fs.inodes.contains_key(&12));
        assert!(fs.freed.is_empty());
        assert_eq!(fs.events, ["truncate-unlinked", "head", "head"]);
    }

    #[test]
    fn failed_head_restore_is_reported_fatal_and_keeps_inode_allocated() {
        let mut fs = Fs { head: 12, fail_head_writes: 2, ..Fs::default() };
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        assert_eq!(reap_one(&mut fs, 12), Err(EINCONSISTENT));
        assert_eq!(fs.head, 12);
        assert_eq!(fs.inodes.get(&12), Some(&InodeLink { links: 0, next: 0 }));
        assert!(fs.freed.is_empty());
    }

    #[test]
    fn failed_predecessor_restore_is_fatal_instead_of_retryable_io_error() {
        let mut fs = Fs { head: 7, fail_inode_write_count: Some((7, 2)), ..Fs::default() };
        fs.inodes.insert(7, InodeLink { links: 1, next: 9 });
        fs.inodes.insert(9, InodeLink { links: 0, next: 0 });
        assert_eq!(reap_one(&mut fs, 9), Err(EINCONSISTENT));
        assert_eq!(fs.inodes.get(&9), Some(&InodeLink { links: 0, next: 0 }));
        // The failed callback may have persisted its write before reporting
        // EIO. The restore callback then makes the predecessor point back at
        // the still-allocated orphan; because the callback also reports an
        // error, the caller must quarantine this generation rather than
        // assuming either the old or restored value is durable.
        assert_eq!(fs.inodes.get(&7), Some(&InodeLink { links: 1, next: 9 }));
        assert!(fs.freed.is_empty());
    }

    #[test]
    fn predecessor_write_failure_is_compensated_before_release() {
        let mut fs = Fs { head: 13, fail_inode_write_once: Some(13), ..Fs::default() };
        fs.inodes.insert(13, InodeLink { links: 1, next: 12 });
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        assert_eq!(reap_one(&mut fs, 12), Err(-5));
        assert_eq!(fs.head, 13);
        assert_eq!(fs.inodes.get(&13), Some(&InodeLink { links: 1, next: 12 }));
        assert_eq!(fs.inodes.get(&12), Some(&InodeLink { links: 0, next: 0 }));
        assert!(fs.freed.is_empty());
        assert!(!fs.events.contains(&"release"));
    }

    #[test]
    fn release_failure_restores_zero_link_entry_for_retry() {
        let mut fs = Fs { head: 12, fail_release: true, ..Fs::default() };
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        assert_eq!(reap_one(&mut fs, 12), Err(-5));
        assert_eq!(fs.head, 12);
        assert_eq!(fs.inodes.get(&12), Some(&InodeLink { links: 0, next: 0 }));
        assert!(fs.freed.is_empty());
        assert_eq!(fs.events, ["truncate-unlinked", "head", "release", "head"]);
    }

    #[test]
    fn uncertain_inode_free_is_not_relinked() {
        let mut fs = Fs { head: 12, fail_release_fatal: true, ..Fs::default() };
        fs.inodes.insert(12, InodeLink { links: 0, next: 0 });
        assert_eq!(reap_one(&mut fs, 12), Err(EINCONSISTENT));
        assert_eq!(fs.head, 0);
        assert_eq!(fs.inodes.get(&12), Some(&InodeLink { links: 0, next: 0xfeed }));
        assert!(!fs.freed.contains(&12));
    }

    #[test]
    fn long_recovery_reads_each_inode_a_bounded_number_of_times() {
        let n = 96u32;
        let mut fs = Fs { head: 1, ..Fs::default() };
        for ino in 1..=n {
            fs.inodes.insert(ino, InodeLink { links: 0, next: if ino == n { 0 } else { ino + 1 } });
        }
        assert_eq!(recover(&mut fs), Ok(n));
        assert_eq!(fs.head, 0);
        assert_eq!(fs.freed.len(), n as usize);
        assert!(fs.reads <= (n as u64) * 2 + 1,
                "recovery performed {} inode reads for {} nodes", fs.reads, n);
    }
}
