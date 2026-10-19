//! VFS dentry cache state engine (Rust-owned split for `vfs/dcache.c`).
//!
//! The dcache is a fixed table of int-indexed slots plus a per-hash-bucket
//! lock array, a free list and a doubly linked LRU, all currently owned by
//! `dcache.c`.  This module owns the table's *state transitions*: the name
//! hash, hash-chain link/unlink, LRU remove/insert/touch, the free-list
//! push/take, the exact-match search with its second-chance `accessed` bit,
//! the eviction policy and the bucket-drop used by invalidate_all.
//!
//! The C adapter keeps the global and per-bucket spinlocks, the vnode
//! references (`vnode_get`/`vnode_get_unless_zero`/`vnode_put`), the retire
//! batch storage and the `memset`/`strncpy`, and calls into this module with
//! the locks the C contract requires already held.  No entry point here takes
//! a lock, allocates, or sleeps.

#![cfg_attr(not(test), no_std)]

use core::ffi::c_int;

/// `MAX_NAME_LEN` for every profile that builds `vfs/dcache.c` (the small
/// profile that uses 32 excludes the dcache).
pub const DCACHE_NAME_LEN: usize = 256;
/// Rust mirror of `vfs_dcache_entry_t` for those profiles.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct DcacheEntry {
    pub used: c_int,
    pub hash_next: c_int,
    pub hash_prev: c_int,
    pub free_next: c_int,
    pub lru_next: c_int,
    pub lru_prev: c_int,
    pub mnt: usize,
    pub parent_ino: u64,
    pub name: [u8; DCACHE_NAME_LEN],
    pub vn: usize,
    pub accessed: u8,
}

const _: [(); 6 * 4 + 8 + 8 + DCACHE_NAME_LEN + 8 + 1 + 7] =
    [(); core::mem::size_of::<DcacheEntry>()];
const _: [(); 24] = [(); core::mem::offset_of!(DcacheEntry, mnt)];
const _: [(); 40] = [(); core::mem::offset_of!(DcacheEntry, name)];

pub const EVICT_TAKE: i32 = 1;
pub const EVICT_SECOND_CHANCE: i32 = 2;

const HASH_MASK: u32 = 511; // VFS_DCACHE_HASH_SIZE - 1 (1<<9)

#[inline]
unsafe fn ent<'a>(entries: *mut DcacheEntry, i: i32) -> &'a mut DcacheEntry {
    // SAFETY: callers pass a live base pointer and an in-range index.
    unsafe { &mut *entries.add(i as usize) }
}

/// FNV-ish rolling hash over the mount pointer, parent inode and name.
#[no_mangle]
pub unsafe extern "C" fn a20_dcache_hash_key(
    mnt: usize,
    ino: u64,
    name: *const u8,
) -> u32 {
    let mut h = (mnt as u32) ^ (ino as u32);
    if !name.is_null() {
        let mut p = name;
        // SAFETY: name is a NUL-terminated C string.
        unsafe {
            while *p != 0 {
                h = h.wrapping_mul(31).wrapping_add(*p as u32);
                p = p.add(1);
            }
        }
    }
    h & HASH_MASK
}

/// Initialise the whole table.  C holds the global lock and calls this once.
#[no_mangle]
pub unsafe extern "C" fn a20_dcache_init(
    entries: *mut DcacheEntry,
    n: usize,
    hash: *mut c_int,
    hash_size: usize,
    free_list: *mut c_int,
    free_count: *mut c_int,
    lru_head: *mut c_int,
    lru_tail: *mut c_int,
) {
    if entries.is_null() || hash.is_null() {
        return;
    }
    // SAFETY: n entries and hash_size buckets are live.
    unsafe {
        for i in 0..n {
            let e = ent(entries, i as i32);
            e.hash_next = -1;
            e.hash_prev = -1;
            e.free_next = if i + 1 < n { (i + 1) as i32 } else { -1 };
            e.lru_next = -1;
            e.lru_prev = -1;
            e.used = 0;
            e.accessed = 0;
        }
        for i in 0..hash_size {
            *hash.add(i) = -1;
        }
        *free_list = if n > 0 { 0 } else { -1 };
        *free_count = n as c_int;
        *lru_head = -1;
        *lru_tail = -1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_dcache_link_hash(
    entries: *mut DcacheEntry,
    hash: *mut c_int,
    slot: c_int,
    h: u32,
) {
    // SAFETY: caller holds the bucket lock for h.
    unsafe {
        let head = *hash.add(h as usize);
        let e = ent(entries, slot);
        e.hash_next = head;
        e.hash_prev = -1;
        if head >= 0 {
            ent(entries, head).hash_prev = slot;
        }
        *hash.add(h as usize) = slot;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_dcache_unlink_hash(
    entries: *mut DcacheEntry,
    hash: *mut c_int,
    slot: c_int,
    h: u32,
) {
    // SAFETY: caller holds the bucket lock for h.
    unsafe {
        let (prev, next) = {
            let e = ent(entries, slot);
            (e.hash_prev, e.hash_next)
        };
        if prev >= 0 {
            ent(entries, prev).hash_next = next;
        } else {
            *hash.add(h as usize) = next;
        }
        if next >= 0 {
            ent(entries, next).hash_prev = prev;
        }
        let e = ent(entries, slot);
        e.hash_next = -1;
        e.hash_prev = -1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_dcache_lru_remove(
    entries: *mut DcacheEntry,
    lru_head: *mut c_int,
    lru_tail: *mut c_int,
    slot: c_int,
) {
    // SAFETY: caller holds the global lock.
    unsafe {
        let (prev, next) = {
            let e = ent(entries, slot);
            (e.lru_prev, e.lru_next)
        };
        if prev >= 0 {
            ent(entries, prev).lru_next = next;
        } else {
            *lru_head = next;
        }
        if next >= 0 {
            ent(entries, next).lru_prev = prev;
        } else {
            *lru_tail = prev;
        }
        let e = ent(entries, slot);
        e.lru_next = -1;
        e.lru_prev = -1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_dcache_lru_insert_front(
    entries: *mut DcacheEntry,
    lru_head: *mut c_int,
    lru_tail: *mut c_int,
    slot: c_int,
) {
    // SAFETY: caller holds the global lock.
    unsafe {
        let head = *lru_head;
        let e = ent(entries, slot);
        e.lru_prev = -1;
        e.lru_next = head;
        if head >= 0 {
            ent(entries, head).lru_prev = slot;
        } else {
            *lru_tail = slot;
        }
        *lru_head = slot;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_dcache_lru_touch(
    entries: *mut DcacheEntry,
    lru_head: *mut c_int,
    lru_tail: *mut c_int,
    slot: c_int,
) {
    // SAFETY: caller holds the global lock.
    unsafe {
        if *lru_head == slot {
            return;
        }
        a20_dcache_lru_remove(entries, lru_head, lru_tail, slot);
        a20_dcache_lru_insert_front(entries, lru_head, lru_tail, slot);
    }
}

/// Take a slot from the free list, or -1 when empty.  Caller holds the global
/// lock.  The C eviction loop handles the no-free-slot path.
#[no_mangle]
pub unsafe extern "C" fn a20_dcache_alloc_free(
    entries: *mut DcacheEntry,
    n: usize,
    free_list: *mut c_int,
    free_count: *mut c_int,
) -> c_int {
    // SAFETY: caller holds the global lock.
    unsafe {
        if *free_count <= 0 {
            return -1;
        }
        let slot = *free_list;
        if slot < 0 || slot as usize >= n {
            return -1;
        }
        *free_list = ent(entries, slot).free_next;
        ent(entries, slot).free_next = -1;
        *free_count -= 1;
        slot
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_dcache_free_push(
    entries: *mut DcacheEntry,
    free_list: *mut c_int,
    free_count: *mut c_int,
    slot: c_int,
) {
    // SAFETY: caller holds the global lock.
    unsafe {
        ent(entries, slot).free_next = *free_list;
        *free_list = slot;
        *free_count += 1;
    }
}

/// Exact-match search of one hash chain.  Returns the matching slot index or
/// -1.  Caller holds the bucket lock; a hit leaves `accessed` untouched (the
/// adapter sets it only after `vnode_get_unless_zero` succeeds).
#[no_mangle]
pub unsafe extern "C" fn a20_dcache_match(
    entries: *mut DcacheEntry,
    hash: *mut c_int,
    h: u32,
    mnt: usize,
    ino: u64,
    name: *const u8,
) -> c_int {
    if name.is_null() {
        return -1;
    }
    // SAFETY: caller holds the bucket lock for h; the chain is well formed.
    unsafe {
        let mut i = *hash.add(h as usize);
        while i >= 0 {
            let e = ent(entries, i);
            if e.used != 0 && e.mnt == mnt && e.parent_ino == ino && name_eq(&e.name, name) {
                return i;
            }
            i = e.hash_next;
        }
    }
    -1
}

unsafe fn name_eq(field: &[u8; DCACHE_NAME_LEN], name: *const u8) -> bool {
    // SAFETY: name is a NUL-terminated C string.
    unsafe {
        let mut i = 0usize;
        loop {
            let c = *name.add(i);
            if i >= DCACHE_NAME_LEN {
                return false;
            }
            if field[i] != c {
                return false;
            }
            if c == 0 {
                return true;
            }
            i += 1;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_dcache_mark_accessed(entries: *mut DcacheEntry, slot: c_int) {
    // SAFETY: caller holds the bucket lock.
    unsafe { ent(entries, slot).accessed = 1; }
}

/// Eviction policy: a used slot with `accessed` set earns one reprieve.
#[no_mangle]
pub extern "C" fn a20_dcache_evict_decide(used: c_int, accessed: c_int) -> c_int {
    if used == 0 {
        return EVICT_TAKE;
    }
    if accessed != 0 {
        return EVICT_SECOND_CHANCE;
    }
    EVICT_TAKE
}

/// Drop one hash chain, pushing each retired vnode handle into `retire`.
/// Caller holds the bucket lock for h and the invalidate lock.  Returns the
/// number of vnode handles pushed.
#[no_mangle]
pub unsafe extern "C" fn a20_dcache_drop_chain(
    entries: *mut DcacheEntry,
    hash: *mut c_int,
    h: u32,
    retire: *mut usize,
    retire_count: *mut u32,
    retire_cap: usize,
) -> u32 {
    // SAFETY: caller holds the bucket lock for h; retire has room for
    // retire_cap entries.
    unsafe {
        let mut pushed = 0u32;
        let mut i = *hash.add(h as usize);
        while i >= 0 {
            let e = ent(entries, i);
            let next = e.hash_next;
            if e.used != 0 {
                if e.vn != 0 && (*retire_count as usize) < retire_cap {
                    *retire.add(*retire_count as usize) = e.vn;
                    *retire_count += 1;
                    pushed += 1;
                }
                e.used = 0;
                e.vn = 0;
                e.accessed = 0;
                e.hash_next = -1;
                e.hash_prev = -1;
            }
            i = next;
        }
        *hash.add(h as usize) = -1;
        pushed
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    const N: usize = 8;
    const HS: usize = 16;

    struct World {
        entries: Vec<DcacheEntry>,
        hash: Vec<c_int>,
        free_list: c_int,
        free_count: c_int,
        lru_head: c_int,
        lru_tail: c_int,
    }

    impl World {
        fn new() -> Self {
            let entries = vec![
                DcacheEntry {
                    used: 0,
                    hash_next: -1,
                    hash_prev: -1,
                    free_next: -1,
                    lru_next: -1,
                    lru_prev: -1,
                    mnt: 0,
                    parent_ino: 0,
                    name: [0; DCACHE_NAME_LEN],
                    vn: 0,
                    accessed: 0,
                };
                N
            ];
            let mut w = World {
                entries,
                hash: vec![-1; HS],
                free_list: -1,
                free_count: 0,
                lru_head: -1,
                lru_tail: -1,
            };
            unsafe {
                a20_dcache_init(
                    w.entries.as_mut_ptr(), N,
                    w.hash.as_mut_ptr(), HS,
                    &mut w.free_list, &mut w.free_count,
                    &mut w.lru_head, &mut w.lru_tail,
                );
            }
            w
        }
        #[allow(dead_code)]
        fn chain(&self, h: u32) -> Vec<i32> {
            let mut out = Vec::new();
            let mut i = self.hash[h as usize];
            while i >= 0 {
                out.push(i);
                i = self.entries[i as usize].hash_next;
            }
            out
        }
    }

    #[test]
    fn init_builds_free_list_and_lru() {
        let w = World::new();
        assert_eq!(w.free_count, N as c_int);
        assert_eq!(w.free_list, 0);
        assert_eq!(w.lru_head, -1);
        assert_eq!(w.entries[N - 1].free_next, -1);
    }

    #[test]
    fn hash_link_unlink_roundtrip() {
        let mut w = World::new();
        unsafe {
            for slot in 0..4 {
                a20_dcache_link_hash(w.entries.as_mut_ptr(), w.hash.as_mut_ptr(), slot, 3);
            }
            assert_eq!(w.chain(3), vec![3, 2, 1, 0]);
            a20_dcache_unlink_hash(w.entries.as_mut_ptr(), w.hash.as_mut_ptr(), 1, 3);
            assert_eq!(w.chain(3), vec![3, 2, 0]);
            a20_dcache_unlink_hash(w.entries.as_mut_ptr(), w.hash.as_mut_ptr(), 3, 3);
            assert_eq!(w.chain(3), vec![2, 0]);
            assert_eq!(w.hash[3], 2);
        }
    }

    #[test]
    fn lru_front_remove_touch() {
        unsafe fn read_list(ep: *mut DcacheEntry, head: c_int) -> Vec<i32> {
            let mut out = Vec::new();
            let mut i = head;
            while i >= 0 {
                out.push(i);
                i = (*ep.add(i as usize)).lru_next;
            }
            out
        }
        let mut w = World::new();
        unsafe {
            let ep = w.entries.as_mut_ptr();
            let hp: *mut c_int = &mut w.lru_head;
            let tp: *mut c_int = &mut w.lru_tail;
            for slot in 0..4 {
                a20_dcache_lru_insert_front(ep, hp, tp, slot);
            }
            assert_eq!(read_list(ep, *hp), vec![3, 2, 1, 0]);
            a20_dcache_lru_touch(ep, hp, tp, 0);
            assert_eq!(read_list(ep, *hp), vec![0, 3, 2, 1]);
            assert_eq!(*tp, 1);
            a20_dcache_lru_remove(ep, hp, tp, 0);
            assert_eq!(read_list(ep, *hp), vec![3, 2, 1]);
            assert_eq!(*hp, 3);
            assert_eq!(*tp, 1);
            a20_dcache_lru_remove(ep, hp, tp, 1);
            assert_eq!(read_list(ep, *hp), vec![3, 2]);
            assert_eq!(*tp, 2);
        }
    }

    #[test]
    fn free_take_push() {
        let mut w = World::new();
        unsafe {
            let (ep, fp, cp) = (w.entries.as_mut_ptr(), &mut w.free_list, &mut w.free_count);
            assert_eq!(a20_dcache_alloc_free(ep, N, fp, cp), 0);
            assert_eq!(a20_dcache_alloc_free(ep, N, fp, cp), 1);
            a20_dcache_free_push(ep, fp, cp, 0);
            assert_eq!(a20_dcache_alloc_free(ep, N, fp, cp), 0);
            assert_eq!(*cp, N as c_int - 2);
        }
    }

    #[test]
    fn match_and_evict_policy() {
        let mut w = World::new();
        unsafe {
            let e = &mut w.entries[2];
            e.used = 1;
            e.mnt = 0x1000;
            e.parent_ino = 7;
            e.name[..3].copy_from_slice(b"ab\0");
            a20_dcache_link_hash(w.entries.as_mut_ptr(), w.hash.as_mut_ptr(), 2, 5);
            let name = b"ab\0".as_ptr();
            assert_eq!(
                a20_dcache_match(w.entries.as_mut_ptr(), w.hash.as_mut_ptr(), 5, 0x1000, 7, name),
                2
            );
            assert_eq!(
                a20_dcache_match(w.entries.as_mut_ptr(), w.hash.as_mut_ptr(), 5, 0x1000, 8, name),
                -1
            );
        }
        assert_eq!(a20_dcache_evict_decide(0, 1), EVICT_TAKE);
        assert_eq!(a20_dcache_evict_decide(1, 1), EVICT_SECOND_CHANCE);
        assert_eq!(a20_dcache_evict_decide(1, 0), EVICT_TAKE);
    }

    #[test]
    fn hash_key_stable() {
        let name = b"hello\0".as_ptr();
        unsafe {
            assert_eq!(
                a20_dcache_hash_key(0x1234, 9, name),
                a20_dcache_hash_key(0x1234, 9, name)
            );
            assert!(a20_dcache_hash_key(0x1234, 9, name) <= HASH_MASK);
        }
    }
}
