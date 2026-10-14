//! Rust-owned ext4 orphan-chain transitions.
//!
//! The C adapter provides bounded inode/superblock reads and journalled
//! metadata writes plus the ext4 truncate/free primitive. This module owns
//! chain validation, link ordering, cycle bounds and the decision sequence.

use core::ffi::c_void;

const EINVAL: i32 = -22;
const EUCLEAN: i32 = -5;

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
    /// Complete recovery for a listed inode. The adapter truncates blocks past
    /// i_size when links remain, and frees the inode when links are zero.
    /// The orphan-list node is removed only after this durable work succeeds.
    fn finish_inode(&mut self, ino: u32, links: u16) -> Result<(), i32>;
}

fn valid_ino(io: &impl OrphanIo, ino: u32) -> bool {
    ino >= io.first_ino() && ino <= io.inode_count()
}

fn add_last_link<I: OrphanIo>(io: &mut I, ino: u32) -> Result<(), i32> {
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

    let mut victim = io.read_inode(ino)?;
    if victim.links != 1 { return Err(EINVAL); }
    let old_head = io.head()?;
    victim.links = 0;
    victim.next = old_head;
    if let Err(error) = io.write_inode(ino, victim) {
        io.abort_transaction();
        return Err(error);
    }
    if let Err(error) = io.set_head(ino) {
        io.abort_transaction();
        return Err(error);
    }
    Ok(())
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

fn reap_one<I: OrphanIo>(io: &mut I, ino: u32) -> Result<(), i32> {
    let (prev, node) = find_link(io, ino)?;
    // ext4's legacy orphan list also contains linked inodes interrupted during
    // truncate. Recovery must truncate them to their recorded i_size and keep
    // the inode; only zero-link entries are freed. If any adapter write fails,
    // the list node stays reachable so journal replay/recovery can retry.
    if let Err(error) = io.finish_inode(ino, node.links) {
        io.abort_transaction();
        return Err(error);
    }
    if prev == 0 {
        if let Err(error) = io.set_head(node.next) {
            io.abort_transaction();
            return Err(error);
        }
    } else {
        let mut predecessor = match io.read_inode(prev) {
            Ok(value) => value,
            Err(error) => { io.abort_transaction(); return Err(error); }
        };
        predecessor.next = node.next;
        if let Err(error) = io.write_inode(prev, predecessor) {
            io.abort_transaction();
            return Err(error);
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
    if let Err(error) = io.finish_inode(ino, node.links) {
        io.abort_transaction();
        return Err(error);
    }
    if let Err(error) = io.set_head(node.next) {
        io.abort_transaction();
        return Err(error);
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
        if io.head()? != next { return Err(EUCLEAN); }
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
    pub finish_inode: Option<unsafe extern "C" fn(*mut c_void, u32, u16) -> i32>,
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
    fn finish_inode(&mut self, ino: u32, links: u16) -> Result<(), i32> {
        let call = self.0.finish_inode.ok_or(EINVAL)?;
        let ret = unsafe { call(self.0.context, ino, links) };
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
        preserved: Vec<u32>,
        aborts: u32,
        reads: u64,
    }
    impl OrphanIo for Fs {
        fn inode_count(&self) -> u32 { 128 }
        fn first_ino(&self) -> u32 { 1 }
        fn head(&mut self) -> Result<u32, i32> { Ok(self.head) }
        fn set_head(&mut self, ino: u32) -> Result<(), i32> { self.head = ino; Ok(()) }
        fn read_inode(&mut self, ino: u32) -> Result<InodeLink, i32> {
            self.reads += 1;
            self.inodes.get(&ino).copied().ok_or(EUCLEAN)
        }
        fn write_inode(&mut self, ino: u32, state: InodeLink) -> Result<(), i32> {
            self.inodes.insert(ino, state); Ok(())
        }
        fn abort_transaction(&mut self) { self.aborts += 1; }
        fn finish_inode(&mut self, ino: u32, links: u16) -> Result<(), i32> {
            if self.fail_reap { return Err(-5); }
            if links == 0 {
                self.inodes.remove(&ino);
                self.freed.push(ino);
            } else {
                // Models the C adapter's truncate-to-i_size operation while
                // leaving the live linked inode allocated and readable.
                self.preserved.push(ino);
            }
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
