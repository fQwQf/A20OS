//! Block and page cache operation engine (Rust-owned split for `block_cache.c`).
//!
//! C supplies locks, allocation/reference primitives, simple warm-hit and byte
//! copy paths, read-ahead mechanics, and device-I/O callbacks. Rust owns cache
//! misses, fill/publication and duplicate resolution, eviction and rollback,
//! sync/writeback transactions, batch-read policy, dirty generations, holds,
//! quarantine and second-chance state. Transactions operate on C-compatible
//! mapped entries; storage and device operations stay in C.
//!
//! The two `#[repr(C)]` mirrors reproduce the LP64 layout exactly, including
//! `bcache_entry_t::data` at offset 33 (a `char[512]` after a `u8`) and
//! `pcache_entry_t::held` before its payload pointer. Byte-compatibility is what lets the
//! C reference model and this engine be driven by one differential harness.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_uint};
use core::sync::atomic::Ordering;

pub const BCACHE_HASH_BUCKETS: usize = 1024;
pub const PCACHE_MIN_PAGES: usize = 2000;
pub const PCACHE_MAX_PAGES: usize = 16384;
pub const PCACHE_TARGET_FRAME_DIVISOR: usize = 32;
pub const PCACHE_HASH_BUCKETS: usize = 4096;

/// Select page-cache descriptor capacity from physical memory. Payload frames
/// remain lazy: the target is RAM/32 frames, capped at 16384 slots (64 MiB of
/// payload), with the historical 2000-slot floor. On smaller RAM the floor
/// may exceed RAM/32. Descriptor bytes and other caches are outside this
/// per-cache payload target.
#[no_mangle]
pub extern "C" fn a20_pcache_page_limit(total_frames: usize) -> usize {
    let target = total_frames / PCACHE_TARGET_FRAME_DIVISOR;
    if target < PCACHE_MIN_PAGES {
        PCACHE_MIN_PAGES
    } else if target > PCACHE_MAX_PAGES {
        PCACHE_MAX_PAGES
    } else {
        target
    }
}

/// 512-byte metadata block pool entry. `data` at offset 33, size 576.
#[repr(C)]
pub struct BcacheEntry {
    pub lba: u64,
    pub dirty: c_int,
    pub dirty_gen: u64,
    pub ref_: c_int,
    pub valid: c_int,
    pub accessed: u8,
    pub data: [u8; 512],
    pub prev: *mut BcacheEntry,
    pub next: *mut BcacheEntry,
    pub hnext: *mut BcacheEntry,
}

/// Compact 4 KiB page descriptor. `data` is a lazily allocated payload owned
/// by this slot until cache destruction; `held` is at 36 and the pointer at 40.
/// The descriptor is 72 bytes on LP64 and 56 bytes on ILP32.
#[repr(C)]
pub struct PcacheEntry {
    pub page_no: u64,
    pub dirty: c_int,
    pub dirty_gen: u64,
    pub ref_: c_int,
    pub valid: c_int,
    pub accessed: u8,
    pub held: c_int,
    pub data: *mut u8,
    pub prev: *mut PcacheEntry,
    pub next: *mut PcacheEntry,
    pub hnext: *mut PcacheEntry,
}

/// Dirty page-slot bitmap paired with the descriptor pool it indexes.
/// Every field is protected by the owning block cache's global lock.
#[repr(C)]
pub struct PcacheDirtySet {
    pub count: usize,
    pub pool: *mut PcacheEntry,
    pub pool_count: usize,
    pub words: *mut u64,
}

#[inline]
fn pcache_dirty_slot(set: &PcacheDirtySet, entry: *mut PcacheEntry) -> Option<usize> {
    if set.pool.is_null() || set.words.is_null() || set.pool_count == 0 || entry.is_null() {
        return None;
    }
    let base = set.pool as usize;
    let address = entry as usize;
    let bytes = set.pool_count.checked_mul(core::mem::size_of::<PcacheEntry>())?;
    let end = base.checked_add(bytes)?;
    if address < base || address >= end {
        return None;
    }
    let offset = address - base;
    if offset % core::mem::size_of::<PcacheEntry>() != 0 {
        return None;
    }
    Some(offset / core::mem::size_of::<PcacheEntry>())
}

// Layout invariants.  The field offsets that do not depend on pointer width
// are asserted unconditionally; the total sizes differ between LP64 and ILP32
// (byte-compatible with the C struct on each target).
const _: [(); 33] = [(); core::mem::offset_of!(BcacheEntry, data)];
const _: [(); 40] = [(); core::mem::offset_of!(PcacheEntry, data)];
const _: [(); 36] = [(); core::mem::offset_of!(PcacheEntry, held)];
#[cfg(target_pointer_width = "64")]
const _: [(); 32] = [(); core::mem::size_of::<PcacheDirtySet>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 16] = [(); core::mem::size_of::<PcacheDirtySet>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 576] = [(); core::mem::size_of::<BcacheEntry>()];
#[cfg(target_pointer_width = "64")]
const _: [(); 72] = [(); core::mem::size_of::<PcacheEntry>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 560] = [(); core::mem::size_of::<BcacheEntry>()];
#[cfg(target_pointer_width = "32")]
const _: [(); 56] = [(); core::mem::size_of::<PcacheEntry>()];

/// Eviction policy verdict for one candidate.
pub const EVICT_SKIP: i32 = 0;
pub const EVICT_TAKE: i32 = 1;
/// Clear the second-chance `accessed` bit and skip this candidate.
pub const EVICT_SECOND_CHANCE: i32 = 2;

/// Sync modes, matching `enum bcache_sync_mode`.
pub const SYNC_ALL: i32 = 0;
pub const SYNC_SCOPED: i32 = 1;
pub const SYNC_HELD: i32 = 2;
pub type BcacheBucketLockFn = extern "C" fn(*mut core::ffi::c_void, u64) -> u64;
pub type BcacheBucketUnlockFn = extern "C" fn(*mut core::ffi::c_void, u64, u64);
pub type BcacheLockFn = extern "C" fn(*mut core::ffi::c_void) -> u64;
pub type BcacheUnlockFn = extern "C" fn(*mut core::ffi::c_void, u64);
pub type BcachePageActionFn = extern "C" fn(*mut core::ffi::c_void, u64);
pub type BcachePageFlushFn = extern "C" fn(*mut core::ffi::c_void, *mut PcacheEntry) -> c_int;
pub type BcachePageReadFn = extern "C" fn(*mut core::ffi::c_void, u64, *mut u8) -> c_int;
pub type PcachePageAllocFn = extern "C" fn(*mut core::ffi::c_void) -> *mut u8;
pub type PcachePageFreeFn = extern "C" fn(*mut u8);
pub type BcachePagePerfFn = extern "C" fn(*mut core::ffi::c_void, c_int);
pub type BcachePageDiagnosticFn = extern "C" fn(*mut core::ffi::c_void, u64);
pub type BcacheBlockIoFn = extern "C" fn(*mut core::ffi::c_void, u64, *mut u8, c_int) -> c_int;
pub type PcachePageFindFn = extern "C" fn(*mut core::ffi::c_void, u64) -> *mut PcacheEntry;
pub type PcacheWriteBatchFn = extern "C" fn(*mut core::ffi::c_void, u64, *const u8, usize) -> c_int;
pub type BcacheBatchReadFn = extern "C" fn(*mut core::ffi::c_void, u64, *mut u8, usize) -> c_int;
pub type BcacheRangeReadFn = extern "C" fn(*mut core::ffi::c_void, u64, *mut u8, usize) -> c_int;
pub type BcacheBatchPerfFn = extern "C" fn(*mut core::ffi::c_void, usize);

pub type SyncCacheLockFn = extern "C" fn(*mut core::ffi::c_void) -> u64;
pub type SyncCacheUnlockFn = extern "C" fn(*mut core::ffi::c_void, u64);
pub type SyncClearPageBatchFn = extern "C" fn(*mut core::ffi::c_void, *const usize, *const u64, *const u64, usize);
pub type SyncClearBlockFn = extern "C" fn(*mut core::ffi::c_void, usize, u64, u64);
pub type SyncWriteFn = extern "C" fn(*mut core::ffi::c_void, u64, *const u8, usize) -> c_int;
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SyncWrite {
    pub lba: u64,
    pub buf: *const u8,
    pub count: usize,
}
pub type SyncWriteManyFn = extern "C" fn(*mut core::ffi::c_void, *const SyncWrite, usize) -> c_int;
pub type SyncLockFn = extern "C" fn(*mut core::ffi::c_void);
pub type SyncErrorFn = extern "C" fn(*mut core::ffi::c_void);
pub type SyncFlushFn = extern "C" fn(*mut core::ffi::c_void) -> c_int;
/// Scratch storage is acquired and released while the exclusive writeback
/// lock is held; callbacks may lend cache-owned storage instead of allocating.
pub type SyncAllocFn = extern "C" fn(*mut core::ffi::c_void, usize) -> *mut u8;
pub type SyncFreeFn = extern "C" fn(*mut core::ffi::c_void, *mut u8);

/// Find the next set slot bit at or after `start`. The caller holds bc->lock
/// so bitmap updates cannot race the scan; clean slots cost no per-entry lock.
unsafe fn pcache_next_dirty_slot(
    dirty_set: *const PcacheDirtySet,
    start: usize,
) -> Option<usize> {
    if dirty_set.is_null() {
        return None;
    }
    unsafe {
        let set = &*dirty_set;
        if set.words.is_null() || start >= set.pool_count {
            return None;
        }
        let mut index = start;
        while index < set.pool_count {
            let word_index = index / 64;
            let bit_offset = index % 64;
            let bits = *set.words.add(word_index) & (u64::MAX << bit_offset);
            if bits != 0 {
                let found = word_index * 64 + bits.trailing_zeros() as usize;
                if found < set.pool_count {
                    return Some(found);
                }
                return None;
            }
            index = (word_index + 1) * 64;
        }
    }
    None
}

/// Find the next dirty slot that this sync mode can write. The caller holds
/// bc->lock, so both the bitmap and selection fields remain coherent while
/// clean, held, and out-of-scope slots are skipped.
unsafe fn pcache_next_selected_dirty_slot(
    dirty_set: *const PcacheDirtySet,
    page_pool: *mut PcacheEntry,
    page_count: usize,
    mode: c_int,
    page_nos: *const u64,
    selected_count: usize,
    start: usize,
) -> Option<usize> {
    let mut index = start;
    while index < page_count {
        let next = unsafe { pcache_next_dirty_slot(dirty_set, index) }?;
        let entry = unsafe { &*page_pool.add(next) };
        let selected = unsafe {
            a20_bcache_sync_select(
                mode,
                entry.valid,
                entry.dirty,
                entry.held,
                page_nos,
                selected_count,
                entry.page_no,
            )
        } != 0;
        if selected {
            return Some(next);
        }
        index = next + 1;
    }
    None
}

/// Own the page/block writeback scan, batching, generation snapshot and
/// flush transaction. The pools are stable C-compatible arrays. Rust reads
/// them under the supplied cache lock so a pool scan crosses the ABI once per
/// selected batch rather than once per descriptor.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_sync_transaction(
    context: *mut core::ffi::c_void,
    page_count: usize,
    block_count: usize,
    page_nos: *const u64,
    selected_count: usize,
    mode: c_int,
    page_pool: *mut PcacheEntry,
    dirty_set: *mut PcacheDirtySet,
    block_pool: *mut BcacheEntry,
    cache_lock: Option<SyncCacheLockFn>,
    cache_unlock: Option<SyncCacheUnlockFn>,
    clear_block: Option<SyncClearBlockFn>,
    clear_pages: Option<SyncClearPageBatchFn>,
    write: Option<SyncWriteFn>,
    write_many: Option<SyncWriteManyFn>,
    flush: Option<SyncFlushFn>,
    writeback_lock: Option<SyncLockFn>,
    writeback_unlock: Option<SyncLockFn>,
    note_error: Option<SyncErrorFn>,
    alloc: Option<SyncAllocFn>,
    free: Option<SyncFreeFn>,
) -> c_int {
    let (cache_lock, cache_unlock, clear_block, clear_pages, write, write_many, flush, writeback_lock,
         writeback_unlock, note_error, alloc, free) = match (
        cache_lock, cache_unlock, clear_block, clear_pages, write, write_many, flush, writeback_lock,
        writeback_unlock, note_error, alloc, free,
    ) {
        (Some(a), Some(b), Some(c), Some(d), Some(e), Some(f), Some(g), Some(h), Some(i), Some(j), Some(k), Some(l)) =>
            (a,b,c,d,e,f,g,h,i,j,k,l),
        _ => return -22,
    };
    if context.is_null()
        || page_pool.is_null()
        || dirty_set.is_null()
        || block_pool.is_null()
    {
        return -22;
    }
    unsafe {
        if (*dirty_set).pool != page_pool
            || (*dirty_set).pool_count != page_count
            || (page_count != 0 && (*dirty_set).words.is_null())
        {
            return -22;
        }
    }
    const PAGE: usize = 4096;
    const BLOCK: usize = 512;
    const BATCH: usize = 256;
    const VECTOR_BATCH: usize = 16;
    let mut first_error = 0;
    writeback_lock(context);
    let mut linear: *mut u8 = core::ptr::null_mut();
    let mut i = 0usize;
    while i < page_count {
        if linear.is_null() {
            // Avoid allocating the large scratch buffer when dirty pages are
            // all held or outside this scoped sync. Recheck the candidate in
            // the batch loop after unlocking in case concurrent state changed.
            let lock = cache_lock(context);
            let candidate = unsafe {
                pcache_next_selected_dirty_slot(
                    dirty_set,
                    page_pool,
                    page_count,
                    mode,
                    page_nos,
                    selected_count,
                    i,
                )
            };
            if let Some(index) = candidate {
                i = index;
            } else {
                i = page_count;
            }
            cache_unlock(context, lock);
            if i >= page_count {
                break;
            }
            /* The exclusive writeback lock is held for this transaction; the
             * borrowed scratch remains valid until released below. */
            linear = alloc(context, BATCH * PAGE);
            if linear.is_null() { first_error = -12; break; }
        }
        let mut numbers = [0u64; BATCH];
        let mut generations = [0u64; BATCH];
        let mut indices = [0usize; BATCH];
        let mut vectors = [SyncWrite { lba: 0, buf: core::ptr::null(), count: 0 }; VECTOR_BATCH];
        let mut count = 0usize;
        let mut vector_count = 0usize;
        while i < page_count && count < BATCH {
            let lock = cache_lock(context);
            let next = unsafe { pcache_next_dirty_slot(dirty_set, i) };
            let next = match next {
                Some(index) => index,
                None => {
                    cache_unlock(context, lock);
                    i = page_count;
                    break;
                }
            };
            i = next;
            let entry = unsafe { &mut *page_pool.add(i) };
            let selected = a20_bcache_sync_select(mode, entry.valid, entry.dirty,
                entry.held, page_nos, selected_count, entry.page_no) != 0;
            if !selected {
                cache_unlock(context, lock);
                i += 1;
                continue;
            }
            let page_no = entry.page_no;
            let generation = entry.dirty_gen;
            let lba = page_no.wrapping_mul(PAGE as u64 / BLOCK as u64);
            let adjacent = vector_count != 0 &&
                vectors[vector_count - 1].lba.wrapping_add(vectors[vector_count - 1].count as u64) == lba;
            if vector_count == VECTOR_BATCH && !adjacent {
                cache_unlock(context, lock);
                break; // keep i at this selected page for the next transaction
            }
            let buffer = unsafe { linear.add(count * PAGE) };
            unsafe { core::ptr::copy_nonoverlapping(entry.data, buffer, PAGE); }
            cache_unlock(context, lock);
            if adjacent {
                vectors[vector_count - 1].count += PAGE / BLOCK;
            } else {
                vectors[vector_count] = SyncWrite { lba, buf: buffer, count: PAGE / BLOCK };
                vector_count += 1;
            }
            numbers[count] = page_no;
            generations[count] = generation;
            indices[count] = i;
            count += 1;
            i += 1;
        }
        if count == 0 { continue; }
        let ret = write_many(context, vectors.as_ptr(), vector_count);
        if ret >= 0 {
            clear_pages(context, indices.as_ptr(), numbers.as_ptr(), generations.as_ptr(), count);
        } else {
            first_error = ret;
            note_error(context);
            break;
        }
    }
    /* Release the scratch borrow before dropping writeback_lock. The later
     * device flush does not access this buffer. */
    if !linear.is_null() { free(context, linear); }
    if first_error == 0 && mode != SYNC_HELD {
        let mut tmp = [0u8; BLOCK];
        for i in 0..block_count {
            let lock = cache_lock(context);
            let e = unsafe { &mut *block_pool.add(i) };
            if e.valid == 0 || e.dirty == 0 {
                cache_unlock(context, lock);
                continue;
            }
            let lba = e.lba;
            let generation = e.dirty_gen;
            unsafe { core::ptr::copy_nonoverlapping(e.data.as_ptr(), tmp.as_mut_ptr(), BLOCK); }
            cache_unlock(context, lock);
            let ret = write(context, lba, tmp.as_mut_ptr(), 1);
            if ret >= 0 {
                clear_block(context, i, lba, generation);
            } else { first_error = ret; note_error(context); break; }
        }
    }
    writeback_unlock(context);
    if first_error == 0 {
        let ret = flush(context);
        if ret < 0 { first_error = ret; }
    }
    first_error
}

// ------------------------------------------------------------------
// hashing
// ------------------------------------------------------------------

#[no_mangle]
pub extern "C" fn a20_bcache_hash_key(lba: u64) -> c_uint {
    ((lba ^ (lba >> 32)) & (BCACHE_HASH_BUCKETS as u64 - 1)) as c_uint
}

#[no_mangle]
pub extern "C" fn a20_pcache_hash_key(page_no: u64) -> c_uint {
    ((page_no ^ (page_no >> 32)) & (PCACHE_HASH_BUCKETS as u64 - 1)) as c_uint
}

// ------------------------------------------------------------------
// reference counts (saturating at zero, matching cache_ref_*)
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_ref_read(r: *const c_int) -> c_int {
    if r.is_null() {
        return 0;
    }
    // SAFETY: caller passes a live `ref` field; acquire load matches C.
    unsafe { (*(r as *const core::sync::atomic::AtomicI32)).load(Ordering::Acquire) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_ref_get(r: *mut c_int) {
    if r.is_null() {
        return;
    }
    // SAFETY: atomic RMW on the caller's int field, matching `cache_ref_get`.
    unsafe {
        (*(r as *mut core::sync::atomic::AtomicI32)).fetch_add(1, Ordering::Acquire);
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_ref_put(r: *mut c_int) {
    if r.is_null() {
        return;
    }
    // SAFETY: saturating CAS decrement that never goes negative, matching the
    // C `cache_ref_put` loop under the same atomic discipline.
    unsafe {
        let a = r as *mut core::sync::atomic::AtomicI32;
        let mut cur = (*a).load(Ordering::Acquire);
        while cur > 0 {
            match (*a).compare_exchange_weak(
                cur,
                cur - 1,
                Ordering::Release,
                Ordering::Acquire,
            ) {
                Ok(_) => return,
                Err(observed) => cur = observed,
            }
        }
    }
}

// ------------------------------------------------------------------
// LRU sentinel list (element `head` is the embedded sentinel)
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_lru_insert_front(
    head: *mut BcacheEntry,
    e: *mut BcacheEntry,
) {
    if head.is_null() || e.is_null() {
        return;
    }
    // SAFETY: `head` is bc->lru_head, `e` a pool entry; caller holds bc->lock.
    unsafe {
        (*e).next = (*head).next;
        (*e).prev = head;
        (*(*head).next).prev = e;
        (*head).next = e;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_lru_remove(e: *mut BcacheEntry) {
    if e.is_null() {
        return;
    }
    // SAFETY: e is on the LRU list; its prev/next are live entries/sentinels.
    unsafe {
        (*(*e).prev).next = (*e).next;
        (*(*e).next).prev = (*e).prev;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pcache_lru_insert_front(
    head: *mut PcacheEntry,
    e: *mut PcacheEntry,
) {
    if head.is_null() || e.is_null() {
        return;
    }
    // SAFETY: `head` is bc->page_lru_head; caller holds bc->lock.
    unsafe {
        (*e).next = (*head).next;
        (*e).prev = head;
        (*(*head).next).prev = e;
        (*head).next = e;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pcache_lru_remove(e: *mut PcacheEntry) {
    if e.is_null() {
        return;
    }
    // SAFETY: e is on the page LRU list.
    unsafe {
        (*(*e).prev).next = (*e).next;
        (*(*e).next).prev = (*e).prev;
    }
}

// ------------------------------------------------------------------
// hash membership (caller holds the entry's bucket lock)
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_hash_insert_locked(
    hash: *mut *mut BcacheEntry,
    e: *mut BcacheEntry,
) {
    if hash.is_null() || e.is_null() {
        return;
    }
    // SAFETY: hash is bc->hash, indexed by the entry's own key.
    unsafe {
        let idx = a20_bcache_hash_key((*e).lba) as usize;
        (*e).hnext = *hash.add(idx);
        *hash.add(idx) = e;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_hash_remove_locked(
    hash: *mut *mut BcacheEntry,
    e: *mut BcacheEntry,
) {
    if hash.is_null() || e.is_null() {
        return;
    }
    // SAFETY: walks the bucket chain mutating the predecessor's link.
    unsafe {
        let idx = a20_bcache_hash_key((*e).lba) as usize;
        let mut pp = hash.add(idx);
        while !(*pp).is_null() {
            if *pp == e {
                *pp = (*e).hnext;
                (*e).hnext = core::ptr::null_mut();
                return;
            }
            pp = core::ptr::addr_of_mut!((**pp).hnext);
        }
        (*e).hnext = core::ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_find_locked(
    hash: *mut *mut BcacheEntry,
    lba: u64,
) -> *mut BcacheEntry {
    if hash.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: reads the bucket chain; entries are owned by the pool.
    unsafe {
        let idx = a20_bcache_hash_key(lba) as usize;
        let mut e = *hash.add(idx);
        while !e.is_null() {
            if (*e).valid != 0 && (*e).lba == lba {
                return e;
            }
            e = (*e).hnext;
        }
        core::ptr::null_mut()
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pcache_hash_insert_locked(
    hash: *mut *mut PcacheEntry,
    e: *mut PcacheEntry,
) {
    if hash.is_null() || e.is_null() {
        return;
    }
    // SAFETY: hash is bc->page_hash.
    unsafe {
        let idx = a20_pcache_hash_key((*e).page_no) as usize;
        (*e).hnext = *hash.add(idx);
        *hash.add(idx) = e;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pcache_hash_remove_locked(
    hash: *mut *mut PcacheEntry,
    e: *mut PcacheEntry,
) {
    if hash.is_null() || e.is_null() {
        return;
    }
    // SAFETY: walks the bucket chain mutating the predecessor's link.
    unsafe {
        let idx = a20_pcache_hash_key((*e).page_no) as usize;
        let mut pp = hash.add(idx);
        while !(*pp).is_null() {
            if *pp == e {
                *pp = (*e).hnext;
                (*e).hnext = core::ptr::null_mut();
                return;
            }
            pp = core::ptr::addr_of_mut!((**pp).hnext);
        }
        (*e).hnext = core::ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pcache_find_locked(
    hash: *mut *mut PcacheEntry,
    page_no: u64,
) -> *mut PcacheEntry {
    if hash.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: reads the bucket chain.
    unsafe {
        let idx = a20_pcache_hash_key(page_no) as usize;
        let mut e = *hash.add(idx);
        while !e.is_null() {
            if (*e).valid != 0 && (*e).page_no == page_no {
                return e;
            }
            e = (*e).hnext;
        }
        core::ptr::null_mut()
    }
}

// ------------------------------------------------------------------
// dirty accounting
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_set_block_dirty(
    dirty_blocks: *mut usize,
    e: *mut BcacheEntry,
    dirty: c_int,
) {
    if dirty_blocks.is_null() || e.is_null() {
        return;
    }
    // SAFETY: caller holds bc->lock; counter and entry belong to the cache.
    unsafe {
        if dirty != 0 {
            (*e).dirty_gen = (*e).dirty_gen.wrapping_add(1);
            if (*e).dirty == 0 {
                *dirty_blocks = (*dirty_blocks).wrapping_add(1);
            }
            (*e).dirty = 1;
        } else if (*e).dirty != 0 {
            (*e).dirty = 0;
            if *dirty_blocks > 0 {
                *dirty_blocks -= 1;
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_bcache_set_page_dirty(
    dirty_pages: *mut PcacheDirtySet,
    e: *mut PcacheEntry,
    dirty: c_int,
) {
    if dirty_pages.is_null() || e.is_null() {
        return;
    }

    // Validate ownership before changing the entry. Doing this with integer
    // addresses avoids `offset_from`'s same-allocation precondition for an
    // invalid caller pointer; production callers pass a slot in this pool.
    unsafe {
        let set = &mut *dirty_pages;
        let index = match pcache_dirty_slot(set, e) {
            Some(index) => index,
            None => return,
        };
        let word_index = index / 64;
        let word_count = set.pool_count / 64 + (set.pool_count % 64 != 0) as usize;
        if word_index >= word_count {
            return;
        }
        let mask = 1u64 << (index % 64);
        let bit = &mut *set.words.add(word_index);

        // SAFETY: the caller holds bc->lock, protecting entry, count, and bit.
        if dirty != 0 {
            (*e).dirty_gen = (*e).dirty_gen.wrapping_add(1);
            if (*e).dirty == 0 {
                set.count = set.count.wrapping_add(1);
            }
            (*e).dirty = 1;
            *bit |= mask;
        } else if (*e).dirty != 0 {
            (*e).dirty = 0;
            if set.count > 0 {
                set.count -= 1;
            }
            *bit &= !mask;
        } else {
            // Repair an unexpected stale bit without changing the dirty count.
            *bit &= !mask;
        }
    }
}

// ------------------------------------------------------------------
// journal hold set
// ------------------------------------------------------------------

/// Linear ascending scan for a resident page, matching `bcache_hold_page`.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_hold_page(
    page_pool: *mut PcacheEntry,
    pool_size: usize,
    page_no: u64,
) -> c_int {
    if page_pool.is_null() {
        return 0;
    }
    // SAFETY: pool_size entries are live.
    unsafe {
        for i in 0..pool_size {
            let e = page_pool.add(i);
            if (*e).valid != 0 && (*e).page_no == page_no {
                (*e).held = 1;
                return 1;
            }
        }
    }
    0
}

/// Clear `held` on every slot, including invalid ones.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_release_holds(
    page_pool: *mut PcacheEntry,
    pool_size: usize,
) {
    if page_pool.is_null() {
        return;
    }
    // SAFETY: pool_size entries are live.
    unsafe {
        for i in 0..pool_size {
            (*page_pool.add(i)).held = 0;
        }
    }
}

/// Count resident held pages, matching `bcache_held_pages`.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_held_pages(
    page_pool: *const PcacheEntry,
    pool_size: usize,
) -> usize {
    if page_pool.is_null() {
        return 0;
    }
    let mut n = 0usize;
    // SAFETY: read-only scan of a live pool.
    unsafe {
        for i in 0..pool_size {
            let e = page_pool.add(i);
            if (*e).valid != 0 && (*e).held != 0 {
                n += 1;
            }
        }
    }
    n
}

// ------------------------------------------------------------------
// write quarantine
// ------------------------------------------------------------------

/// `bc->write_quarantine_until && now < until`.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_write_quarantined(
    quarantine_until: *const u64,
    now: u64,
) -> c_int {
    if quarantine_until.is_null() {
        return 0;
    }
    // SAFETY: lockless read of the caller's field, as in C.
    unsafe { if *quarantine_until != 0 && now < *quarantine_until { 1 } else { 0 } }
}

/// `until = now + ticks`.  Returns the stored deadline.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_note_write_error(
    quarantine_until: *mut u64,
    now: u64,
    ticks: u64,
) -> u64 {
    if quarantine_until.is_null() {
        return 0;
    }
    // SAFETY: lockless store, as in C.
    unsafe {
        let v = now.wrapping_add(ticks);
        *quarantine_until = v;
        v
    }
}

// ------------------------------------------------------------------
// sync selection
// ------------------------------------------------------------------

/// Binary search over a sorted ascending page-number array.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_sync_page_selected(
    page_nos: *const u64,
    count: usize,
    page_no: u64,
) -> c_int {
    if page_nos.is_null() {
        return 1;
    }
    let mut lo = 0usize;
    let mut hi = count;
    // SAFETY: count entries are readable.
    unsafe {
        while lo < hi {
            let mid = lo + (hi - lo) / 2;
            let v = *page_nos.add(mid);
            if v == page_no {
                return 1;
            } else if v < page_no {
                lo = mid + 1;
            } else {
                hi = mid;
            }
        }
    }
    0
}

/// Per-slot sync predicate: `bcache_sync_common` selection at lines 683-687.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_sync_select(
    mode: c_int,
    valid: c_int,
    dirty: c_int,
    held: c_int,
    page_nos: *const u64,
    page_count: usize,
    page_no: u64,
) -> c_int {
    if valid == 0 || dirty == 0 {
        return 0;
    }
    let want_held = mode == SYNC_HELD;
    if want_held != (held != 0) {
        return 0;
    }
    if mode == SYNC_SCOPED
        && a20_bcache_sync_page_selected(page_nos, page_count, page_no) == 0
    {
        return 0;
    }
    1
}

// ------------------------------------------------------------------
// eviction policy
// ------------------------------------------------------------------

/// Decide what to do with one LRU candidate, mirroring `bcache_evict` /
/// `pcache_evict_locked`.  `check_held` is true for the 4 KiB page pool.
/// The caller has already established `ref == 0` and re-checked under the
/// bucket lock; this owns the second-chance / quarantine / hold policy.
#[no_mangle]
pub extern "C" fn a20_bcache_evict_decide(
    quarantined: bool,
    valid: bool,
    dirty: bool,
    accessed: bool,
    held: bool,
    check_held: bool,
) -> c_int {
    if !valid {
        // Invalid/free descriptor: take immediately, no second chance.
        return EVICT_TAKE;
    }
    if check_held && held {
        return EVICT_SKIP;
    }
    if quarantined && dirty {
        return EVICT_SKIP;
    }
    if accessed {
        return EVICT_SECOND_CHANCE;
    }
    EVICT_TAKE
}

/// Scan and claim an LRU block entry while the caller holds `bc->lock`.
/// Rust owns candidate progression, second-chance mutation, ref recheck,
/// hash/LRU detach and claim publication. The C callbacks only acquire/release
/// the per-LBA bucket spinlock.
///
/// # Safety
/// `head`, `tail`, and `hash` are live members of one cache; the caller holds
/// its global cache lock for the entire call.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_evict_transaction(
    head: *mut BcacheEntry,
    tail: *mut BcacheEntry,
    hash: *mut *mut BcacheEntry,
    context: *mut core::ffi::c_void,
    quarantined: c_int,
    lock_bucket: Option<BcacheBucketLockFn>,
    unlock_bucket: Option<BcacheBucketUnlockFn>,
) -> *mut BcacheEntry {
    if head.is_null() || tail.is_null() || hash.is_null() || context.is_null() {
        return core::ptr::null_mut();
    }
    let (lockb, unlockb) = match (lock_bucket, unlock_bucket) {
        (Some(l), Some(u)) => (l, u),
        _ => return core::ptr::null_mut(),
    };
    // SAFETY: caller serializes the list with bc->lock and the callbacks
    // protect bucket membership during each valid-entry recheck.
    unsafe {
        let mut e = (*tail).prev;
        while !e.is_null() && e != head {
            let prev = (*e).prev;
            if a20_bcache_ref_read(core::ptr::addr_of!((*e).ref_)) == 0 {
                if quarantined != 0 && (*e).valid != 0 && (*e).dirty != 0 {
                    e = prev;
                    continue;
                }
                if (*e).valid != 0 {
                    let flags = lockb(context, (*e).lba);
                    if a20_bcache_ref_read(core::ptr::addr_of!((*e).ref_)) == 0
                        && (*e).valid != 0
                    {
                        let decision = a20_bcache_evict_decide(
                            quarantined != 0, true, (*e).dirty != 0,
                            (*e).accessed != 0, false, false,
                        );
                        if decision == EVICT_SECOND_CHANCE {
                            (*e).accessed = 0;
                            unlockb(context, (*e).lba, flags);
                            e = prev;
                            continue;
                        }
                        if decision == EVICT_TAKE {
                            a20_bcache_lru_remove(e);
                            a20_bcache_hash_remove_locked(hash, e);
                            (*e).ref_ = 1;
                            unlockb(context, (*e).lba, flags);
                            return e;
                        }
                    }
                    unlockb(context, (*e).lba, flags);
                } else {
                    a20_bcache_lru_remove(e);
                    (*e).ref_ = 1;
                    return e;
                }
            }
            e = prev;
        }
    }
    core::ptr::null_mut()
}

/// Page-pool second-chance eviction, called with the cache lock held.
#[no_mangle]
pub unsafe extern "C" fn a20_pcache_evict_transaction(
    head: *mut PcacheEntry, tail: *mut PcacheEntry,
    hash: *mut *mut PcacheEntry, context: *mut core::ffi::c_void,
    quarantined: c_int, lock_bucket: Option<BcacheBucketLockFn>,
    unlock_bucket: Option<BcacheBucketUnlockFn>,
) -> *mut PcacheEntry {
    let (lockb, unlockb) = match (lock_bucket, unlock_bucket) {
        (Some(l), Some(u)) => (l, u), _ => return core::ptr::null_mut(),
    };
    if head.is_null() || tail.is_null() || hash.is_null() || context.is_null() {
        return core::ptr::null_mut();
    }
    unsafe {
        for _ in 0..4 {
            let mut cleared = false;
            let mut e = (*tail).prev;
            while !e.is_null() && e != head {
                let prev = (*e).prev;
                if a20_bcache_ref_read(core::ptr::addr_of!((*e).ref_)) == 0 {
                    if quarantined != 0 && (*e).valid != 0 && (*e).dirty != 0 {
                        e = prev;
                        continue;
                    }
                    if (*e).valid != 0 {
                        let flags = lockb(context, (*e).page_no);
                        if a20_bcache_ref_read(core::ptr::addr_of!((*e).ref_)) == 0
                            && (*e).valid != 0
                        {
                            let take = a20_bcache_evict_decide(
                                quarantined != 0, true, (*e).dirty != 0,
                                (*e).accessed != 0, (*e).held != 0, true,
                            );
                            if take == EVICT_SECOND_CHANCE {
                                (*e).accessed = 0;
                                cleared = true;
                                unlockb(context, (*e).page_no, flags);
                                e = prev;
                                continue;
                            }
                            if take == EVICT_TAKE {
                                a20_pcache_hash_remove_locked(hash, e);
                                a20_pcache_lru_remove(e);
                                (*e).ref_ = 1;
                                unlockb(context, (*e).page_no, flags);
                                return e;
                            }
                        }
                        unlockb(context, (*e).page_no, flags);
                    } else {
                        a20_pcache_lru_remove(e);
                        (*e).ref_ = 1;
                        return e;
                    }
                }
                e = prev;
            }
            if !cleared { break; }
        }
    }
    core::ptr::null_mut()
}

/// Full page-cache lookup/fill/publication transaction. Locks, allocator, and
/// device IO are supplied as primitive callbacks; Rust owns the hit/miss
/// ordering, victim handoff, error restoration, duplicate suppression and
/// publication state transitions.
#[no_mangle]
pub unsafe extern "C" fn a20_pcache_get_transaction(
    context: *mut core::ffi::c_void,
    pool: *mut PcacheEntry,
    pool_count: usize,
    hash: *mut *mut PcacheEntry,
    lru_head: *mut PcacheEntry,
    lru_tail: *mut PcacheEntry,
    dirty_pages: *mut PcacheDirtySet,
    page_no: u64,
    full_overwrite: *const u8,
    quarantined: c_int,
    lock_bucket: Option<BcacheBucketLockFn>,
    unlock_bucket: Option<BcacheBucketUnlockFn>,
    lock_global: Option<BcacheLockFn>,
    unlock_global: Option<BcacheUnlockFn>,
    fill_lock: Option<BcachePageActionFn>,
    fill_unlock: Option<BcachePageActionFn>,
    writeback_lock: Option<BcachePageActionFn>,
    writeback_unlock: Option<BcachePageActionFn>,
    flush_victim: Option<BcachePageFlushFn>,
    read_page: Option<BcachePageReadFn>,
    alloc_page: Option<PcachePageAllocFn>,
    perf: Option<BcachePagePerfFn>,
    diagnostic: Option<BcachePageDiagnosticFn>,
    note_error: Option<BcachePageActionFn>,
) -> *mut PcacheEntry {
    let (
        lockb,
        unlockb,
        lockg,
        unlockg,
        fill,
        unfill,
        writeback,
        unwback,
        flush,
        read,
    ) = match (
        lock_bucket,
        unlock_bucket,
        lock_global,
        unlock_global,
        fill_lock,
        fill_unlock,
        writeback_lock,
        writeback_unlock,
        flush_victim,
        read_page,
    ) {
        (
            Some(a), Some(b), Some(c), Some(d), Some(e),
            Some(f), Some(g), Some(h), Some(i), Some(j),
        ) => (a, b, c, d, e, f, g, h, i, j),
        _ => return core::ptr::null_mut(),
    };
    if context.is_null()
        || pool.is_null()
        || pool_count == 0
        || hash.is_null()
        || lru_head.is_null()
        || lru_tail.is_null()
        || dirty_pages.is_null()
    {
        return core::ptr::null_mut();
    }

    unsafe {
        let flags = lockb(context, page_no);
        let mut e = a20_pcache_find_locked(hash, page_no);
        if !e.is_null() {
            a20_bcache_ref_get(core::ptr::addr_of_mut!((*e).ref_));
            (*e).accessed = 1;
            unlockb(context, page_no, flags);
            return e;
        }
        unlockb(context, page_no, flags);

        let flags = lockg(context);
        e = a20_pcache_find_locked(hash, page_no);
        if !e.is_null() {
            a20_bcache_ref_get(core::ptr::addr_of_mut!((*e).ref_));
            a20_pcache_lru_remove(e);
            a20_pcache_lru_insert_front(lru_head, e);
            unlockg(context, flags);
            return e;
        }
        unlockg(context, flags);

        if let Some(f) = perf {
            f(context, 0);
        }
        fill(context, page_no);
        let flags = lockg(context);
        e = a20_pcache_find_locked(hash, page_no);
        if !e.is_null() {
            a20_bcache_ref_get(core::ptr::addr_of_mut!((*e).ref_));
            a20_pcache_lru_remove(e);
            a20_pcache_lru_insert_front(lru_head, e);
            unlockg(context, flags);
            unfill(context, page_no);
            return e;
        }
        unlockg(context, flags);

        writeback(context, page_no);
        let flags = lockg(context);
        e = a20_pcache_find_locked(hash, page_no);
        if !e.is_null() {
            a20_bcache_ref_get(core::ptr::addr_of_mut!((*e).ref_));
            a20_pcache_lru_remove(e);
            a20_pcache_lru_insert_front(lru_head, e);
            unlockg(context, flags);
            unwback(context, page_no);
            unfill(context, page_no);
            return e;
        }
        e = a20_pcache_evict_transaction(
            lru_head,
            lru_tail,
            hash,
            context,
            quarantined,
            Some(lockb),
            Some(unlockb),
        );
        if e.is_null() {
            unlockg(context, flags);
            if let Some(f) = diagnostic {
                f(context, page_no);
            }
            unwback(context, page_no);
            unfill(context, page_no);
            return core::ptr::null_mut();
        }
        let old_page = (*e).page_no;
        let old_dirty = (*e).dirty != 0 && (*e).valid != 0;
        unlockg(context, flags);

        if old_dirty && flush(context, e) < 0 {
            if let Some(f) = note_error {
                f(context, page_no);
            }
            let flags = lockg(context);
            (*e).page_no = old_page;
            (*e).valid = 1;
            (*e).ref_ = 0;
            a20_bcache_set_page_dirty(dirty_pages, e, 1);
            a20_pcache_hash_insert_locked(hash, e);
            a20_pcache_lru_insert_front(lru_head, e);
            unlockg(context, flags);
            unwback(context, page_no);
            unfill(context, page_no);
            return core::ptr::null_mut();
        }
        let flags = lockg(context);
        (*e).valid = 0;
        a20_bcache_set_page_dirty(dirty_pages, e, 0);
        unlockg(context, flags);
        unwback(context, page_no);

        // The victim is exclusively owned by this fill. Cache and writeback
        // locks are released for allocation, while the fill lock still prevents
        // duplicate same-page fills. The allocator cannot reclaim, so this does
        // not re-enter cache code while that serialization lock is held.
        if (*e).data.is_null() {
            let payload = match alloc_page {
                Some(allocate) => allocate(context),
                None => core::ptr::null_mut(),
            };
            if payload.is_null() {
                let flags = lockg(context);
                (*e).valid = 0;
                (*e).ref_ = 0;
                (*e).page_no = u64::MAX;
                a20_pcache_lru_insert_front(lru_head, e);
                unlockg(context, flags);
                unfill(context, page_no);
                return core::ptr::null_mut();
            }
            let flags = lockg(context);
            (*e).data = payload;
            unlockg(context, flags);
        }

        if !full_overwrite.is_null() {
            core::ptr::copy_nonoverlapping(full_overwrite, (*e).data, 4096);
            if let Some(f) = perf {
                f(context, 3);
            }
        } else if read(context, page_no, (*e).data) < 0 {
            let flags = lockg(context);
            (*e).ref_ = 0;
            (*e).page_no = u64::MAX;
            a20_pcache_lru_insert_front(lru_head, e);
            unlockg(context, flags);
            unfill(context, page_no);
            return core::ptr::null_mut();
        }

        let flags = lockg(context);
        let dup = a20_pcache_find_locked(hash, page_no);
        if !dup.is_null() {
            (*e).ref_ = 0;
            (*e).page_no = u64::MAX;
            (*e).valid = 0;
            a20_pcache_lru_insert_front(lru_head, e);
            a20_bcache_ref_get(core::ptr::addr_of_mut!((*dup).ref_));
            a20_pcache_lru_remove(dup);
            a20_pcache_lru_insert_front(lru_head, dup);
            unlockg(context, flags);
            unfill(context, page_no);
            return dup;
        }
        (*e).page_no = page_no;
        a20_bcache_set_page_dirty(dirty_pages, e, 0);
        (*e).dirty_gen = 0;
        (*e).valid = 1;
        a20_pcache_hash_insert_locked(hash, e);
        a20_pcache_lru_insert_front(lru_head, e);
        unlockg(context, flags);
        unfill(context, page_no);
        e
    }
}

/// Release slot-owned payloads when the cache is exclusively being destroyed.
/// Each pointer is cleared before calling C so repeated cleanup cannot double-free.
#[no_mangle]
pub unsafe extern "C" fn a20_pcache_release_storage(
    pool: *mut PcacheEntry,
    pool_count: usize,
    free_page: Option<PcachePageFreeFn>,
) {
    let free_page = match free_page {
        Some(free_page) => free_page,
        None => return,
    };
    if pool.is_null() {
        return;
    }

    unsafe {
        for i in 0..pool_count {
            let entry = &mut *pool.add(i);
            let data = entry.data;
            entry.data = core::ptr::null_mut();
            if !data.is_null() {
                free_page(data);
            }
        }
    }
}

/// Full 512-byte block-cache miss/fill/eviction transaction.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_get_transaction(
    context: *mut core::ffi::c_void, pool: *mut BcacheEntry, pool_count: usize,
    hash: *mut *mut BcacheEntry, lru_head: *mut BcacheEntry,
    lru_tail: *mut BcacheEntry, dirty_blocks: *mut usize, lba: u64,
    quarantined: c_int, lock_bucket: Option<BcacheBucketLockFn>,
    unlock_bucket: Option<BcacheBucketUnlockFn>, lock_global: Option<BcacheLockFn>,
    unlock_global: Option<BcacheUnlockFn>, writeback_lock: Option<BcachePageActionFn>,
    writeback_unlock: Option<BcachePageActionFn>, io: Option<BcacheBlockIoFn>,
    note_error: Option<BcachePageActionFn>,
) -> *mut BcacheEntry {
    let (lockb, unlockb, lockg, unlockg, wb, unwb, io, note) =
        match (lock_bucket, unlock_bucket, lock_global, unlock_global,
               writeback_lock, writeback_unlock, io, note_error) {
            (Some(a),Some(b),Some(c),Some(d),Some(e),Some(f),Some(g),Some(h)) => (a,b,c,d,e,f,g,h),
            _ => return core::ptr::null_mut(),
        };
    if context.is_null() || pool.is_null() || pool_count == 0 || hash.is_null()
        || lru_head.is_null() || lru_tail.is_null() || dirty_blocks.is_null() {
        return core::ptr::null_mut();
    }
    unsafe {
        let flags=lockb(context,lba);
        let mut e=a20_bcache_find_locked(hash,lba);
        if !e.is_null(){a20_bcache_ref_get(core::ptr::addr_of_mut!((*e).ref_));(*e).accessed=1;unlockb(context,lba,flags);return e;}
        unlockb(context,lba,flags);
        let flags=lockg(context);
        e=a20_bcache_find_locked(hash,lba);
        if !e.is_null(){a20_bcache_ref_get(core::ptr::addr_of_mut!((*e).ref_));a20_bcache_lru_remove(e);a20_bcache_lru_insert_front(lru_head,e);unlockg(context,flags);return e;}
        unlockg(context,flags);
        wb(context,lba);
        let flags=lockg(context);
        e=a20_bcache_find_locked(hash,lba);
        if !e.is_null(){a20_bcache_ref_get(core::ptr::addr_of_mut!((*e).ref_));a20_bcache_lru_remove(e);a20_bcache_lru_insert_front(lru_head,e);unlockg(context,flags);unwb(context,lba);return e;}
        e=a20_bcache_evict_transaction(lru_head,lru_tail,hash,context,quarantined,Some(lockb),Some(unlockb));
        if e.is_null(){unlockg(context,flags);unwb(context,lba);return core::ptr::null_mut();}
        let old_lba=(*e).lba; let old_dirty=(*e).dirty!=0&&(*e).valid!=0;
        (*e).valid=0; unlockg(context,flags);
        if old_dirty && io(context,old_lba,(*e).data.as_mut_ptr(),1)<0 {
            note(context,lba); let flags=lockg(context);
            (*e).ref_=0; a20_bcache_set_block_dirty(dirty_blocks,e,1); (*e).valid=1;
            a20_bcache_hash_insert_locked(hash,e);a20_bcache_lru_insert_front(lru_head,e);
            unlockg(context,flags);unwb(context,lba);return core::ptr::null_mut();
        }
        unwb(context,lba);
        if io(context,lba,(*e).data.as_mut_ptr(),0)<0 {
            let flags=lockg(context);(*e).ref_=0;(*e).lba=u64::MAX;
            a20_bcache_lru_insert_front(lru_head,e);unlockg(context,flags);return core::ptr::null_mut();
        }
        let flags=lockg(context);
        let dup=a20_bcache_find_locked(hash,lba);
        if !dup.is_null(){(*e).ref_=0;(*e).lba=u64::MAX;a20_bcache_lru_insert_front(lru_head,e);
            a20_bcache_ref_get(core::ptr::addr_of_mut!((*dup).ref_));a20_bcache_lru_remove(dup);
            a20_bcache_lru_insert_front(lru_head,dup);unlockg(context,flags);return dup;}
        (*e).lba=lba;a20_bcache_set_block_dirty(dirty_blocks,e,0);(*e).dirty_gen=0;(*e).valid=1;
        a20_bcache_hash_insert_locked(hash,e);a20_bcache_lru_insert_front(lru_head,e);
        unlockg(context,flags);e
    }
}

/// Own contiguous dirty-page victim writeback. The adapter supplies page
/// lookup, lock, and device-write primitives; Rust decides the batch boundary,
/// snapshots generations, and confirms only the versions actually written.
#[no_mangle]
pub unsafe extern "C" fn a20_pcache_flush_transaction(
    context: *mut core::ffi::c_void, first: *mut PcacheEntry,
    writeback_buffer: *mut u8, dirty_pages: *mut PcacheDirtySet,
    max_pages: usize,
    find: Option<PcachePageFindFn>, lock: Option<BcacheLockFn>,
    unlock: Option<BcacheUnlockFn>, write: Option<PcacheWriteBatchFn>,
) -> c_int {
    let (find, lock, unlock, write) = match (find, lock, unlock, write) {
        (Some(a),Some(b),Some(c),Some(d)) => (a,b,c,d), _ => return -22,
    };
    if context.is_null() || first.is_null() || writeback_buffer.is_null() || dirty_pages.is_null() || max_pages == 0 {
        return -22;
    }
    unsafe {
        let flags=lock(context);
        if (*first).valid==0 || (*first).dirty==0 { unlock(context,flags); return 0; }
        let first_no=(*first).page_no;
        let mut count=0usize;
        let mut gens=[0u64;256];
        let limit=core::cmp::min(max_pages,gens.len());
        while count<limit {
            let no=first_no.wrapping_add(count as u64);
            let entry=if count==0 {first} else {find(context,no)};
            if entry.is_null() || (*entry).valid==0 || (*entry).dirty==0 { break; }
            gens[count]=(*entry).dirty_gen;
            core::ptr::copy_nonoverlapping((*entry).data,writeback_buffer.add(count*4096),4096);
            count+=1;
        }
        unlock(context,flags);
        if count==0 { return 0; }
        let lba=first_no.wrapping_mul(4096)/512;
        let result=write(context,lba,writeback_buffer,count*8);
        if result<0 { return result; }
        let flags=lock(context);
        for i in 0..count {
            let no=first_no.wrapping_add(i as u64);
            let entry=find(context,no);
            if !entry.is_null() && (*entry).valid!=0 && (*entry).page_no==no && (*entry).dirty_gen==gens[i] {
                a20_bcache_set_page_dirty(dirty_pages, entry, 0);
            }
        }
        unlock(context,flags);
        result
    }
}

/// Own eligibility, device-read/overlay ordering, and fallback selection for
/// the page-aligned multi-page fast path. Device access and accounting remain
/// narrow callbacks; dirty cached versions always overlay disk bytes while the
/// writeback read lock prevents a concurrent flush from invalidating them.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_read_batch_transaction(
    context: *mut core::ffi::c_void, byte_offset: u64, buffer: *mut u8, len: usize,
    page_size: u64, block_size: u64, max_pages: usize,
    hash: *mut *mut PcacheEntry, lru_head: *mut PcacheEntry,
    lock: Option<BcacheLockFn>, unlock: Option<BcacheUnlockFn>,
    writeback_read_lock: Option<BcachePageActionFn>,
    writeback_read_unlock: Option<BcachePageActionFn>,
    find: Option<PcachePageFindFn>, read: Option<BcacheBatchReadFn>,
    fallback: Option<BcacheRangeReadFn>, perf: Option<BcacheBatchPerfFn>,
) -> c_int {
    let (lock,unlock,wb_lock,wb_unlock,find,read,fallback) = match
        (lock,unlock,writeback_read_lock,writeback_read_unlock,find,read,fallback) {
        (Some(a),Some(b),Some(c),Some(d),Some(e),Some(f),Some(g)) => (a,b,c,d,e,f,g),
        _ => return -22,
    };
    if context.is_null() || hash.is_null() || lru_head.is_null() || (len>0 && buffer.is_null()) { return -22; }
    if len==0 { return 0; }
    let mut first=0u64; let mut pages=0usize; let mut lba=0u64; let mut sectors=0usize;
    if a20_bcache_read_batch_plan(byte_offset,len,page_size,block_size,max_pages,
        &mut first,&mut pages,&mut lba,&mut sectors)==0 {
        return fallback(context,byte_offset,buffer,len);
    }
    unsafe {
        if let Some(f)=perf { f(context,pages); }
        wb_lock(context,0);
        let result=read(context,lba,buffer,sectors);
        if result>=0 {
            let flags=lock(context);
            for i in 0..pages {
                let entry=find(context,first.wrapping_add(i as u64));
                if !entry.is_null() {
                    core::ptr::copy_nonoverlapping((*entry).data,buffer.add(i*page_size as usize),page_size as usize);
                    a20_pcache_lru_remove(entry);
                    a20_pcache_lru_insert_front(lru_head,entry);
                }
            }
            unlock(context,flags);
        }
        wb_unlock(context,0);
        if result<0 { result } else { 0 }
    }
}

/// Batch-read eligibility and geometry.  A multi-page aligned read may be
/// served by one device command when it is page-aligned, spans at least two
/// pages and stays within the caller's batch bound.  Returns 1 with the
/// geometry filled, or 0 when the caller must fall back to the per-page path.
///
/// # Safety
/// The out pointers are writable.
#[no_mangle]
pub unsafe extern "C" fn a20_bcache_read_batch_plan(
    byte_off: u64,
    len: usize,
    page_size: u64,
    block_size: u64,
    max_pages: usize,
    out_first_page: *mut u64,
    out_page_count: *mut usize,
    out_lba: *mut u64,
    out_sectors: *mut usize,
) -> c_int {
    if out_first_page.is_null() || out_page_count.is_null() || out_lba.is_null()
        || out_sectors.is_null() || page_size == 0 || block_size == 0
    {
        return 0;
    }
    let ps = page_size;
    if byte_off % ps != 0 || (len as u64) % ps != 0 {
        return 0;
    }
    let page_count = len / page_size as usize;
    if page_count < 2 || page_count > max_pages {
        return 0;
    }
    // SAFETY: out pointers are writable by contract.
    unsafe {
        *out_first_page = byte_off / ps;
        *out_page_count = page_count;
        *out_lba = byte_off / block_size;
        *out_sectors = len / block_size as usize;
    }
    1
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mk_bcache() -> BcacheEntry {
        BcacheEntry {
            lba: 0,
            dirty: 0,
            dirty_gen: 0,
            ref_: 0,
            valid: 0,
            accessed: 0,
            data: [0u8; 512],
            prev: core::ptr::null_mut(),
            next: core::ptr::null_mut(),
            hnext: core::ptr::null_mut(),
        }
    }

    fn mk_pcache() -> PcacheEntry {
        PcacheEntry { page_no: 0, dirty: 0, dirty_gen: 0, ref_: 0, valid: 0,
            accessed: 0, held: 0, data: core::ptr::null_mut(), prev: core::ptr::null_mut(),
            next: core::ptr::null_mut(), hnext: core::ptr::null_mut() }
    }

    #[test]
    #[cfg(target_pointer_width = "64")]
    fn layout_matches_c() {
        assert_eq!(core::mem::size_of::<BcacheEntry>(), 576);
        assert_eq!(core::mem::size_of::<PcacheEntry>(), 72);
        assert_eq!(core::mem::offset_of!(BcacheEntry, data), 33);
        assert_eq!(core::mem::offset_of!(PcacheEntry, held), 36);
        assert_eq!(core::mem::offset_of!(PcacheEntry, data), 40);
    }

    #[test]
    fn hash_keys_respect_mask() {
        assert_eq!(a20_bcache_hash_key(0x2), 2);
        assert_eq!(a20_bcache_hash_key(BCACHE_HASH_BUCKETS as u64), 0);
        for lba in 0..(PCACHE_HASH_BUCKETS as u64 * 2) {
            assert!(a20_bcache_hash_key(lba) < BCACHE_HASH_BUCKETS as u32);
            assert!(a20_pcache_hash_key(lba) < PCACHE_HASH_BUCKETS as u32);
        }
        assert_eq!(a20_pcache_hash_key((PCACHE_HASH_BUCKETS - 1) as u64),
                   (PCACHE_HASH_BUCKETS - 1) as u32);
        assert_eq!(a20_pcache_hash_key(PCACHE_HASH_BUCKETS as u64), 0);
    }

    #[test]
    fn sync_page_selected_binary_search() {
        let nos = [3u64, 7, 11, 40];
        unsafe {
            assert_eq!(a20_bcache_sync_page_selected(nos.as_ptr(), 4, 3), 1);
            assert_eq!(a20_bcache_sync_page_selected(nos.as_ptr(), 4, 40), 1);
            assert_eq!(a20_bcache_sync_page_selected(nos.as_ptr(), 4, 5), 0);
            assert_eq!(a20_bcache_sync_page_selected(nos.as_ptr(), 4, 0), 0);
            assert_eq!(a20_bcache_sync_page_selected(core::ptr::null(), 0, 9), 1);
        }
    }

    #[test]
    fn sync_select_modes() {
        unsafe {
            let nos = [5u64];
            // ALL: held pages skipped.
            assert_eq!(a20_bcache_sync_select(SYNC_ALL, 1, 1, 0, core::ptr::null(), 0, 0), 1);
            assert_eq!(a20_bcache_sync_select(SYNC_ALL, 1, 1, 1, core::ptr::null(), 0, 0), 0);
            // SCOPED: selected and not held.
            assert_eq!(a20_bcache_sync_select(SYNC_SCOPED, 1, 1, 0, nos.as_ptr(), 1, 5), 1);
            assert_eq!(a20_bcache_sync_select(SYNC_SCOPED, 1, 1, 0, nos.as_ptr(), 1, 6), 0);
            assert_eq!(a20_bcache_sync_select(SYNC_SCOPED, 1, 1, 1, nos.as_ptr(), 1, 5), 0);
            // HELD: only held.
            assert_eq!(a20_bcache_sync_select(SYNC_HELD, 1, 1, 1, core::ptr::null(), 0, 0), 1);
            assert_eq!(a20_bcache_sync_select(SYNC_HELD, 1, 1, 0, core::ptr::null(), 0, 0), 0);
        }
    }

    #[test]
    fn evict_policy_second_chance_and_quarantine() {
        // invalid slot is always taken
        assert_eq!(a20_bcache_evict_decide(false, false, true, true, true, true), EVICT_TAKE);
        // held page is skipped in the page pool, taken in the block pool
        assert_eq!(a20_bcache_evict_decide(false, true, false, false, true, true), EVICT_SKIP);
        assert_eq!(a20_bcache_evict_decide(false, true, false, false, true, false), EVICT_TAKE);
        // quarantined dirty page skipped
        assert_eq!(a20_bcache_evict_decide(true, true, true, false, false, true), EVICT_SKIP);
        // second chance
        assert_eq!(a20_bcache_evict_decide(false, true, false, true, false, true), EVICT_SECOND_CHANCE);
    }

    extern "C" fn evict_lock(ctx: *mut core::ffi::c_void, _: u64) -> u64 {
        unsafe { *(ctx as *mut usize) += 1 };
        0
    }
    extern "C" fn evict_unlock(_: *mut core::ffi::c_void, _: u64, _: u64) {}

    #[test]
    fn eviction_transaction_clears_second_chance_then_detaches_hash() {
        unsafe {
            let mut head = mk_bcache();
            let mut tail = mk_bcache();
            let mut e = mk_bcache();
            let mut hash: [*mut BcacheEntry; BCACHE_HASH_BUCKETS] =
                [core::ptr::null_mut(); BCACHE_HASH_BUCKETS];
            head.next = &mut tail;
            tail.prev = &mut head;
            a20_bcache_lru_insert_front(&mut head, &mut e);
            // insert_front puts e before the previous first element.
            e.lba = 19;
            e.valid = 1;
            e.accessed = 1;
            a20_bcache_hash_insert_locked(hash.as_mut_ptr(), &mut e);
            let mut locks = 0usize;
            let context = &mut locks as *mut usize as *mut core::ffi::c_void;
            assert!(a20_bcache_evict_transaction(
                &mut head, &mut tail, hash.as_mut_ptr(), context, 0,
                Some(evict_lock), Some(evict_unlock),
            ).is_null());
            assert_eq!(e.accessed, 0);
            assert_eq!(a20_bcache_find_locked(hash.as_mut_ptr(), 19), &mut e as *mut _);
            let taken = a20_bcache_evict_transaction(
                &mut head, &mut tail, hash.as_mut_ptr(), context, 0,
                Some(evict_lock), Some(evict_unlock),
            );
            assert_eq!(taken, &mut e as *mut _);
            assert_eq!(e.ref_, 1);
            assert!(a20_bcache_find_locked(hash.as_mut_ptr(), 19).is_null());
            assert_eq!(locks, 2);
            assert_eq!(head.next, &mut tail as *mut _);
            assert_eq!(tail.prev, &mut head as *mut _);
        }
    }

    #[test]
    fn page_eviction_transaction_respects_second_chance_hold_and_hash() {
        extern "C" fn lock(ctx: *mut core::ffi::c_void, _: u64) -> u64 {
            unsafe { *(ctx as *mut usize) += 1 }; 0
        }
        extern "C" fn unlock(_: *mut core::ffi::c_void, _: u64, _: u64) {}
        unsafe {
            let mut head = mk_pcache();
            let mut tail = mk_pcache();
            let mut e = mk_pcache();
            let mut held = mk_pcache();
            let mut hash: [*mut PcacheEntry; PCACHE_HASH_BUCKETS] =
                [core::ptr::null_mut(); PCACHE_HASH_BUCKETS];
            head.next = &mut tail; tail.prev = &mut head;
            a20_pcache_lru_insert_front(&mut head, &mut e);
            e.page_no = 19; e.valid = 1; e.accessed = 1;
            a20_pcache_hash_insert_locked(hash.as_mut_ptr(), &mut e);
            let mut locks = 0usize;
            let ctx = &mut locks as *mut usize as *mut core::ffi::c_void;
            assert!(a20_pcache_evict_transaction(&mut head, &mut tail,
                hash.as_mut_ptr(), ctx, 0, Some(lock), Some(unlock)) == &mut e as *mut _);
            assert_eq!(e.accessed, 0);
            assert_eq!(e.ref_, 1);
            assert!(a20_pcache_find_locked(hash.as_mut_ptr(), 19).is_null());
            assert_eq!(locks, 2);

            // Held dirty pages remain resident even after their second chance.
            a20_pcache_lru_insert_front(&mut head, &mut held);
            held.page_no = 23; held.valid = 1; held.dirty = 1; held.held = 1;
            a20_pcache_hash_insert_locked(hash.as_mut_ptr(), &mut held);
            assert!(a20_pcache_evict_transaction(&mut head, &mut tail,
                hash.as_mut_ptr(), ctx, 0, Some(lock), Some(unlock)).is_null());
            assert_eq!(held.ref_, 0);
            assert!(a20_pcache_find_locked(hash.as_mut_ptr(), 23) == &mut held as *mut _);
        }
    }

    #[test]
    fn hash_insert_find_remove_roundtrip() {
        unsafe {
            let mut pool = [mk_bcache(), mk_bcache()];
            let mut hash: [*mut BcacheEntry; BCACHE_HASH_BUCKETS] =
                [core::ptr::null_mut(); BCACHE_HASH_BUCKETS];
            // Two lbas in the same bucket (differ by BCACHE_HASH_BUCKETS).
            pool[0].lba = 1234;
            pool[0].valid = 1;
            pool[1].lba = 1234 + BCACHE_HASH_BUCKETS as u64;
            pool[1].valid = 1;
            let h = hash.as_mut_ptr();
            a20_bcache_hash_insert_locked(h, &mut pool[0]);
            a20_bcache_hash_insert_locked(h, &mut pool[1]);
            // Newest insert is the bucket head; find(1234) skips it and finds pool[0].
            assert_eq!(a20_bcache_find_locked(h, 1234), &mut pool[0] as *mut _);
            assert_eq!(
                a20_bcache_find_locked(h, 1234 + BCACHE_HASH_BUCKETS as u64),
                &mut pool[1] as *mut _
            );
            a20_bcache_hash_remove_locked(h, &mut pool[1]);
            assert_eq!(a20_bcache_find_locked(h, 1234), &mut pool[0] as *mut _);
            a20_bcache_hash_remove_locked(h, &mut pool[0]);
            assert!(a20_bcache_find_locked(h, 1234).is_null());
        }
    }

    #[test]
    fn dirty_accounting_transitions() {
        unsafe {
            let mut blocks = 0usize;
            let mut e = mk_bcache();
            a20_bcache_set_block_dirty(&mut blocks, &mut e, 1);
            assert_eq!(e.dirty, 1);
            assert_eq!(e.dirty_gen, 1);
            assert_eq!(blocks, 1);
            // redundant dirty: gen bumps, counter does not
            a20_bcache_set_block_dirty(&mut blocks, &mut e, 1);
            assert_eq!(e.dirty_gen, 2);
            assert_eq!(blocks, 1);
            a20_bcache_set_block_dirty(&mut blocks, &mut e, 0);
            assert_eq!(e.dirty, 0);
            assert_eq!(blocks, 0);
            // clean -> clean is a no-op
            a20_bcache_set_block_dirty(&mut blocks, &mut e, 0);
            assert_eq!(e.dirty_gen, 2);
            assert_eq!(blocks, 0);
        }
    }
    #[test]
    fn read_batch_plan_eligibility_and_geometry() {
        let (mut fp, mut pc, mut lba, mut sec) = (0u64, 0usize, 0u64, 0usize);
        unsafe {
            /* aligned two-page read: eligible */
            assert_eq!(
                a20_bcache_read_batch_plan(8192, 8192, 4096, 512, 8, &mut fp, &mut pc, &mut lba, &mut sec),
                1
            );
            assert_eq!((fp, pc, lba, sec), (2, 2, 16, 16));
            /* unaligned offset falls back */
            assert_eq!(a20_bcache_read_batch_plan(100, 8192, 4096, 512, 8, &mut fp, &mut pc, &mut lba, &mut sec), 0);
            /* unaligned length falls back */
            assert_eq!(a20_bcache_read_batch_plan(0, 5000, 4096, 512, 8, &mut fp, &mut pc, &mut lba, &mut sec), 0);
            /* a single page is not batched */
            assert_eq!(a20_bcache_read_batch_plan(0, 4096, 4096, 512, 8, &mut fp, &mut pc, &mut lba, &mut sec), 0);
            /* over the batch bound falls back */
            assert_eq!(a20_bcache_read_batch_plan(0, 40960, 4096, 512, 8, &mut fp, &mut pc, &mut lba, &mut sec), 0);
        }
    }

}
