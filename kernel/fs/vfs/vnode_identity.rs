//! Canonical vnode identity index (Rust-owned split for the VFS).
//!
//! One live vnode per `(superblock, inode)` pair.  The C adapter owns the
//! IRQ-save spinlock, vnode allocation/release, and the vnode reference each
//! index entry holds; this module owns the decisions: which vnode a key
//! resolves to, duplicate handling, entry removal, per-superblock clearing,
//! and the rotating cache-only eviction scan.
//!
//! Handles (`sb`, `vnode`) are opaque `usize` tokens: this module never
//! dereferences them.  `superblock` is the address of the adapter's live
//! superblock object; the adapter must clear a superblock's entries before the
//! object can be reused by a different mount.
//!
//! Reference contract (adapter side), all under its spinlock:
//!  - `lookup`/`insert` return a vnode whose entry owns one reference; the
//!    adapter must take its own caller reference with `vnode_get` before
//!    dropping the lock;
//!  - on `INSERTED` the adapter must `vnode_get` the inserted vnode once so
//!    the entry owns exactly one reference;
//!  - `remove`, `clear_sb` and `prune_batch` hand the entry's reference to the
//!    caller, which must `vnode_put` it *outside* the lock;
//!  - `set_ref_peek` registers a non-blocking total-reference-count reader
//!    used only for the cache-only eviction decision.
//!
//! The state is fixed-size and allocation-free so it can live in `.bss`.
//! Sequence counters are logical, not atomic: every entry point must be
//! called with the adapter's spinlock held.
#![cfg_attr(not(test), no_std)]

/// Insert succeeded; the adapter must take the entry's reference.
pub const A20_VNODE_IDENTITY_INSERTED: i32 = 0;
/// The key already had a vnode; `existing` is set and nothing changed.
pub const A20_VNODE_IDENTITY_DUPLICATE: i32 = 1;
/// No free slot and no cache-only victim could be produced by `prune_batch`.
pub const A20_VNODE_IDENTITY_FULL: i32 = 2;

const MAX_ENTRIES: usize = 16384;
const HASH_BUCKETS: usize = 8192;
const NO_SLOT: i32 = -1;

/// Result of `a20_vnode_identity_insert`.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct A20VnodeIdentityInsertOut {
    pub status: i32,
    pub existing: usize,
}

type RefPeek = unsafe extern "C" fn(vnode: usize) -> usize;

#[derive(Clone, Copy)]
struct Entry {
    sb: usize,
    ino: u64,
    vnode: usize,
    hash_next: i32,
    free_next: i32,
    used: u8,
}

struct State {
    entries: [Entry; MAX_ENTRIES],
    buckets: [i32; HASH_BUCKETS],
    free_head: i32,
    count: usize,
    prune_cursor: usize,
    initialized: bool,
    ref_peek: Option<RefPeek>,
}

/// All-zero image so the table is placed in `.bss` instead of carrying a
/// 688 KiB `.data` initializer (the `Entry` free-chain uses `-1`/`NO_SLOT`,
/// which would otherwise force a fully-initialised image).  `init_locked`
/// rebuilds the free chain and bucket sentinels on first use, exactly like the
/// C reference model; zero is a valid all-empty field value for every member
/// (`used == 0`, `None` peek, and hash/free links only ever read after init).
static mut STATE: State = unsafe { core::mem::zeroed() };

#[inline]
fn state() -> &'static mut State {
    // Serialized by the adapter's spinlock; references never escape a call.
    unsafe { &mut *core::ptr::addr_of_mut!(STATE) }
}

fn hash_key(sb: usize, ino: u64) -> usize {
    let mut h = (sb as u64).wrapping_mul(0x9E37_79B9_7F4A_7C15) ^ ino;
    h ^= h >> 33;
    h = h.wrapping_mul(0xff51_afd7_ed55_8ccd);
    h ^= h >> 33;
    (h as usize) & (HASH_BUCKETS - 1)
}

fn init_locked(s: &mut State) {
    if s.initialized {
        return;
    }
    let mut i = 0;
    while i < MAX_ENTRIES {
        let e = &mut s.entries[i];
        e.sb = 0;
        e.ino = 0;
        e.vnode = 0;
        e.hash_next = NO_SLOT;
        e.free_next = if i + 1 < MAX_ENTRIES {
            (i + 1) as i32
        } else {
            NO_SLOT
        };
        e.used = 0;
        i += 1;
    }
    let mut b = 0;
    while b < HASH_BUCKETS {
        s.buckets[b] = NO_SLOT;
        b += 1;
    }
    s.free_head = 0;
    s.count = 0;
    s.prune_cursor = 0;
    s.initialized = true;
}

fn find_locked(s: &State, sb: usize, ino: u64) -> i32 {
    let mut i = s.buckets[hash_key(sb, ino)];
    while i >= 0 {
        let e = &s.entries[i as usize];
        if e.used != 0 && e.sb == sb && e.ino == ino {
            return i;
        }
        i = e.hash_next;
    }
    NO_SLOT
}

fn unlink_hash_locked(s: &mut State, slot: usize) {
    let (sb, ino, next) = {
        let e = &s.entries[slot];
        (e.sb, e.ino, e.hash_next)
    };
    let bucket = hash_key(sb, ino);
    let mut prev: i32 = NO_SLOT;
    let mut cur = s.buckets[bucket];
    while cur >= 0 {
        if cur == slot as i32 {
            if prev >= 0 {
                s.entries[prev as usize].hash_next = next;
            } else {
                s.buckets[bucket] = next;
            }
            break;
        }
        prev = cur;
        cur = s.entries[cur as usize].hash_next;
    }
    s.entries[slot].hash_next = NO_SLOT;
}

/// Unlink a live entry and return its vnode reference for the caller to drop.
fn detach_locked(s: &mut State, slot: usize) -> usize {
    unlink_hash_locked(s, slot);
    let vnode;
    {
        let e = &mut s.entries[slot];
        vnode = e.vnode;
        e.used = 0;
        e.sb = 0;
        e.ino = 0;
        e.vnode = 0;
        e.free_next = s.free_head;
    }
    s.free_head = slot as i32;
    s.count -= 1;
    vnode
}

fn lookup_impl(s: &State, sb: usize, ino: u64) -> usize {
    // The table starts as a zero image; `init_locked` must run before any
    // bucket walk so a zero `hash_next` is never mistaken for a live link.
    let slot = find_locked(s, sb, ino);
    if slot < 0 {
        0
    } else {
        s.entries[slot as usize].vnode
    }
}

fn insert_impl(s: &mut State, sb: usize, ino: u64, vnode: usize) -> A20VnodeIdentityInsertOut {
    init_locked(s);
    let dup = find_locked(s, sb, ino);
    if dup >= 0 {
        return A20VnodeIdentityInsertOut {
            status: A20_VNODE_IDENTITY_DUPLICATE,
            existing: s.entries[dup as usize].vnode,
        };
    }
    let slot = s.free_head;
    if slot < 0 {
        return A20VnodeIdentityInsertOut {
            status: A20_VNODE_IDENTITY_FULL,
            existing: 0,
        };
    }
    s.free_head = s.entries[slot as usize].free_next;
    let bucket = hash_key(sb, ino);
    {
        let e = &mut s.entries[slot as usize];
        e.used = 1;
        e.sb = sb;
        e.ino = ino;
        e.vnode = vnode;
        e.hash_next = s.buckets[bucket];
        e.free_next = NO_SLOT;
    }
    s.buckets[bucket] = slot;
    s.count += 1;
    A20VnodeIdentityInsertOut {
        status: A20_VNODE_IDENTITY_INSERTED,
        existing: vnode,
    }
}

fn remove_impl(s: &mut State, sb: usize, ino: u64) -> usize {
    init_locked(s);
    let slot = find_locked(s, sb, ino);
    if slot < 0 {
        0
    } else {
        detach_locked(s, slot as usize)
    }
}

fn clear_sb_impl(s: &mut State, sb: usize, out: *mut usize, cap: usize) -> usize {
    init_locked(s);
    if out.is_null() || cap == 0 {
        return 0;
    }
    let mut n = 0usize;
    let mut i = 0usize;
    while i < MAX_ENTRIES && n < cap {
        if s.entries[i].used != 0 && s.entries[i].sb == sb {
            let victim = detach_locked(s, i);
            // SAFETY: `out` has room for `cap` entries and n < cap here.
            unsafe { *out.add(n) = victim };
            n += 1;
        }
        i += 1;
    }
    n
}

fn prune_batch_impl(s: &mut State, out: *mut usize, cap: usize) -> usize {
    init_locked(s);
    if out.is_null() || cap == 0 {
        return 0;
    }
    let peek = match s.ref_peek {
        Some(f) => f,
        None => return 0,
    };
    let mut n = 0usize;
    let mut scanned = 0usize;
    while scanned < MAX_ENTRIES && n < cap {
        let i = s.prune_cursor;
        s.prune_cursor = if i + 1 == MAX_ENTRIES { 0 } else { i + 1 };
        scanned += 1;
        if s.entries[i].used == 0 {
            continue;
        }
        let vnode = s.entries[i].vnode;
        // Cache-only means the entry owns the vnode's only reference. The
        // callback is the adapter's non-blocking refcount reader.
        if unsafe { peek(vnode) } <= 1 {
            let victim = detach_locked(s, i);
            // SAFETY: `out` has room for `cap` entries and n < cap here.
            unsafe { *out.add(n) = victim };
            n += 1;
        }
    }
    n
}

/// Register the adapter's non-blocking total-reference-count reader.
///
/// # Safety
/// `peek` must be callable with the adapter's spinlock held, must not sleep or
/// re-enter this module, and must return the live reference count for a vnode
/// the index still owns.
#[no_mangle]
pub unsafe extern "C" fn a20_vnode_identity_set_ref_peek(peek: Option<RefPeek>) {
    let s = state();
    // Lazy init here as well so a registration that races with the first
    // insert cannot be lost to the init path clearing the field.
    init_locked(s);
    s.ref_peek = peek;
}

/// Resolve `(sb, ino)` to a vnode handle, or 0.  Caller holds the spinlock and
/// must take its own vnode reference before unlocking.
#[no_mangle]
pub unsafe extern "C" fn a20_vnode_identity_lookup(sb: usize, ino: u64) -> usize {
    let s = state();
    init_locked(s);
    lookup_impl(s, sb, ino)
}

/// Insert `vnode` for `(sb, ino)`.
///
/// `out` receives `status` and, for `DUPLICATE`, the existing vnode handle.
/// On `INSERTED` the adapter must `vnode_get(vnode)` once; on `DUPLICATE` it
/// must discard the new object and use `existing`; on `FULL` nothing changed.
///
/// # Safety
/// `out` must be a valid, writable pointer for the duration of the call.
#[no_mangle]
pub unsafe extern "C" fn a20_vnode_identity_insert(
    sb: usize,
    ino: u64,
    vnode: usize,
    out: *mut A20VnodeIdentityInsertOut,
) {
    if out.is_null() {
        return;
    }
    let s = state();
    // SAFETY: caller-provided writable pointer.
    unsafe { *out = insert_impl(s, sb, ino, vnode) };
}

/// Remove `(sb, ino)`; returns the vnode whose entry reference the caller must
/// drop outside the lock, or 0 when the key was absent.
#[no_mangle]
pub unsafe extern "C" fn a20_vnode_identity_remove(sb: usize, ino: u64) -> usize {
    let s = state();
    remove_impl(s, sb, ino)
}

/// Remove up to `cap` entries for `sb`; returns the count written to `out`.
/// Each returned handle carries a reference the caller must drop outside the
/// lock.  Call repeatedly until it returns 0.
///
/// # Safety
/// `out` must be valid for `cap` `usize` values.
#[no_mangle]
pub unsafe extern "C" fn a20_vnode_identity_clear_sb(
    sb: usize,
    out: *mut usize,
    cap: usize,
) -> usize {
    let s = state();
    clear_sb_impl(s, sb, out, cap)
}

/// Collect up to `cap` cache-only entries (refcount <= 1), rotating a cursor
/// across calls so a full sweep terminates.  Returns the count written to
/// `out`; the caller drops each reference outside the lock.
///
/// # Safety
/// `out` must be valid for `cap` `usize` values.
#[no_mangle]
pub unsafe extern "C" fn a20_vnode_identity_prune_batch(out: *mut usize, cap: usize) -> usize {
    let s = state();
    prune_batch_impl(s, out, cap)
}

/// Current live entry count (diagnostics/tests).
#[no_mangle]
pub unsafe extern "C" fn a20_vnode_identity_count() -> usize {
    let s = state();
    if !s.initialized {
        return 0;
    }
    s.count
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{Mutex, MutexGuard};

    static TEST_LOCK: Mutex<()> = Mutex::new(());
    static PEEK_REFS: core::sync::atomic::AtomicUsize =
        core::sync::atomic::AtomicUsize::new(1);

    extern "C" fn test_peek(_vnode: usize) -> usize {
        PEEK_REFS.load(core::sync::atomic::Ordering::Relaxed)
    }

    fn lock() -> MutexGuard<'static, ()> {
        TEST_LOCK.lock().unwrap_or_else(|e| e.into_inner())
    }

    unsafe fn insert(sb: usize, ino: u64, vnode: usize) -> A20VnodeIdentityInsertOut {
        let mut out = A20VnodeIdentityInsertOut {
            status: -1,
            existing: 0,
        };
        a20_vnode_identity_insert(sb, ino, vnode, &mut out);
        out
    }

    #[test]
    fn insert_lookup_remove_roundtrip() {
        let _g = lock();
        let sb = 0xa008;
        unsafe {
            assert_eq!(a20_vnode_identity_lookup(sb, 41), 0);
            let out = insert(sb, 41, 0x1000);
            assert_eq!(out.status, A20_VNODE_IDENTITY_INSERTED);
            assert_eq!(a20_vnode_identity_lookup(sb, 41), 0x1000);
            // A second insert for the same key reports the canonical handle.
            let dup = insert(sb, 41, 0x2000);
            assert_eq!(dup.status, A20_VNODE_IDENTITY_DUPLICATE);
            assert_eq!(dup.existing, 0x1000);
            assert_eq!(a20_vnode_identity_lookup(sb, 41), 0x1000);
            assert_eq!(a20_vnode_identity_remove(sb, 41), 0x1000);
            assert_eq!(a20_vnode_identity_lookup(sb, 41), 0);
            assert_eq!(a20_vnode_identity_remove(sb, 41), 0);
        }
    }

    #[test]
    fn keys_are_scoped_per_superblock() {
        let _g = lock();
        unsafe {
            assert_eq!(insert(0xb001, 7, 0x3000).status, A20_VNODE_IDENTITY_INSERTED);
            assert_eq!(insert(0xb002, 7, 0x3001).status, A20_VNODE_IDENTITY_INSERTED);
            assert_eq!(a20_vnode_identity_lookup(0xb001, 7), 0x3000);
            assert_eq!(a20_vnode_identity_lookup(0xb002, 7), 0x3001);
            assert_eq!(a20_vnode_identity_remove(0xb001, 7), 0x3000);
            assert_eq!(a20_vnode_identity_lookup(0xb002, 7), 0x3001);
            assert_eq!(a20_vnode_identity_remove(0xb002, 7), 0x3001);
        }
    }

    #[test]
    fn clear_sb_returns_only_that_superblocks_entries() {
        let _g = lock();
        let mut buf = [0usize; 8];
        unsafe {
            assert_eq!(insert(0xc001, 1, 0x4000).status, A20_VNODE_IDENTITY_INSERTED);
            assert_eq!(insert(0xc001, 2, 0x4001).status, A20_VNODE_IDENTITY_INSERTED);
            assert_eq!(insert(0xc002, 1, 0x4002).status, A20_VNODE_IDENTITY_INSERTED);
            let n = a20_vnode_identity_clear_sb(0xc001, buf.as_mut_ptr(), 8);
            assert_eq!(n, 2);
            assert!(buf[..2].contains(&0x4000) && buf[..2].contains(&0x4001));
            assert_eq!(a20_vnode_identity_clear_sb(0xc001, buf.as_mut_ptr(), 8), 0);
            assert_eq!(a20_vnode_identity_lookup(0xc002, 1), 0x4002);
            assert_eq!(a20_vnode_identity_remove(0xc002, 1), 0x4002);
        }
    }

    #[test]
    fn prune_removes_only_cache_only_entries() {
        let _g = lock();
        let mut buf = [0usize; 16];
        unsafe {
            a20_vnode_identity_set_ref_peek(Some(test_peek));
            assert_eq!(insert(0xd001, 10, 0x5000).status, A20_VNODE_IDENTITY_INSERTED);
            assert_eq!(insert(0xd001, 11, 0x5001).status, A20_VNODE_IDENTITY_INSERTED);
            // Both entries are cache-only: the scan evicts them in one sweep.
            PEEK_REFS.store(1, core::sync::atomic::Ordering::Relaxed);
            let n = a20_vnode_identity_prune_batch(buf.as_mut_ptr(), 16);
            assert_eq!(n, 2);
            assert_eq!(a20_vnode_identity_lookup(0xd001, 10), 0);
            assert_eq!(a20_vnode_identity_lookup(0xd001, 11), 0);

            assert_eq!(insert(0xd001, 12, 0x5002).status, A20_VNODE_IDENTITY_INSERTED);
            assert_eq!(insert(0xd001, 13, 0x5003).status, A20_VNODE_IDENTITY_INSERTED);
            // A busy vnode (index + caller) survives.
            PEEK_REFS.store(2, core::sync::atomic::Ordering::Relaxed);
            assert_eq!(a20_vnode_identity_prune_batch(buf.as_mut_ptr(), 16), 0);
            assert_eq!(a20_vnode_identity_lookup(0xd001, 12), 0x5002);
            // Releasing the external reference makes both evictable again.
            PEEK_REFS.store(1, core::sync::atomic::Ordering::Relaxed);
            let n = a20_vnode_identity_prune_batch(buf.as_mut_ptr(), 16);
            assert_eq!(n, 2);
            a20_vnode_identity_set_ref_peek(None);
        }
    }

    #[test]
    fn table_exhaustion_reports_full_and_prune_recovers_capacity() {
        let _g = lock();
        let mut buf = [0usize; 8192];
        use core::sync::atomic::Ordering;
        unsafe {
            a20_vnode_identity_set_ref_peek(Some(test_peek));
            // Busy vnodes: the index reference plus a caller reference, so the
            // prune scan must not produce victims.
            PEEK_REFS.store(2, Ordering::Relaxed);
            let before = a20_vnode_identity_count();
            let sb = 0x0e05usize;
            let mut inserted = 0usize;
            loop {
                match insert(sb, inserted as u64, 0x1_0000 + inserted).status {
                    A20_VNODE_IDENTITY_INSERTED => inserted += 1,
                    A20_VNODE_IDENTITY_FULL => break,
                    other => panic!("unexpected insert status {other}"),
                }
            }
            assert_eq!(before + inserted, MAX_ENTRIES);
            let full = insert(sb, inserted as u64, 0x1_0000 + inserted);
            assert_eq!(full.status, A20_VNODE_IDENTITY_FULL);
            assert_eq!(full.existing, 0);
            assert_eq!(a20_vnode_identity_count(), MAX_ENTRIES);
            // Every inserted key still resolves.
            assert_eq!(a20_vnode_identity_lookup(sb, 0), 0x1_0000);
            assert_eq!(
                a20_vnode_identity_lookup(sb, (inserted - 1) as u64),
                0x1_0000 + inserted - 1
            );

            // All entries are busy: a full-capacity prune yields nothing and
            // drops nothing.
            assert_eq!(a20_vnode_identity_prune_batch(buf.as_mut_ptr(), 8192), 0);
            assert_eq!(a20_vnode_identity_count(), MAX_ENTRIES);

            // Releasing the external references makes every entry cache-only;
            // the rotating cursor sweeps the whole table over two bounded
            // batches and stops.
            PEEK_REFS.store(1, Ordering::Relaxed);
            let n1 = a20_vnode_identity_prune_batch(buf.as_mut_ptr(), 8192);
            assert_eq!(n1, 8192);
            let n2 = a20_vnode_identity_prune_batch(buf.as_mut_ptr(), 8192);
            assert_eq!(n2, MAX_ENTRIES - 8192);
            assert_eq!(a20_vnode_identity_count(), 0);
            assert_eq!(a20_vnode_identity_prune_batch(buf.as_mut_ptr(), 8192), 0);

            // A pruned slot accepts a new key again.
            assert_eq!(insert(sb, 1, 0x2_0000).status, A20_VNODE_IDENTITY_INSERTED);
            assert_eq!(a20_vnode_identity_lookup(sb, 1), 0x2_0000);
            assert_eq!(a20_vnode_identity_remove(sb, 1), 0x2_0000);
            a20_vnode_identity_set_ref_peek(None);
        }
    }

    #[test]
    fn insert_out_layout_matches_the_c_abi() {
        assert_eq!(
            core::mem::size_of::<A20VnodeIdentityInsertOut>(),
            2 * core::mem::size_of::<usize>()
        );
        assert_eq!(
            core::mem::align_of::<A20VnodeIdentityInsertOut>(),
            core::mem::align_of::<usize>()
        );
    }
}
