//! Page-cache operation engine (Rust-owned split for `page_cache.c`).
//!
//! C supplies global/bucket/page locks, frame allocation and zeroing, vnode
//! references, and filesystem read/write callbacks. Rust owns page get and
//! publication, vfile read/write/write-through loops, the hash/LRU/free-list
//! and mapping transitions, writeback batching/confirmation, eviction, and
//! invalidate/truncate decisions. It performs bounded copies through the
//! C-compatible page descriptors and follows lock callbacks at transition
//! boundaries.
//!
//! * the hash membership (insert/remove/find with scan accounting),
//! * the global LRU (remove / insert-front / insert-tail),
//! * the free-descriptor stack (push / take-with-publish),
//! * the per-vnode mapping list,
//! * the per-vnode and global dirty lists, kept in descending index order,
//! * the page reference + per-bucket pin accounting,
//! * mapping publication and deferred detach,
//! * the bounded second-chance eviction sweep,
//! * the writeback batch collector and the dirty-generation confirmation that
//!   decides which pages a completed writeback may clear,
//! * the invalidate/range/discard and truncate detachment decisions.
//!
//! Callback contracts preserve the existing lock order; Rust transactions
//! acquire/release through those callbacks, while allocation and filesystem IO
//! remain supplied mechanisms.
//!
//! The `#[repr(C)]` mirrors reproduce the LP64/ILP32 layout of
//! `page_cache_page_t` up to (but not including) the trailing `fill_lock`
//! mutex, plus the leading `vnode` fields the state model reads.  The C adapter
//! asserts the offsets through `_Static_assert`, so a layout drift fails the
//! build instead of corrupting the cache.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};
use core::sync::atomic::{AtomicI32, AtomicUsize, Ordering};

/// Kernel page size (fixed at 4096 on every supported A20OS target).
const PAGE_SIZE: usize = 4096;

// riscv32/arm32 lack 64-bit atomics.  The page cache only touches dirty_gen /
// invalidate_gen under g_page_cache_lock, so the fallback is a plain access,
// matching the C reference's publication discipline on those profiles.
#[inline]
fn load_u64(p: *const u64) -> u64 {
    #[cfg(target_has_atomic = "64")]
    {
        // SAFETY: caller passes a live u64 field.
        unsafe { (*(p as *const core::sync::atomic::AtomicU64)).load(Ordering::Acquire) }
    }
    #[cfg(not(target_has_atomic = "64"))]
    {
        // SAFETY: caller holds g_page_cache_lock.
        unsafe { *p }
    }
}

#[inline]
fn add_u64(p: *mut u64, v: u64) {
    #[cfg(target_has_atomic = "64")]
    {
        // SAFETY: caller passes a live u64 field and holds the cache lock.
        unsafe {
            (*(p as *mut core::sync::atomic::AtomicU64)).fetch_add(v, Ordering::Release);
        }
    }
    #[cfg(not(target_has_atomic = "64"))]
    {
        // SAFETY: caller holds g_page_cache_lock.
        unsafe { *p = (*p).wrapping_add(v) };
    }
}

pub const PAGE_CACHE_HASH_BUCKETS: usize = 262144;
pub const PAGE_CACHE_BUCKET_LOCKS: usize = 1024;
/// `sizeof(struct page_cache_pin_counter)` (padded to one cache line).
pub const PIN_COUNTER_STRIDE: usize = 64;
const _: [(); 64] = [(); PIN_COUNTER_STRIDE];

/// Mirror of `page_cache_page_t` through `dirty_parent_color`.  `fill_lock` and
/// everything after it stay C-owned; the struct is never stored as an array in
/// Rust, only reached through pointers the C pool hands over.
#[repr(C)]
pub struct PageCachePage {
    pub vnode: *mut c_void,
    pub index: u64,
    pub ref_count: c_int,
    pub valid: c_int,
    pub dirty: c_int,
    pub dirty_gen: u64,
    pub invalidate_gen: u64,
    pub uptodate: c_int,
    pub accessed: u8,
    pub pfn: u32,
    pub data: *mut c_void,
    pub prev: *mut PageCachePage,
    pub next: *mut PageCachePage,
    pub hnext: *mut PageCachePage,
    pub mapping_prev: *mut PageCachePage,
    pub mapping_next: *mut PageCachePage,
    pub dirty_prev: *mut PageCachePage,
    pub dirty_next: *mut PageCachePage,
    pub global_dirty_prev: *mut PageCachePage,
    pub global_dirty_next: *mut PageCachePage,
    pub dirty_children: [*mut PageCachePage; 2],
    pub dirty_parent_color: usize,
}

/// Leading fields of `vnode_t` the page cache state model reads.  The C adapter
/// asserts `ino == 0`, `mnt == 48`, `cache_pages == 72` (LP64) / `36`/`48`
/// (ILP32) so this mirror can never silently drift.
#[repr(C)]
pub struct VnodeHead {
    pub ino: u64,
    pub vtype: c_int,
    pub mode: u32,
    pub uid: u32,
    pub gid: u32,
    pub size: usize,
    pub ref_count: c_int,
    pub parent: *mut c_void,
    pub mnt: *mut c_void,
    pub fs_data: *mut c_void,
    pub ops: *mut c_void,
    pub cache_pages: *mut PageCachePage,
    pub cache_dirty_pages: *mut PageCachePage,
    pub cache_dirty_tail: *mut PageCachePage,
    pub cache_dirty_root: *mut PageCachePage,
}

/// Optional telemetry hook: `page_cache_account_scan(entries)`.
pub type ScanFn = extern "C" fn(usize);
/// `pfn_valid(p) && pfa.meta[p].refcount <= 1`.
pub type PfnReclaimableFn = extern "C" fn(u32) -> c_int;
/// `spin_lock_irqsave(&g_page_cache_bucket_locks[blk])`.
pub type BucketLockFn = extern "C" fn(u32) -> u64;
/// `spin_unlock_irqrestore(&g_page_cache_bucket_locks[blk], flags)`.
pub type BucketUnlockFn = extern "C" fn(u32, u64);
pub type WritebackCollectFn = extern "C" fn(
    *mut VnodeHead,
    *mut *mut PageCachePage,
    usize,
) -> usize;
pub type WritebackLockFn = extern "C" fn(*mut *mut PageCachePage, usize);
pub type WritebackUnlockFn = extern "C" fn(*mut *mut PageCachePage, usize);
pub type WritebackBatchFn = extern "C" fn(*mut VnodeHead, usize) -> c_int;
pub type LegacyWritepageFn = extern "C" fn(
    *mut VnodeHead,
    u64,
    *const c_void,
    usize,
    *mut c_void,
) -> c_int;
pub type WritebackIoFn = extern "C" fn(
    *mut VnodeHead,
    u64,
    *mut *mut PageCachePage,
    usize,
    Option<LegacyWritepageFn>,
    *mut c_void,
    *mut c_void,
) -> c_int;
pub type WritebackBufferAllocFn = extern "C" fn(usize) -> *mut c_void;
pub type WritebackBufferFreeFn = extern "C" fn(*mut c_void);
pub type WritebackConfirmFn = extern "C" fn(
    *mut *mut PageCachePage,
    usize,
    *mut VnodeHead,
    u64,
    *const u64,
);
pub type WritebackPutFn = extern "C" fn(*mut PageCachePage);
pub type WritebackSerialFn = extern "C" fn(*mut VnodeHead);
pub type WritebackPerfFn = extern "C" fn(usize);
pub type CacheLockFn = extern "C" fn(*mut c_void) -> u64;
pub type CacheUnlockFn = extern "C" fn(*mut c_void, u64);
pub type CacheBucketLockFn = extern "C" fn(*mut c_void, u32) -> u64;
pub type CacheBucketUnlockFn = extern "C" fn(*mut c_void, u32, u64);

// Private transaction guards. Cache operations use raw C-layout nodes, but
// lock and caller-pin ownership still needs an explicit Rust lifetime: an early
// return must release the bucket/global lock and must not leak a lookup pin.
struct CacheGlobalLockGuard {
    context: *mut c_void,
    flags: u64,
    unlock: CacheUnlockFn,
    held: bool,
}

impl CacheGlobalLockGuard {
    fn acquire(context: *mut c_void, lock: CacheLockFn, unlock: CacheUnlockFn) -> Self {
        Self { context, flags: lock(context), unlock, held: true }
    }

    fn release(&mut self) {
        if self.held {
            self.held = false;
            (self.unlock)(self.context, self.flags);
        }
    }
}

impl Drop for CacheGlobalLockGuard {
    fn drop(&mut self) { self.release(); }
}

struct CacheBucketLockGuard {
    context: *mut c_void,
    bucket: u32,
    flags: u64,
    unlock: CacheBucketUnlockFn,
    held: bool,
}

impl CacheBucketLockGuard {
    fn acquire(
        context: *mut c_void,
        bucket: u32,
        lock: CacheBucketLockFn,
        unlock: CacheBucketUnlockFn,
    ) -> Self {
        Self { context, bucket, flags: lock(context, bucket), unlock, held: true }
    }

    fn release(&mut self) {
        if self.held {
            self.held = false;
            (self.unlock)(self.context, self.bucket, self.flags);
        }
    }
}

impl Drop for CacheBucketLockGuard {
    fn drop(&mut self) { self.release(); }
}

struct PagePinGuard {
    page: *mut PageCachePage,
    bucket_pinned: *mut usize,
}

impl PagePinGuard {
    unsafe fn pin(page: *mut PageCachePage, bucket_pinned: *mut usize) -> Self {
        unsafe {
            a20_pagecache_pin(page, bucket_pinned);
            (*page).accessed = 1;
        }
        Self { page, bucket_pinned }
    }

    fn transfer_to_caller(mut self) -> *mut PageCachePage {
        let page = self.page;
        self.page = core::ptr::null_mut();
        page
    }
}

impl Drop for PagePinGuard {
    fn drop(&mut self) {
        if !self.page.is_null() {
            unsafe { a20_pagecache_put(self.page, self.bucket_pinned); }
        }
    }
}
pub type CacheGrowFn = extern "C" fn(*mut c_void, c_int) -> c_int;
pub type CacheEvictFn = extern "C" fn(*mut c_void, *mut *mut c_void) -> *mut PageCachePage;
pub type CachePressureWritebackFn = extern "C" fn(*mut c_void, usize) -> c_int;
pub type CachePageZeroFn = extern "C" fn(*mut c_void, *mut PageCachePage);
pub type CacheVnodeRefFn = extern "C" fn(*mut c_void, *mut c_void);
pub type FilePageGetFn = extern "C" fn(*mut c_void, u64, c_int) -> *mut PageCachePage;
pub type FilePageFn = extern "C" fn(*mut c_void, *mut PageCachePage) -> c_int;
pub type FileReadAheadFn = extern "C" fn(*mut c_void, *mut PageCachePage, usize) -> c_int;
pub type FileReadHintFn = extern "C" fn(*mut c_void, u64) -> c_int;
pub type FileReadRecordFn = extern "C" fn(*mut c_void, u64);
pub type FilePagePutFn = extern "C" fn(*mut c_void, *mut PageCachePage);
pub type FileSeekFn = extern "C" fn(*mut c_void, u64);
pub type FileWriteCommitFn = extern "C" fn(*mut c_void, u64);
pub type FilePerfFn = extern "C" fn(*mut c_void, usize);
pub type PageFillLockFn = extern "C" fn(*mut c_void, *mut PageCachePage);
pub type PageReadyFn = extern "C" fn(*mut c_void, *mut PageCachePage) -> c_int;
pub type PageGenerationFn = extern "C" fn(*mut c_void, *mut PageCachePage) -> u64;
pub type PagePublishFn = extern "C" fn(*mut c_void, *mut PageCachePage, u64) -> c_int;
pub type PageReadOneFn = extern "C" fn(*mut c_void, *mut PageCachePage, *mut u8) -> c_int;
pub type PageReadManyFn = extern "C" fn(*mut c_void, u64, *mut u8, usize) -> c_int;
pub type PageBufferTakeFn = extern "C" fn(*mut c_void, usize) -> *mut u8;
pub type PageBufferReturnFn = extern "C" fn(*mut c_void, *mut u8);
pub type PageWindowFillFn = extern "C" fn(*mut c_void, *mut *mut PageCachePage, usize) -> c_int;
pub type PageWindowPerfFn = extern "C" fn(*mut c_void, usize);

/// Own page-fill locking, readpages window selection, short-read zeroing,
/// generation validation, and publication. C callbacks perform only the
/// concrete filesystem I/O, page mutex operations, and reusable buffer pool.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_fill_one_transaction(
    context: *mut c_void, page: *mut PageCachePage,
    lock: Option<PageFillLockFn>, unlock: Option<PageFillLockFn>,
    ready: Option<PageReadyFn>, generation: Option<PageGenerationFn>,
    publish: Option<PagePublishFn>, read_one: Option<PageReadOneFn>,
) -> c_int {
    let (Some(lock), Some(unlock), Some(ready), Some(generation), Some(publish), Some(read_one)) =
        (lock, unlock, ready, generation, publish, read_one) else { return -22; };
    if page.is_null() { return -22; }
    lock(context, page);
    if ready(context, page) != 0 {
        unlock(context, page);
        return 0;
    }
    let data = unsafe { (*page).data as *mut u8 };
    if data.is_null() {
        unlock(context, page);
        return -12;
    }
    loop {
        let generation_before = generation(context, page);
        let result = read_one(context, page, data);
        if result < 0 {
            unlock(context, page);
            return result;
        }
        let amount = result as usize;
        if amount < PAGE_SIZE {
            unsafe { core::ptr::write_bytes(data.add(amount), 0, PAGE_SIZE - amount); }
        }
        if publish(context, page, generation_before) != 0 {
            unlock(context, page);
            return result;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_fill_many_transaction(
    context: *mut c_void, pages: *mut *mut PageCachePage, count: usize,
    max_pages: usize, lock: Option<PageFillLockFn>, unlock: Option<PageFillLockFn>,
    ready: Option<PageReadyFn>, generation: Option<PageGenerationFn>,
    publish: Option<PagePublishFn>, read_one: Option<PageReadOneFn>,
    read_many: Option<PageReadManyFn>, take_buffer: Option<PageBufferTakeFn>,
    return_buffer: Option<PageBufferReturnFn>,
) -> c_int {
    let (Some(lock), Some(unlock), Some(ready), Some(generation), Some(publish),
         Some(read_one), Some(take_buffer), Some(return_buffer)) =
        (lock, unlock, ready, generation, publish, read_one, take_buffer, return_buffer)
        else { return -22; };
    if pages.is_null() || count == 0 || count > max_pages || max_pages == 0 || max_pages > 32 { return -22; }
    let first = unsafe { *pages };
    if first.is_null() { return -22; }
    let vnode = unsafe { (*first).vnode };
    let mut i = 0usize;
    while i < count {
        let page = unsafe { *pages.add(i) };
        if page.is_null() || unsafe { (*page).vnode != vnode } {
            return -22;
        }
        if i > 0 {
            let prev = unsafe { *pages.add(i - 1) };
            if unsafe { (*page).index != (*prev).index.wrapping_add(1) } { return -22; }
        }
        i += 1;
    }
    let read_many = match read_many {
        Some(read_many) => read_many,
        None => {
            for i in 0..count {
                let page = unsafe { *pages.add(i) };
                if ready(context, page) == 0 {
                    let ret = unsafe { a20_pagecache_fill_one_transaction(
                        context, page, Some(lock), Some(unlock), Some(ready),
                        Some(generation), Some(publish), Some(read_one)) };
                    if ret < 0 { return ret; }
                }
            }
            return 0;
        }
    };
    let buffer = take_buffer(context, max_pages.saturating_mul(PAGE_SIZE));
    if buffer.is_null() {
        let first = unsafe { *pages };
        return unsafe { a20_pagecache_fill_one_transaction(
            context, first, Some(lock), Some(unlock), Some(ready),
            Some(generation), Some(publish), Some(read_one)) };
    }
    for i in 0..count { lock(context, unsafe { *pages.add(i) }); }
    let mut result = 0;
    loop {
        let mut retry = false;
        let mut cursor = 0usize;
        while cursor < count {
            while cursor < count && ready(context, unsafe { *pages.add(cursor) }) != 0 { cursor += 1; }
            if cursor == count { break; }
            let start = cursor;
            while cursor < count && ready(context, unsafe { *pages.add(cursor) }) == 0 { cursor += 1; }
            let run = cursor - start;
            let mut generations = [0u64; 32];
            for j in 0..run { generations[j] = generation(context, unsafe { *pages.add(start + j) }); }
            let first_page = unsafe { *pages.add(start) };
            let bytes = run * PAGE_SIZE;
            let read = read_many(context, unsafe { (*first_page).index }, buffer, bytes);
            if read < 0 { result = read; break; }
            let amount = core::cmp::min(read as usize, bytes);
            if amount < bytes { unsafe { core::ptr::write_bytes(buffer.add(amount), 0, bytes - amount); } }
            for j in 0..run {
                let page = unsafe { *pages.add(start + j) };
                unsafe { core::ptr::copy_nonoverlapping(buffer.add(j * PAGE_SIZE), (*page).data as *mut u8, PAGE_SIZE); }
                if publish(context, page, generations[j]) == 0 { retry = true; }
            }
        }
        if result < 0 || !retry { break; }
    }
    for i in (0..count).rev() { unlock(context, unsafe { *pages.add(i) }); }
    return_buffer(context, buffer);
    result
}

/// Own sequential read-ahead window sizing and the pin/release lifecycle.
/// The filesystem fill operation remains a callback into the page-fill
/// transaction above; page acquisition and release are allocator mechanisms.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_readahead_transaction(
    context: *mut c_void, first: *mut PageCachePage, file_size: usize,
    max_pages: usize, get: Option<FilePageGetFn>, fill: Option<PageWindowFillFn>,
    put: Option<FilePagePutFn>, perf: Option<PageWindowPerfFn>,
) -> c_int {
    let (Some(get), Some(fill), Some(put), Some(perf)) = (get, fill, put, perf)
        else { return -22; };
    if context.is_null() || first.is_null() || max_pages == 0 || max_pages > 32 { return -22; }
    let mut pages = [core::ptr::null_mut(); 32];
    pages[0] = first;
    let first_index = unsafe { (*first).index };
    let file_pages = file_size.saturating_add(PAGE_SIZE - 1) / PAGE_SIZE;
    let mut count = 1usize;
    while count < max_pages && first_index.wrapping_add(count as u64) < file_pages as u64 {
        let page = get(context, first_index.wrapping_add(count as u64), 1);
        if page.is_null() { break; }
        pages[count] = page;
        count += 1;
    }
    perf(context, count);
    let result = fill(context, pages.as_mut_ptr(), count);
    while count > 1 {
        count -= 1;
        put(context, pages[count]);
    }
    result
}

/// Drive the page-cache writeback transaction.  The C adapter supplies only
/// the lock, vnode/device callback, and page-release primitives; Rust owns
/// batch selection, ordering, generation snapshots, success confirmation,
/// failure stop, and exactly-once unlock/unpin cleanup.
///
/// # Safety
/// The callbacks obey their narrow contracts and all page pointers returned
/// by `collect` remain pinned until `put` is called.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_writeback_transaction(
    vn: *mut VnodeHead,
    writepage: Option<LegacyWritepageFn>,
    ctx: *mut c_void,
    max_pages: usize,
    written_pages: *mut usize,
    collect: Option<WritebackCollectFn>,
    lock_pages: Option<WritebackLockFn>,
    unlock_pages: Option<WritebackUnlockFn>,
    batch_allowed: Option<WritebackBatchFn>,
    write: Option<WritebackIoFn>,
    alloc_buffer: Option<WritebackBufferAllocFn>,
    free_buffer: Option<WritebackBufferFreeFn>,
    confirm: Option<WritebackConfirmFn>,
    put: Option<WritebackPutFn>,
    serial_lock: Option<WritebackSerialFn>,
    serial_unlock: Option<WritebackSerialFn>,
    perf: Option<WritebackPerfFn>,
) -> c_int {
    let (collect, lock_pages, unlock_pages, batch_allowed, write, alloc_buffer,
         free_buffer, confirm, put, serial_lock, serial_unlock) = match (
        collect, lock_pages, unlock_pages, batch_allowed, write, alloc_buffer,
        free_buffer, confirm, put, serial_lock, serial_unlock,
    ) {
        (Some(c), Some(l), Some(u), Some(b), Some(w), Some(a), Some(f),
         Some(cf), Some(p), Some(sl), Some(su)) =>
            (c, l, u, b, w, a, f, cf, p, sl, su),
        _ => return -22,
    };
    const BATCH: usize = 256;
    let mut pages = [core::ptr::null_mut(); BATCH];
    let mut gens = [0u64; BATCH];
    let mut written = 0usize;
    let mut result = 0;
    let mut linear: *mut c_void = core::ptr::null_mut();
    if !written_pages.is_null() {
        unsafe { *written_pages = 0 };
    }
    serial_lock(vn);
    loop {
        let limit = if max_pages != 0 {
            core::cmp::min(BATCH, max_pages.saturating_sub(written))
        } else {
            BATCH
        };
        if limit == 0 {
            break;
        }
        let count = collect(vn, pages.as_mut_ptr(), limit);
        if count == 0 {
            break;
        }
        let first = pages[0];
        let page_vn = unsafe { (*first).vnode as *mut VnodeHead };
        let index = unsafe { (*first).index };
        // A whole-cache flush owns the global collector gate, not the
        // per-vnode writeback gate. It must also take the actual vnode's
        // gate before touching its inode: otherwise sync/pressure writeback
        // and a concurrent fsync can publish competing extent snapshots.
        let global_batch = vn.is_null() && !page_vn.is_null();
        if global_batch {
            serial_lock(page_vn);
        }
        let batched = writepage.is_none() && batch_allowed(page_vn, count) != 0;
        let count = if batched || count == 1 {
            count
        } else {
            for page in pages.iter_mut().take(count).skip(1) {
                if !page.is_null() {
                    put(*page);
                    *page = core::ptr::null_mut();
                }
            }
            1
        };
        lock_pages(pages.as_mut_ptr(), count);
        for i in 0..count {
            gens[i] = unsafe {
                load_u64(core::ptr::addr_of!((*pages[i]).dirty_gen))
            };
        }
        if batched && linear.is_null() {
            linear = alloc_buffer(BATCH * PAGE_SIZE);
            if linear.is_null() {
                result = -12;
            }
        }
        if result == 0 {
            let write_result = write(page_vn, index, pages.as_mut_ptr(), count,
                if batched { None } else { writepage }, linear, ctx);
            if write_result < 0 {
                result = write_result;
            }
        }
        if result >= 0 {
            confirm(pages.as_mut_ptr(), count, page_vn, index, gens.as_ptr());
        }
        unlock_pages(pages.as_mut_ptr(), count);
        for page in pages.iter_mut().take(count) {
            if !page.is_null() {
                put(*page);
                *page = core::ptr::null_mut();
            }
        }
        if global_batch {
            serial_unlock(page_vn);
        }
        if result < 0 {
            break;
        }
        written = written.saturating_add(count);
        if let Some(f) = perf {
            f(count);
        }
        if !written_pages.is_null() {
            unsafe { *written_pages = written };
        }
        if max_pages != 0 && written >= max_pages {
            break;
        }
    }
    serial_unlock(vn);
    if !linear.is_null() {
        free_buffer(linear);
    }
    result
}

#[inline]
unsafe fn page<'a>(p: *mut PageCachePage) -> &'a mut PageCachePage {
    // SAFETY: callers pass a live pool descriptor.
    unsafe { &mut *p }
}

#[inline]
fn scan(account: Option<ScanFn>, entries: usize) {
    if let Some(f) = account {
        f(entries);
    }
}

#[inline]
fn hash_index(vn: *mut VnodeHead, index: u64) -> usize {
    let v: u64 = if !vn.is_null() && unsafe { !(*vn).mnt.is_null() } {
        (unsafe { (*vn).mnt } as usize) as u64 ^ unsafe { (*vn).ino }
    } else {
        vn as usize as u64
    };
    (((v as u64) >> 4) ^ index ^ (index >> 32)) as usize & (PAGE_CACHE_HASH_BUCKETS - 1)
}

#[inline]
fn bucket_of(idx: usize) -> u32 {
    (idx & (PAGE_CACHE_BUCKET_LOCKS - 1)) as u32
}

#[inline]
fn ref_read(p: *mut PageCachePage) -> c_int {
    // SAFETY: relaxed atomic load of the caller's live refcount field.
    unsafe { (*(core::ptr::addr_of!((*p).ref_count) as *const AtomicI32)).load(Ordering::Relaxed) }
}

#[inline]
fn ref_set(p: *mut PageCachePage, v: c_int) {
    // SAFETY: relaxed atomic store, matching `refcount_set`.
    unsafe { (*(core::ptr::addr_of_mut!((*p).ref_count) as *mut AtomicI32)).store(v, Ordering::Relaxed) }
}

#[inline]
fn pin_counter_add(base: *mut usize, page_no: *mut PageCachePage, delta: isize) {
    if base.is_null() || page_no.is_null() {
        return;
    }
    let idx = hash_index(unsafe { (*page_no).vnode as *mut VnodeHead }, unsafe { (*page_no).index });
    let blk = bucket_of(idx) as usize;
    // SAFETY: `base` is g_bucket_pinned, an array of PAGE_CACHE_BUCKET_LOCKS
    // 64-byte counters; the atomic is the leading size_t of the bucket.
    let slot = unsafe { (base as *mut AtomicUsize).add(blk * (PIN_COUNTER_STRIDE / core::mem::size_of::<usize>())) };
    if delta >= 0 {
        unsafe { (*slot).fetch_add(delta as usize, Ordering::Relaxed) };
    } else {
        unsafe { (*slot).fetch_sub((-delta) as usize, Ordering::Relaxed) };
    }
}

// ------------------------------------------------------------------
// hash key / bucket
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_hash_key(vn: *mut VnodeHead, index: u64) -> u32 {
    hash_index(vn, index) as u32
}

// ------------------------------------------------------------------
// global LRU
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_lru_remove(p: *mut PageCachePage) {
    if p.is_null() {
        return;
    }
    // SAFETY: p is on the LRU list; its prev/next are live sentinels or pages.
    unsafe {
        let page = page(p);
        (*page.prev).next = page.next;
        (*page.next).prev = page.prev;
        page.prev = core::ptr::null_mut();
        page.next = core::ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_lru_insert_front(head: *mut PageCachePage, p: *mut PageCachePage) {
    if head.is_null() || p.is_null() {
        return;
    }
    // SAFETY: head is the g_lru_head sentinel; caller holds g_page_cache_lock.
    unsafe {
        let page = page(p);
        page.next = (*head).next;
        page.prev = head;
        (*(*head).next).prev = p;
        (*head).next = p;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_lru_insert_tail(tail: *mut PageCachePage, p: *mut PageCachePage) {
    if tail.is_null() || p.is_null() {
        return;
    }
    // SAFETY: tail is the g_lru_tail sentinel; caller holds g_page_cache_lock.
    unsafe {
        let page = page(p);
        page.prev = (*tail).prev;
        page.next = tail;
        (*(*tail).prev).next = p;
        (*tail).prev = p;
    }
}

// ------------------------------------------------------------------
// free stack
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_free_insert(
    free_head: *mut *mut PageCachePage,
    p: *mut PageCachePage,
) {
    if free_head.is_null() || p.is_null() {
        return;
    }
    // SAFETY: free stack is linked through `hnext`, as in the C reference.
    unsafe {
        (*p).hnext = *free_head;
        *free_head = p;
    }
}

/// Take one free descriptor and publish it on the LRU front.  Returns null when
/// the free stack is empty.  Mirrors `free_take_locked`.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_free_take(
    free_head: *mut *mut PageCachePage,
    lru_head: *mut PageCachePage,
) -> *mut PageCachePage {
    if free_head.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: free stack membership is guarded by g_page_cache_lock.
    unsafe {
        let p = *free_head;
        if p.is_null() {
            return core::ptr::null_mut();
        }
        *free_head = (*p).hnext;
        (*p).hnext = core::ptr::null_mut();
        ref_set(p, 1);
        a20_pagecache_lru_remove(p);
        a20_pagecache_lru_insert_front(lru_head, p);
        p
    }
}

// ------------------------------------------------------------------
// hash membership
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_hash_insert(hash: *mut *mut PageCachePage, p: *mut PageCachePage) {
    if hash.is_null() || p.is_null() {
        return;
    }
    // SAFETY: hash is the g_hash bucket array; caller holds the bucket lock.
    unsafe {
        let idx = hash_index((*p).vnode as *mut VnodeHead, (*p).index);
        (*p).hnext = *hash.add(idx);
        *hash.add(idx) = p;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_hash_remove(
    hash: *mut *mut PageCachePage,
    p: *mut PageCachePage,
    account: Option<ScanFn>,
) {
    if hash.is_null() || p.is_null() {
        return;
    }
    // SAFETY: walks the bucket chain mutating the predecessor's link.
    unsafe {
        let idx = hash_index((*p).vnode as *mut VnodeHead, (*p).index);
        let mut pp = hash.add(idx);
        let mut visited = 0usize;
        while !(*pp).is_null() {
            visited += 1;
            if *pp == p {
                *pp = (*p).hnext;
                (*p).hnext = core::ptr::null_mut();
                scan(account, visited);
                return;
            }
            pp = core::ptr::addr_of_mut!((**pp).hnext);
        }
        scan(account, visited);
        (*p).hnext = core::ptr::null_mut();
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_find(
    hash: *mut *mut PageCachePage,
    vn: *mut VnodeHead,
    index: u64,
    account: Option<ScanFn>,
) -> *mut PageCachePage {
    if hash.is_null() {
        return core::ptr::null_mut();
    }
    // SAFETY: reads a bucket chain; entries belong to the C pool.
    unsafe {
        let idx = hash_index(vn, index);
        let mut visited = 0usize;
        let mut p = *hash.add(idx);
        while !p.is_null() {
            visited += 1;
            if (*p).valid != 0 && (*p).vnode == vn as *mut c_void && (*p).index == index {
                scan(account, visited);
                return p;
            }
            p = (*p).hnext;
        }
        scan(account, visited);
        core::ptr::null_mut()
    }
}

// ------------------------------------------------------------------
// per-vnode mapping list
// ------------------------------------------------------------------

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_mapping_insert(p: *mut PageCachePage) {
    if p.is_null() || (*p).vnode.is_null() {
        return;
    }
    // SAFETY: page->vnode is a live vnode; the mapping list is spliced under
    // g_page_cache_lock.
    unsafe {
        let vn = &mut (*((*p).vnode as *mut VnodeHead));
        (*p).mapping_prev = core::ptr::null_mut();
        (*p).mapping_next = vn.cache_pages;
        if !vn.cache_pages.is_null() {
            (*vn.cache_pages).mapping_prev = p;
        }
        vn.cache_pages = p;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_mapping_remove(p: *mut PageCachePage) {
    if p.is_null() {
        return;
    }
    // SAFETY: caller holds g_page_cache_lock; page is on the vnode mapping list.
    unsafe {
        let vn = if (*p).vnode.is_null() {
            core::ptr::null_mut()
        } else {
            (*p).vnode as *mut VnodeHead
        };
        if !(*p).mapping_prev.is_null() {
            (*(*p).mapping_prev).mapping_next = (*p).mapping_next;
        } else if !vn.is_null() {
            (*vn).cache_pages = (*p).mapping_next;
        }
        if !(*p).mapping_next.is_null() {
            (*(*p).mapping_next).mapping_prev = (*p).mapping_prev;
        }
        (*p).mapping_prev = core::ptr::null_mut();
        (*p).mapping_next = core::ptr::null_mut();
    }
}

// ------------------------------------------------------------------
// dirty lists (per-vnode descending index + global dirtied order)
// ------------------------------------------------------------------

// Intrusive red-black index. The ordered dirty links remain the writeback
// iterator; this index only removes the linear search for middle insertion.
// All operations hold g_page_cache_lock; there are no allocations or recursion.
// Descriptor alignment leaves bit 0 of the parent pointer available for red.
#[inline]
unsafe fn dirty_parent(p: *mut PageCachePage) -> *mut PageCachePage {
    ((*p).dirty_parent_color & !1usize) as *mut PageCachePage
}
#[inline]
unsafe fn dirty_red(p: *mut PageCachePage) -> bool {
    !p.is_null() && (*p).dirty_parent_color & 1 != 0
}
#[inline]
unsafe fn dirty_set_parent(p: *mut PageCachePage, parent: *mut PageCachePage) {
    if !p.is_null() {
        (*p).dirty_parent_color = parent as usize | ((*p).dirty_parent_color & 1);
    }
}
#[inline]
unsafe fn dirty_set_red(p: *mut PageCachePage, red: bool) {
    if !p.is_null() {
        (*p).dirty_parent_color = ((*p).dirty_parent_color & !1usize) | red as usize;
    }
}

// Promote x's child on `side` (0 = right rotation, 1 = left rotation).
unsafe fn dirty_rotate(root: &mut *mut PageCachePage, x: *mut PageCachePage, side: usize) {
    let y = (*x).dirty_children[side];
    let parent = dirty_parent(x);
    (*x).dirty_children[side] = (*y).dirty_children[1 - side];
    dirty_set_parent((*x).dirty_children[side], x);
    dirty_set_parent(y, parent);
    if parent.is_null() {
        *root = y;
    } else {
        let at = ((*parent).dirty_children[1] == x) as usize;
        (*parent).dirty_children[at] = y;
    }
    (*y).dirty_children[1 - side] = x;
    dirty_set_parent(x, y);
}

/// Insert p and return the nearest strictly higher index for the dirty list.
unsafe fn dirty_tree_insert(vn: &mut VnodeHead, p: *mut PageCachePage) -> *mut PageCachePage {
    let mut parent = core::ptr::null_mut();
    let mut at = vn.cache_dirty_root;
    let mut higher = core::ptr::null_mut();
    let mut side = 0;
    // Preserve the old list's constant-time position lookup for sequential
    // writers. Min/max leaves have a free outward child; rotations still do
    // the usual bounded repair. Strict inequalities keep duplicate-key test
    // inputs on the general path (production hash keys are unique per vnode).
    let head = vn.cache_dirty_pages;
    let tail = vn.cache_dirty_tail;
    if !head.is_null() && (*p).index > (*head).index &&
       (*head).dirty_children[1].is_null() {
        parent = head;
        side = 1;
    } else if !tail.is_null() && (*p).index < (*tail).index &&
              (*tail).dirty_children[0].is_null() {
        parent = tail;
        higher = tail;
    } else {
        while !at.is_null() {
            parent = at;
            side = ((*p).index >= (*at).index) as usize;
            if side == 0 { higher = at; }
            at = (*at).dirty_children[side];
        }
    }
    (*p).dirty_children = [core::ptr::null_mut(); 2];
    (*p).dirty_parent_color = parent as usize | 1;
    if parent.is_null() { vn.cache_dirty_root = p; }
    else { (*parent).dirty_children[side] = p; }

    let mut x = p;
    while dirty_red(dirty_parent(x)) {
        let mut par = dirty_parent(x);
        let grand = dirty_parent(par);
        let dir = ((*grand).dirty_children[1] == par) as usize;
        let uncle = (*grand).dirty_children[1 - dir];
        if dirty_red(uncle) {
            dirty_set_red(par, false);
            dirty_set_red(uncle, false);
            dirty_set_red(grand, true);
            x = grand;
        } else {
            if x == (*par).dirty_children[1 - dir] {
                x = par;
                dirty_rotate(&mut vn.cache_dirty_root, x, 1 - dir);
                par = dirty_parent(x);
            }
            dirty_set_red(par, false);
            dirty_set_red(grand, true);
            dirty_rotate(&mut vn.cache_dirty_root, grand, dir);
        }
    }
    dirty_set_red(vn.cache_dirty_root, false);
    higher
}

unsafe fn dirty_transplant(root: &mut *mut PageCachePage, old: *mut PageCachePage,
                          new: *mut PageCachePage) {
    let parent = dirty_parent(old);
    if parent.is_null() { *root = new; }
    else { (*parent).dirty_children[((*parent).dirty_children[1] == old) as usize] = new; }
    dirty_set_parent(new, parent);
}

unsafe fn dirty_tree_remove(vn: &mut VnodeHead, z: *mut PageCachePage) {
    let mut y = z;
    let mut was_red = dirty_red(y);
    let mut x;
    let mut parent;
    if (*z).dirty_children[0].is_null() {
        x = (*z).dirty_children[1];
        parent = dirty_parent(z);
        dirty_transplant(&mut vn.cache_dirty_root, z, x);
    } else if (*z).dirty_children[1].is_null() {
        x = (*z).dirty_children[0];
        parent = dirty_parent(z);
        dirty_transplant(&mut vn.cache_dirty_root, z, x);
    } else {
        y = (*z).dirty_children[1];
        while !(*y).dirty_children[0].is_null() { y = (*y).dirty_children[0]; }
        was_red = dirty_red(y);
        x = (*y).dirty_children[1];
        if dirty_parent(y) == z {
            parent = y;
            dirty_set_parent(x, y);
        } else {
            parent = dirty_parent(y);
            dirty_transplant(&mut vn.cache_dirty_root, y, x);
            (*y).dirty_children[1] = (*z).dirty_children[1];
            dirty_set_parent((*y).dirty_children[1], y);
        }
        dirty_transplant(&mut vn.cache_dirty_root, z, y);
        (*y).dirty_children[0] = (*z).dirty_children[0];
        dirty_set_parent((*y).dirty_children[0], y);
        dirty_set_red(y, dirty_red(z));
    }
    if !was_red {
        while x != vn.cache_dirty_root && !dirty_red(x) {
            // A null leaf has no embedded parent; carry it across transplants.
            let dir = ((*parent).dirty_children[1] == x &&
                       (*parent).dirty_children[0] != x) as usize;
            let mut sibling = (*parent).dirty_children[1 - dir];
            if dirty_red(sibling) {
                dirty_set_red(sibling, false);
                dirty_set_red(parent, true);
                dirty_rotate(&mut vn.cache_dirty_root, parent, 1 - dir);
                sibling = (*parent).dirty_children[1 - dir];
            }
            // A valid RB tree has a sibling here; null is black defensively.
            if sibling.is_null() || (!dirty_red((*sibling).dirty_children[0]) &&
                                     !dirty_red((*sibling).dirty_children[1])) {
                dirty_set_red(sibling, true);
                x = parent;
                parent = dirty_parent(x);
            } else {
                if !dirty_red((*sibling).dirty_children[1 - dir]) {
                    dirty_set_red((*sibling).dirty_children[dir], false);
                    dirty_set_red(sibling, true);
                    dirty_rotate(&mut vn.cache_dirty_root, sibling, dir);
                    sibling = (*parent).dirty_children[1 - dir];
                }
                dirty_set_red(sibling, dirty_red(parent));
                dirty_set_red(parent, false);
                dirty_set_red((*sibling).dirty_children[1 - dir], false);
                dirty_rotate(&mut vn.cache_dirty_root, parent, 1 - dir);
                x = vn.cache_dirty_root;
                break;
            }
        }
        dirty_set_red(x, false);
    }
    (*z).dirty_children = [core::ptr::null_mut(); 2];
    (*z).dirty_parent_color = 0;
}

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_dirty_insert(
    p: *mut PageCachePage,
    global_dirty_head: *mut *mut PageCachePage,
    global_dirty_count: *mut usize,
) {
    if p.is_null() || (*p).dirty != 0 || (*p).vnode.is_null() {
        return;
    }
    // SAFETY: caller holds g_page_cache_lock; page->vnode is live.
    unsafe {
        let vn = &mut (*((*p).vnode as *mut VnodeHead));
        let higher = dirty_tree_insert(vn, p);
        let head = vn.cache_dirty_pages;
        let tail = vn.cache_dirty_tail;
        if head.is_null() {
            (*p).dirty_prev = core::ptr::null_mut();
            (*p).dirty_next = core::ptr::null_mut();
            vn.cache_dirty_pages = p;
            vn.cache_dirty_tail = p;
        } else if (*p).index >= (*head).index {
            (*p).dirty_prev = core::ptr::null_mut();
            (*p).dirty_next = head;
            (*head).dirty_prev = p;
            vn.cache_dirty_pages = p;
        } else if (*p).index <= (*tail).index {
            (*p).dirty_prev = tail;
            (*p).dirty_next = core::ptr::null_mut();
            (*tail).dirty_next = p;
            vn.cache_dirty_tail = p;
        } else {
            let at = higher;
            (*p).dirty_prev = at;
            (*p).dirty_next = (*at).dirty_next;
            (*(*at).dirty_next).dirty_prev = p;
            (*at).dirty_next = p;
        }

        (*p).global_dirty_prev = core::ptr::null_mut();
        (*p).global_dirty_next = *global_dirty_head;
        if !(*global_dirty_head).is_null() {
            (**global_dirty_head).global_dirty_prev = p;
        }
        *global_dirty_head = p;
        (*p).dirty = 1;
        *global_dirty_count += 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_dirty_remove(
    p: *mut PageCachePage,
    global_dirty_head: *mut *mut PageCachePage,
    global_dirty_count: *mut usize,
) {
    if p.is_null() || (*p).dirty == 0 {
        return;
    }
    // SAFETY: caller holds g_page_cache_lock.
    unsafe {
        let vn = if (*p).vnode.is_null() {
            core::ptr::null_mut()
        } else {
            (*p).vnode as *mut VnodeHead
        };
        if !vn.is_null() { dirty_tree_remove(&mut *vn, p); }
        if !(*p).dirty_prev.is_null() {
            (*(*p).dirty_prev).dirty_next = (*p).dirty_next;
        } else if !vn.is_null() {
            (*vn).cache_dirty_pages = (*p).dirty_next;
        }
        if !(*p).dirty_next.is_null() {
            (*(*p).dirty_next).dirty_prev = (*p).dirty_prev;
        } else if !vn.is_null() {
            (*vn).cache_dirty_tail = (*p).dirty_prev;
        }

        if !(*p).global_dirty_prev.is_null() {
            (*(*p).global_dirty_prev).global_dirty_next = (*p).global_dirty_next;
        } else {
            *global_dirty_head = (*p).global_dirty_next;
        }
        if !(*p).global_dirty_next.is_null() {
            (*(*p).global_dirty_next).global_dirty_prev = (*p).global_dirty_prev;
        }

        (*p).dirty_prev = core::ptr::null_mut();
        (*p).dirty_next = core::ptr::null_mut();
        (*p).global_dirty_prev = core::ptr::null_mut();
        (*p).global_dirty_next = core::ptr::null_mut();
        (*p).dirty = 0;
        *global_dirty_count -= 1;
    }
}

// ------------------------------------------------------------------
// detach + reference accounting
// ------------------------------------------------------------------

/// Full deferred detach: unlink from the dirty lists, the per-vnode mapping,
/// the hash chain, then clear the descriptor and bump `invalidate_gen`.
/// Returns the vnode whose cache reference the caller must drop after it has
/// released the spinlock.  Mirrors `detach_mapping_deferred_locked`.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_detach(
    p: *mut PageCachePage,
    global_dirty_head: *mut *mut PageCachePage,
    global_dirty_count: *mut usize,
    hash: *mut *mut PageCachePage,
    valid_pages: *mut usize,
    account: Option<ScanFn>,
) -> *mut c_void {
    if p.is_null() || (*p).valid == 0 {
        return core::ptr::null_mut();
    }
    // SAFETY: caller holds g_page_cache_lock and the page's bucket lock.
    unsafe {
        a20_pagecache_dirty_remove(p, global_dirty_head, global_dirty_count);
        a20_pagecache_mapping_remove(p);
        a20_pagecache_hash_remove(hash, p, account);
        let vn = (*p).vnode;
        (*p).vnode = core::ptr::null_mut();
        (*p).index = 0;
        (*p).valid = 0;
        (*p).dirty = 0;
        (*p).dirty_gen = 0;
        (*p).invalidate_gen = (*p).invalidate_gen.wrapping_add(1);
        (*p).uptodate = 0;
        *valid_pages -= 1;
        vn
    }
}

/// `page_cache_account_pin`: count a pin for a page whose descriptor came off
/// the free stack already holding its reference (publish path).
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_account_pin(p: *mut PageCachePage, bucket_pinned: *mut usize) {
    pin_counter_add(bucket_pinned, p, 1);
}

/// `page_cache_pin`: bump the refcount, counting the 0->1 edge in the page's
/// bucket pin counter so the reported pinned count never walks the array.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_pin(p: *mut PageCachePage, bucket_pinned: *mut usize) {
    if p.is_null() {
        return;
    }
    // SAFETY: relaxed fetch_add on the caller's live refcount field.
    let a = core::ptr::addr_of_mut!((*p).ref_count) as *mut AtomicI32;
    if unsafe { (*a).fetch_add(1, Ordering::Relaxed) } == 0 {
        pin_counter_add(bucket_pinned, p, 1);
    }
}

/// `page_cache_put`: saturating release; the 1->0 edge decrements the bucket's
/// pinned counter.  Returns 1 when the page dropped to zero references.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_put(p: *mut PageCachePage, bucket_pinned: *mut usize) -> c_int {
    if p.is_null() {
        return 0;
    }
    // SAFETY: lazy refcount_dec_and_test, identical to the C helper.
    unsafe {
        let a = core::ptr::addr_of_mut!((*p).ref_count) as *mut AtomicI32;
        if (*a).load(Ordering::Relaxed) > 0 && (*a).fetch_sub(1, Ordering::AcqRel) == 1 {
            pin_counter_add(bucket_pinned, p, -1);
            return 1;
        }
    }
    0
}

/// Sum of the per-bucket pin counters for `page_cache_get_stats`.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_pinned_total(bucket_pinned: *const usize) -> usize {
    if bucket_pinned.is_null() {
        return 0;
    }
    let mut total = 0usize;
    let slots = PAGE_CACHE_BUCKET_LOCKS * (PIN_COUNTER_STRIDE / core::mem::size_of::<usize>());
    // SAFETY: reads the padded counter array; the atomic is each bucket's lead.
    unsafe {
        for i in 0..PAGE_CACHE_BUCKET_LOCKS {
            let slot = (bucket_pinned as *const AtomicUsize)
                .add(i * (PIN_COUNTER_STRIDE / core::mem::size_of::<usize>()));
            total = total.wrapping_add((*slot).load(Ordering::Relaxed));
        }
        let _ = slots;
    }
    total
}

// ------------------------------------------------------------------
// eviction
// ------------------------------------------------------------------

/// Bounded second-chance eviction sweep.  The caller holds
/// `g_page_cache_lock`; the function takes each candidate's bucket lock through
/// the supplied callbacks to re-check the refcount/dirty state atomically with
/// the detach.  Returns the reclaimed descriptor or null when the budget or the
/// LRU head is reached.  Mirrors `evict_locked`.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_evict_scan(
    lru_head: *mut PageCachePage,
    lru_tail: *mut PageCachePage,
    budget: usize,
    pfn_reclaimable: Option<PfnReclaimableFn>,
    lock_bucket: Option<BucketLockFn>,
    unlock_bucket: Option<BucketUnlockFn>,
    hash: *mut *mut PageCachePage,
    global_dirty_head: *mut *mut PageCachePage,
    global_dirty_count: *mut usize,
    valid_pages: *mut usize,
    account: Option<ScanFn>,
    out_vnode: *mut *mut c_void,
) -> *mut PageCachePage {
    if lru_head.is_null() || lru_tail.is_null() {
        return core::ptr::null_mut();
    }
    let (reclaimable, lockb, unlockb) = match (pfn_reclaimable, lock_bucket, unlock_bucket) {
        (Some(r), Some(l), Some(u)) => (r, l, u),
        _ => return core::ptr::null_mut(),
    };
    if out_vnode.is_null() {
        return core::ptr::null_mut();
    }
    unsafe {
        *out_vnode = core::ptr::null_mut();
        let mut p = (*lru_tail).prev;
        let mut visited = 0usize;
        while p != lru_head && visited < budget {
            visited += 1;
            // Move every visited descriptor, including pinned/dirty ones,
            // away from the tail. Otherwise each bounded call inspects the
            // same window forever while clean pages beyond it go unused.
            let next = (*p).prev;
            a20_pagecache_lru_remove(p);
            a20_pagecache_lru_insert_front(lru_head, p);
            if ref_read(p) == 0
                && (*p).dirty == 0
                && reclaimable((*p).pfn) != 0
            {
                if (*p).valid != 0 {
                    let idx = hash_index((*p).vnode as *mut VnodeHead, (*p).index);
                    let blk = bucket_of(idx);
                    let bflags = lockb(blk);
                    let mut detached = false;
                    if (*p).valid != 0 && ref_read(p) == 0 && (*p).dirty == 0 {
                        if (*p).accessed != 0 {
                            (*p).accessed = 0;
                            unlockb(blk, bflags);
                            p = next;
                            continue;
                        }
                        *out_vnode = a20_pagecache_detach(
                            p, global_dirty_head, global_dirty_count, hash,
                            valid_pages, account,
                        );
                        detached = true;
                    }
                    unlockb(blk, bflags);
                    if detached && (*p).valid == 0 {
                        ref_set(p, 1);
                        a20_pagecache_lru_remove(p);
                        a20_pagecache_lru_insert_front(lru_head, p);
                        scan(account, visited);
                        return p;
                    }
                    p = next;
                    continue;
                }
                // Free descriptor: not in any hash chain, no bucket lock needed.
                ref_set(p, 1);
                a20_pagecache_lru_remove(p);
                a20_pagecache_lru_insert_front(lru_head, p);
                scan(account, visited);
                return p;
            }
            p = next;
        }
        scan(account, visited);
        core::ptr::null_mut()
    }
}

// ------------------------------------------------------------------
// clean-page reclaim (OOM path)
// ------------------------------------------------------------------

/// Reclaim one clean, unpinned page found by the OOM scan.  Applies the outer
/// reclaimability predicate, re-checks under the bucket lock, detaches and
/// pushes the descriptor back onto the free stack.  Returns the vnode whose
/// reference the caller must drop after releasing the spinlock, or null when
/// the page is not reclaimable.  Mirrors one iteration of
/// `page_cache_drop_clean`.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_reclaim_clean(
    p: *mut PageCachePage,
    lock_bucket: Option<BucketLockFn>,
    unlock_bucket: Option<BucketUnlockFn>,
    pfn_reclaimable: Option<PfnReclaimableFn>,
    hash: *mut *mut PageCachePage,
    global_dirty_head: *mut *mut PageCachePage,
    global_dirty_count: *mut usize,
    valid_pages: *mut usize,
    free_head: *mut *mut PageCachePage,
    account: Option<ScanFn>,
) -> *mut c_void {
    if p.is_null() {
        return core::ptr::null_mut();
    }
    let (lockb, unlockb, reclaimable) = match (lock_bucket, unlock_bucket, pfn_reclaimable) {
        (Some(l), Some(u), Some(r)) => (l, u, r),
        _ => return core::ptr::null_mut(),
    };
    // SAFETY: reads the descriptor under g_page_cache_lock.
    unsafe {
        if (*p).valid == 0 || (*p).dirty != 0 || ref_read(p) != 0 || reclaimable((*p).pfn) == 0 {
            return core::ptr::null_mut();
        }
        let idx = hash_index((*p).vnode as *mut VnodeHead, (*p).index);
        let blk = bucket_of(idx);
        let bflags = lockb(blk);
        let mut vn = core::ptr::null_mut();
        if (*p).valid != 0 && (*p).dirty == 0 && ref_read(p) == 0 && reclaimable((*p).pfn) != 0 {
            vn = a20_pagecache_detach(
                p, global_dirty_head, global_dirty_count, hash, valid_pages, account,
            );
            if !vn.is_null() {
                a20_pagecache_free_insert(free_head, p);
            }
        }
        unlockb(blk, bflags);
        vn
    }
}

// ------------------------------------------------------------------
// writeback
// ------------------------------------------------------------------

/// Collect the next contiguous dirty batch.  With `vn == NULL` the global
/// dirtied-order list nominates the vnode, then that vnode is consumed from its
/// lowest dirty offset so the filesystem sees monotonically increasing logical
/// blocks.  Pins every collected page.  Mirrors `collect_dirty_batch_locked`.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_collect_dirty(
    vn: *mut VnodeHead,
    global_dirty_head: *mut PageCachePage,
    max_pages: usize,
    out: *mut *mut PageCachePage,
    bucket_pinned: *mut usize,
    account: Option<ScanFn>,
) -> usize {
    if out.is_null() || max_pages == 0 {
        return 0;
    }
    // SAFETY: reads the dirty lists under g_page_cache_lock.
    unsafe {
        let mut p = if !vn.is_null() {
            (*vn).cache_dirty_tail
        } else if !global_dirty_head.is_null() && !(*global_dirty_head).vnode.is_null() {
            let vptr = (*global_dirty_head).vnode as *mut VnodeHead;
            (*vptr).cache_dirty_tail
        } else {            core::ptr::null_mut()
        };
        scan(account, if p.is_null() { 0 } else { 1 });
        if p.is_null() {
            return 0;
        }
        let batch_vn = (*p).vnode;
        let mut next_index = (*p).index;
        let mut count = 0usize;
        while !p.is_null()
            && count < max_pages
            && (*p).vnode == batch_vn
            && (*p).index == next_index
        {
            a20_pagecache_pin(p, bucket_pinned);
            *out.add(count) = p;
            count += 1;
            next_index += 1;
            // Per-vnode dirty pages descend from head to tail, so the tail's
            // predecessor is the next higher file offset.
            p = (*p).dirty_prev;
        }
        count
    }
}

/// Clear the dirty bit of every page whose exact dirty generation reached
/// storage.  Mirrors the confirmation loop in `page_cache_writeback_common`.
/// Returns the number of pages cleared.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_writeback_confirm(
    pages: *const *mut PageCachePage,
    count: usize,
    batch_vn: *mut c_void,
    index: u64,
    gens: *const u64,
    global_dirty_head: *mut *mut PageCachePage,
    global_dirty_count: *mut usize,
) -> usize {
    if pages.is_null() || gens.is_null() {
        return 0;
    }
    let mut cleared = 0usize;
    // SAFETY: array bounds and generations are the caller's writeback batch.
    unsafe {
        for i in 0..count {
            let p = *pages.add(i);
            if p.is_null() {
                continue;
            }
            if (*p).valid != 0
                && (*p).vnode == batch_vn
                && (*p).index == index + i as u64
                && load_u64(core::ptr::addr_of!((*p).dirty_gen)) == *gens.add(i)
            {
                a20_pagecache_dirty_remove(p, global_dirty_head, global_dirty_count);
                cleared += 1;
            }
        }
    }
    cleared
}

// ------------------------------------------------------------------
// invalidation / truncate
// ------------------------------------------------------------------

/// Detach every unpinned page of `vn` whose index falls in `[first, last]`.
/// Pass `first = 0, last = u64::MAX` for a whole-file invalidation.  Walks the
/// mapping list, taking each candidate's bucket lock for the atomic recheck.
/// Returns the number of detached pages; the caller drops one vnode reference
/// per detached page.  Mirrors `page_cache_invalidate*_range` /
/// `page_cache_discard_unlinked` (the discard variant only differs in the
/// caller's discarded-dirty accounting).
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_detach_unpinned_range(
    vn: *mut VnodeHead,
    first: u64,
    last: u64,
    require_clean: c_int,
    lock_bucket: Option<BucketLockFn>,
    unlock_bucket: Option<BucketUnlockFn>,
    hash: *mut *mut PageCachePage,
    global_dirty_head: *mut *mut PageCachePage,
    global_dirty_count: *mut usize,
    valid_pages: *mut usize,
    account: Option<ScanFn>,
    out_discarded_dirty: *mut usize,
) -> usize {
    if vn.is_null() {
        return 0;
    }
    let lockb = match lock_bucket {
        Some(f) => f,
        None => return 0,
    };
    let unlockb = match unlock_bucket {
        Some(f) => f,
        None => return 0,
    };
    let mut detached = 0usize;
    let mut visited = 0usize;
    // SAFETY: walks the vnode's mapping list under g_page_cache_lock.
    unsafe {
        let mut p = (*vn).cache_pages;
        while !p.is_null() {
            visited += 1;
            let next = (*p).mapping_next;
            if (*p).index < first || (*p).index > last {
                p = next;
                continue;
            }
            if ref_read(p) == 0 && (require_clean == 0 || (*p).dirty == 0) {
                if !out_discarded_dirty.is_null() && (*p).dirty != 0 {
                    *out_discarded_dirty += 1;
                }
                let idx = hash_index((*p).vnode as *mut VnodeHead, (*p).index);
                let blk = bucket_of(idx);
                let bflags = lockb(blk);
                if (*p).valid != 0
                    && (*p).vnode == vn as *mut c_void
                    && (*p).index >= first
                    && (*p).index <= last
                    && ref_read(p) == 0
                    && (require_clean == 0 || (*p).dirty == 0)
                {
                    a20_pagecache_detach(
                        p, global_dirty_head, global_dirty_count, hash,
                        valid_pages, account,
                    );
                    detached += 1;
                }
                unlockb(blk, bflags);
            }
            p = next;
        }
    }
    scan(account, visited);
    detached
}

/// Truncate the cache to `new_size`.  Pages wholly beyond EOF are detached when
/// unpinned; the partial EOF page keeps its prefix beyond the new size zeroed
/// (or is only marked when a live user mapping pins it); pinned pages beyond
/// EOF are invalidated without detaching.  Returns the number of detached
/// pages.  Mirrors `page_cache_truncate`.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_truncate(
    vn: *mut VnodeHead,
    new_size: u64,
    lock_bucket: Option<BucketLockFn>,
    unlock_bucket: Option<BucketUnlockFn>,
    hash: *mut *mut PageCachePage,
    global_dirty_head: *mut *mut PageCachePage,
    global_dirty_count: *mut usize,
    valid_pages: *mut usize,
    account: Option<ScanFn>,
) -> usize {
    if vn.is_null() {
        return 0;
    }
    let lockb = match lock_bucket {
        Some(f) => f,
        None => return 0,
    };
    let unlockb = match unlock_bucket {
        Some(f) => f,
        None => return 0,
    };
    let eof_index = new_size / 4096;
    let eof_offset = (new_size % 4096) as usize;
    let mut detached = 0usize;
    let mut visited = 0usize;
    // SAFETY: walks the vnode's mapping list under g_page_cache_lock.
    unsafe {
        let mut p = (*vn).cache_pages;
        while !p.is_null() {
            visited += 1;
            let next = (*p).mapping_next;
            if (*p).index < eof_index {
                p = next;
                continue;
            }
            let partial_eof_page = eof_offset != 0 && (*p).index == eof_index;
            let pinned = ref_read(p) != 0;
            let uptodate = (*(core::ptr::addr_of!((*p).uptodate) as *const AtomicI32))
                .load(Ordering::Acquire) != 0;
            if partial_eof_page && ((*p).dirty != 0 || uptodate) {
                if !pinned {
                    core::ptr::write_bytes(
                        ((*p).data as *mut u8).add(eof_offset),
                        0,
                        4096 - eof_offset,
                    );
                }
                (*p).invalidate_gen = (*p).invalidate_gen.wrapping_add(1);
                if (*p).dirty != 0 {
                    add_u64(core::ptr::addr_of_mut!((*p).dirty_gen), 1);
                }
            } else if !pinned {
                let idx = hash_index((*p).vnode as *mut VnodeHead, (*p).index);
                let blk = bucket_of(idx);
                let bflags = lockb(blk);
                if (*p).valid != 0
                    && (*p).vnode == vn as *mut c_void
                    && (*p).index >= eof_index
                    && ref_read(p) == 0
                {
                    a20_pagecache_detach(
                        p, global_dirty_head, global_dirty_count, hash,
                        valid_pages, account,
                    );
                    detached += 1;
                }
                unlockb(blk, bflags);
            } else {
                (*p).invalidate_gen = (*p).invalidate_gen.wrapping_add(1);
                (*(core::ptr::addr_of_mut!((*p).uptodate) as *mut AtomicI32))
                    .store(0, Ordering::Release);
                a20_pagecache_dirty_remove(p, global_dirty_head, global_dirty_count);
            }
            p = next;
        }
    }
    scan(account, visited);
    detached
}

/* ------------------------------------------------------------------ */
/* read/write path decision helpers (pure arithmetic + policy)         */
/* ------------------------------------------------------------------ */

/// Readahead admission and clamp.  Returns 0 to skip (non-regular vnode,
/// zero count, or a start at/after EOF) and 1 to proceed with `*out_count`
/// clamped to the bytes remaining after `start`.
///
/// # Safety
/// `out_count` is writable.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_readahead_plan(
    is_regular: c_int,
    file_size: u64,
    start: u64,
    count: usize,
    out_count: *mut usize,
) -> c_int {
    if out_count.is_null() {
        return 0;
    }
    // SAFETY: out_count is writable by contract.
    unsafe { *out_count = 0 };
    if is_regular == 0 || count == 0 || start >= file_size {
        return 0;
    }
    let mut c = count as u64;
    let remain = file_size - start;
    if c > remain {
        c = remain;
    }
    // SAFETY: out_count is writable by contract.
    unsafe { *out_count = c as usize };
    1
}

/// Write-path page geometry and the read-modify-write precondition for one
/// chunk of a `write(2)`.
///
/// `*out_chunk` is the bytes this page accepts (clamped to what is left),
/// `*out_page_off` the in-page offset and `*out_may_fill` the *static* part of
/// the RMW decision (`page_start < file_size && !full-overwrite`); the caller
/// ANDs that with "the page is not uptodate".  Returns 0 always (the loop
/// terminates when `done` reaches `count`).
///
/// # Safety
/// The out pointers are writable.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_write_plan(
    file_size: u64,
    start: u64,
    done: usize,
    count: usize,
    out_index: *mut u64,
    out_page_off: *mut usize,
    out_chunk: *mut usize,
    out_may_fill: *mut c_int,
) -> c_int {
    if out_index.is_null() || out_page_off.is_null() || out_chunk.is_null()
        || out_may_fill.is_null()
    {
        return -1;
    }
    let pos = start + done as u64;
    let ps = PAGE_SIZE as u64;
    let index = pos / ps;
    let page_start = index * ps;
    let page_off = (pos - page_start) as usize;
    let mut chunk = PAGE_SIZE - page_off;
    let remain = count - done;
    if chunk > remain {
        chunk = remain;
    }
    let full = page_off == 0 && chunk == PAGE_SIZE as usize;
    let may_fill = page_start < file_size && !full;
    // SAFETY: out pointers are writable by contract.
    unsafe {
        *out_index = index;
        *out_page_off = page_off;
        *out_chunk = chunk;
        *out_may_fill = if may_fill { 1 } else { 0 };
    }
    0
}

/// Own the page-cache hit/miss and descriptor-publication transaction. Frame
/// growth/zeroing, vnode references, and the pressure writeback operation are
/// narrow mechanisms; Rust owns lookup, pinning, bounded eviction retries,
/// duplicate suppression, field initialization, and mapping publication.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_get_transaction(
    context: *mut c_void, vn: *mut VnodeHead, index: u64, create: c_int,
    hash: *mut *mut PageCachePage, lru_head: *mut PageCachePage,
    lru_tail: *mut PageCachePage, free_head: *mut *mut PageCachePage,
    global_dirty_head: *mut *mut PageCachePage, global_dirty_count: *mut usize,
    valid_pages: *mut usize, bucket_pinned: *mut usize,
    pressure_batch_pages: usize,
    global_lock: Option<CacheLockFn>, global_unlock: Option<CacheUnlockFn>,
    bucket_lock: Option<CacheBucketLockFn>, bucket_unlock: Option<CacheBucketUnlockFn>,
    grow: Option<CacheGrowFn>, evict: Option<CacheEvictFn>,
    sweep_limit: Option<extern "C" fn(*mut c_void) -> usize>,
    pressure_writeback: Option<CachePressureWritebackFn>,
    zero_page: Option<CachePageZeroFn>, vnode_get: Option<CacheVnodeRefFn>,
    vnode_put: Option<CacheVnodeRefFn>,
    reclaimable: Option<PfnReclaimableFn>, account: Option<ScanFn>,
) -> *mut PageCachePage {
    let (lockg, unlockg, lockb, unlockb, grow, evict, writeback, zero, vget, vput) = match
        (global_lock, global_unlock, bucket_lock, bucket_unlock, grow, evict,
         pressure_writeback, zero_page, vnode_get, vnode_put) {
        (Some(a), Some(b), Some(c), Some(d), Some(e), Some(f), Some(g), Some(h), Some(i), Some(j)) =>
            (a, b, c, d, e, f, g, h, i, j),
        _ => return core::ptr::null_mut(),
    };
    if context.is_null() || vn.is_null() || hash.is_null() || lru_head.is_null()
        || lru_tail.is_null() || free_head.is_null() || global_dirty_head.is_null()
        || global_dirty_count.is_null() || valid_pages.is_null() || bucket_pinned.is_null()
        || reclaimable.is_none() || sweep_limit.is_none() { return core::ptr::null_mut(); }

    unsafe {
        let hash_idx = hash_index(vn, index);
        let bucket = bucket_of(hash_idx);
        {
            let mut bucket_guard = CacheBucketLockGuard::acquire(context, bucket, lockb, unlockb);
            let page = a20_pagecache_find(hash, vn, index, account);
            if !page.is_null() {
                let pin = PagePinGuard::pin(page, bucket_pinned);
                bucket_guard.release();
                return pin.transfer_to_caller();
            }
        }
        if create == 0 { return core::ptr::null_mut(); }

        let mut sweeps = 0usize;
        loop {
            {
                let mut bucket_guard = CacheBucketLockGuard::acquire(context, bucket, lockb, unlockb);
                let page = a20_pagecache_find(hash, vn, index, account);
                if !page.is_null() {
                    let pin = PagePinGuard::pin(page, bucket_pinned);
                    bucket_guard.release();
                    return pin.transfer_to_caller();
                }
            }

            let mut deferred: *mut c_void = core::ptr::null_mut();
            let mut global_guard = CacheGlobalLockGuard::acquire(context, lockg, unlockg);
            {
                let mut bucket_guard = CacheBucketLockGuard::acquire(context, bucket, lockb, unlockb);
                let page = a20_pagecache_find(hash, vn, index, account);
                if !page.is_null() {
                    let pin = PagePinGuard::pin(page, bucket_pinned);
                    bucket_guard.release();
                    global_guard.release();
                    return pin.transfer_to_caller();
                }
            }

            let mut page = a20_pagecache_free_take(free_head, lru_head);
            if page.is_null() {
                global_guard.release();
                if grow(context, 0) == 0 { continue; }

                global_guard = CacheGlobalLockGuard::acquire(context, lockg, unlockg);
                {
                    let mut bucket_guard = CacheBucketLockGuard::acquire(context, bucket, lockb, unlockb);
                    let found = a20_pagecache_find(hash, vn, index, account);
                    if !found.is_null() {
                        let pin = PagePinGuard::pin(found, bucket_pinned);
                        bucket_guard.release();
                        global_guard.release();
                        return pin.transfer_to_caller();
                    }
                }
                page = a20_pagecache_free_take(free_head, lru_head);
            }
            if page.is_null() {
                page = evict(context, &mut deferred);
            }
            let limit = sweep_limit.unwrap()(context);
            global_guard.release();
            if !deferred.is_null() { vput(context, deferred); }
            if page.is_null() {
                sweeps += 1;
                if sweeps < limit { continue; }
                if writeback(context, pressure_batch_pages) > 0 { sweeps = 0; continue; }
                if grow(context, 1) == 0 { sweeps = 0; continue; }
                return core::ptr::null_mut();
            }

            zero(context, page);
            let mut global_guard = CacheGlobalLockGuard::acquire(context, lockg, unlockg);
            {
                let mut bucket_guard = CacheBucketLockGuard::acquire(context, bucket, lockb, unlockb);
                let racer = a20_pagecache_find(hash, vn, index, account);
                if !racer.is_null() {
                    let pin = PagePinGuard::pin(racer, bucket_pinned);
                    bucket_guard.release();
                    a20_pagecache_free_insert(free_head, page);
                    global_guard.release();
                    return pin.transfer_to_caller();
                }
            }

            (*page).vnode = vn as *mut c_void;
            (*page).index = index;
            (*page).valid = 1;
            (*page).dirty = 0;
            (*page).dirty_gen = 0;
            (*page).invalidate_gen = (*page).invalidate_gen.wrapping_add(1);
            (*page).uptodate = 0;
            (*page).accessed = 0;
            vget(context, vn as *mut c_void);
            {
                let _bucket_guard = CacheBucketLockGuard::acquire(context, bucket, lockb, unlockb);
                a20_pagecache_hash_insert(hash, page);
                a20_pagecache_mapping_insert(page);
                *valid_pages = (*valid_pages).wrapping_add(1);
                a20_pagecache_account_pin(page, bucket_pinned);
            }
            global_guard.release();
            return page;
        }
    }
}

/// Own the vfile read loop, page boundaries, partial-error rule and stream
/// hint updates. Filesystem reads, page acquisition, locking and seek ABI are
/// supplied as narrow callbacks.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_read_transaction(
    context: *mut c_void, file_size: usize, start: usize, buffer: *mut u8,
    count: usize, get: Option<FilePageGetFn>, ready: Option<FilePageFn>,
    fill: Option<FilePageFn>, readahead: Option<FileReadAheadFn>,
    resumes: Option<FileReadHintFn>, record_end: Option<FileReadRecordFn>,
    put: Option<FilePagePutFn>, seek: Option<FileSeekFn>,
) -> isize {
    let (get,ready,fill,readahead,resumes,record,put,seek)=match
        (get,ready,fill,readahead,resumes,record_end,put,seek) {
        (Some(a),Some(b),Some(c),Some(d),Some(e),Some(f),Some(g),Some(h)) => (a,b,c,d,e,f,g,h),
        _ => return -22,
    };
    if context.is_null() || (count>0 && buffer.is_null()) { return -22; }
    if count==0 || start>=file_size { return 0; }
    let count=core::cmp::min(count,file_size-start);
    let mut done=0usize;
    while done<count {
        let pos=start+done;
        let index=(pos/ PAGE_SIZE) as u64;
        let page_off=pos%PAGE_SIZE;
        let chunk=core::cmp::min(PAGE_SIZE-page_off,count-done);
        let page=get(context,index,1);
        if page.is_null() { break; }
        if ready(context,page)==0 {
            let r=if resumes(context,index)!=0 { readahead(context,page,file_size) }
                 else { fill(context,page) };
            if r<0 { put(context,page); return if done==0 {r as isize} else {done as isize}; }
        }
        record(context,index);
        core::ptr::copy_nonoverlapping((*page).data.cast::<u8>().add(page_off),buffer.add(done),chunk);
        put(context,page);
        done+=chunk;
    }
    if done>0 { seek(context,(start+done) as u64); }
    done as isize
}

/// Own the buffered write loop and read-modify-write decision. Filesystem fill,
/// fill-lock, vnode-size publication, and syscall seek are mechanical callbacks.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_write_transaction(
    context: *mut c_void, old_size: usize, start: usize,
    buffer: *const u8, count: usize, get: Option<FilePageGetFn>,
    ready: Option<FilePageFn>, fill: Option<FilePageFn>,
    lock_page: Option<FilePageFn>, unlock_page: Option<FilePageFn>,
    mark_dirty: Option<FilePageFn>, zero_page: Option<FilePageFn>,
    mark_uptodate: Option<FilePageFn>,
    put: Option<FilePagePutFn>, seek: Option<FileSeekFn>,
    commit: Option<FileWriteCommitFn>, perf: Option<FilePerfFn>,
) -> isize {
    let (get,ready,fill,lock_page,unlock_page,mark_dirty,zero_page,mark_uptodate,put,seek,commit)=match
        (get,ready,fill,lock_page,unlock_page,mark_dirty,zero_page,mark_uptodate,put,seek,commit) {
        (Some(a),Some(b),Some(c),Some(d),Some(e),Some(f),Some(g),Some(h),Some(i),Some(j),Some(k)) =>
            (a,b,c,d,e,f,g,h,i,j,k), _ => return -22,
    };
    if context.is_null() || (count>0 && buffer.is_null()) { return -22; }
    if count==0 { return 0; }
    let mut done=0usize; let mut updated=0usize;
    while done<count {
        let mut index=0u64; let mut page_off=0usize; let mut chunk=0usize; let mut may_fill=0;
        if a20_pagecache_write_plan(old_size as u64,start as u64,done,count,
            &mut index,&mut page_off,&mut chunk,&mut may_fill)<0 { break; }
        let page=get(context,index,1);
        if page.is_null() { break; }
        if ready(context,page)==0 && may_fill!=0 {
            let r=fill(context,page);
            if r<0 { put(context,page); return if done==0 {r as isize} else {done as isize}; }
        }
        lock_page(context,page);
        if ready(context,page)==0 {
            zero_page(context,page);
            mark_uptodate(context,page);
        }
        core::ptr::copy_nonoverlapping(buffer.add(done),(*page).data.cast::<u8>().add(page_off),chunk);
        // Publish EOF before exposing a dirty page to sync/writeback. Otherwise
        // an extending write can be acknowledged as clean while the filesystem
        // still sees the old EOF and skips the page entirely.
        commit(context,(start+done+chunk) as u64);
        mark_dirty(context,page);
        unlock_page(context,page);
        put(context,page);
        done+=chunk; updated+=1;
    }
    if done>0 { seek(context,(start+done) as u64); }
    if let Some(f)=perf { f(context,updated); }
    done as isize
}

/// Own cache coherence after a filesystem has committed a write. A cached
/// page is created only for a full-page or post-EOF write; partial existing
/// pages are updated only when their old bytes are authoritative.
#[no_mangle]
pub unsafe extern "C" fn a20_pagecache_update_after_write(
    context: *mut c_void, start: u64, old_size: u64, buffer: *const u8, len: usize,
    get: Option<FilePageGetFn>, ready: Option<FilePageFn>,
    lock_page: Option<FilePageFn>, unlock_page: Option<FilePageFn>,
    mark_uptodate: Option<FilePageFn>, mark_clean: Option<FilePageFn>,
    put: Option<FilePagePutFn>, perf: Option<FilePerfFn>,
) {
    let (get,ready,lock_page,unlock_page,mark_uptodate,mark_clean,put,perf)=match
        (get,ready,lock_page,unlock_page,mark_uptodate,mark_clean,put,perf) {
        (Some(a),Some(b),Some(c),Some(d),Some(e),Some(f),Some(g),Some(h)) => (a,b,c,d,e,f,g,h),
        _ => return,
    };
    if context.is_null() || buffer.is_null() || len==0 { return; }
    unsafe {
        let mut done=0usize; let mut updated=0usize;
        while done<len {
            let pos=start.wrapping_add(done as u64);
            let index=pos/PAGE_SIZE as u64;
            let page_start=index*PAGE_SIZE as u64;
            let page_off=(pos-page_start) as usize;
            let chunk=core::cmp::min(PAGE_SIZE-page_off,len-done);
            let full=page_off==0 && chunk==PAGE_SIZE;
            let beyond=page_start>=old_size;
            let mut page=get(context,index,0);
            if page.is_null() && (full || beyond) { page=get(context,index,1); }
            if !page.is_null() {
                lock_page(context,page);
                let authoritative=ready(context,page)!=0;
                if full || beyond || authoritative {
                    if !authoritative && !full {
                        core::ptr::write_bytes((*page).data.cast::<u8>(),0,PAGE_SIZE);
                    }
                    core::ptr::copy_nonoverlapping(buffer.add(done),(*page).data.cast::<u8>().add(page_off),chunk);
                    mark_uptodate(context,page);
                    mark_clean(context,page);
                    updated+=1;
                }
                unlock_page(context,page);
                put(context,page);
            }
            done+=chunk;
        }
        perf(context,updated);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    extern "C" fn scan_stub(_: usize) {}
    extern "C" fn reclaimable_yes(_: u32) -> c_int { 1 }
    extern "C" fn reclaimable_no(_: u32) -> c_int { 0 }
    extern "C" fn lock_stub(_: u32) -> u64 { 0 }
    extern "C" fn unlock_stub(_: u32, _: u64) {}

    #[derive(Default)]
    struct TransactionGuardHarness { global_unlocks: usize, bucket_unlocks: usize }
    extern "C" fn guard_global_lock(_: *mut c_void) -> u64 { 17 }
    extern "C" fn guard_global_unlock(ctx: *mut c_void, flags: u64) {
        assert_eq!(flags, 17);
        unsafe { (*(ctx as *mut TransactionGuardHarness)).global_unlocks += 1; }
    }
    extern "C" fn guard_bucket_lock(_: *mut c_void, bucket: u32) -> u64 {
        u64::from(bucket) + 23
    }
    extern "C" fn guard_bucket_unlock(ctx: *mut c_void, bucket: u32, flags: u64) {
        assert_eq!(flags, u64::from(bucket) + 23);
        unsafe { (*(ctx as *mut TransactionGuardHarness)).bucket_unlocks += 1; }
    }

    #[test]
    fn miss_transaction_guards_release_locks_and_transfer_pins_once() {
        let mut ctx = TransactionGuardHarness::default();
        {
            let _global = CacheGlobalLockGuard::acquire(
                &mut ctx as *mut _ as *mut c_void, guard_global_lock, guard_global_unlock,
            );
            let _bucket = CacheBucketLockGuard::acquire(
                &mut ctx as *mut _ as *mut c_void, 3, guard_bucket_lock, guard_bucket_unlock,
            );
        }
        assert_eq!(ctx.global_unlocks, 1);
        assert_eq!(ctx.bucket_unlocks, 1);

        let mut page = mk();
        let mut counters = [0usize; PAGE_CACHE_BUCKET_LOCKS * (PIN_COUNTER_STRIDE / core::mem::size_of::<usize>())];
        {
            let _pin = unsafe { PagePinGuard::pin(&mut page, counters.as_mut_ptr()) };
            assert_eq!(page.ref_count, 1);
            assert_eq!(page.accessed, 1);
            assert_eq!(unsafe { a20_pagecache_pinned_total(counters.as_ptr()) }, 1);
        }
        assert_eq!(page.ref_count, 0);
        assert_eq!(unsafe { a20_pagecache_pinned_total(counters.as_ptr()) }, 0);

        let pin = unsafe { PagePinGuard::pin(&mut page, counters.as_mut_ptr()) };
        let transferred = pin.transfer_to_caller();
        assert_eq!(transferred, &mut page as *mut _);
        assert_eq!(page.ref_count, 1);
        assert_eq!(unsafe { a20_pagecache_put(transferred, counters.as_mut_ptr()) }, 1);
        assert_eq!(unsafe { a20_pagecache_pinned_total(counters.as_ptr()) }, 0);
    }

    fn mk() -> PageCachePage {
        PageCachePage {
            vnode: core::ptr::null_mut(),
            index: 0,
            ref_count: 0,
            valid: 0,
            dirty: 0,
            dirty_gen: 0,
            invalidate_gen: 0,
            uptodate: 0,
            accessed: 0,
            pfn: 0,
            data: core::ptr::null_mut(),
            prev: core::ptr::null_mut(),
            next: core::ptr::null_mut(),
            hnext: core::ptr::null_mut(),
            mapping_prev: core::ptr::null_mut(),
            mapping_next: core::ptr::null_mut(),
            dirty_prev: core::ptr::null_mut(),
            dirty_next: core::ptr::null_mut(),
            global_dirty_prev: core::ptr::null_mut(),
            global_dirty_next: core::ptr::null_mut(),
            dirty_children: [core::ptr::null_mut(); 2], dirty_parent_color: 0,
        }
    }

    fn mk_vn() -> VnodeHead {
        VnodeHead {
            ino: 0,
            vtype: 0,
            mode: 0,
            uid: 0,
            gid: 0,
            size: 0,
            ref_count: 0,
            parent: core::ptr::null_mut(),
            mnt: core::ptr::null_mut(),
            fs_data: core::ptr::null_mut(),
            ops: core::ptr::null_mut(),
            cache_pages: core::ptr::null_mut(),
            cache_dirty_pages: core::ptr::null_mut(),
            cache_dirty_tail: core::ptr::null_mut(),
            cache_dirty_root: core::ptr::null_mut(),
        }
    }

    struct FillCtx { locks: usize, unlocks: usize, reads: usize, buffer: Vec<u8> }
    extern "C" fn fill_lock(ctx: *mut c_void, _: *mut PageCachePage) {
        unsafe { (*(ctx as *mut FillCtx)).locks += 1; }
    }
    extern "C" fn fill_unlock(ctx: *mut c_void, _: *mut PageCachePage) {
        unsafe { (*(ctx as *mut FillCtx)).unlocks += 1; }
    }
    extern "C" fn fill_ready(_: *mut c_void, page: *mut PageCachePage) -> c_int {
        unsafe { (*page).uptodate }
    }
    extern "C" fn fill_generation(_: *mut c_void, page: *mut PageCachePage) -> u64 {
        unsafe { (*page).invalidate_gen }
    }
    extern "C" fn fill_publish(_: *mut c_void, page: *mut PageCachePage, gen: u64) -> c_int {
        unsafe {
            if (*page).invalidate_gen != gen { return 0; }
            (*page).uptodate = 1;
            1
        }
    }
    extern "C" fn fill_read_one(_: *mut c_void, _: *mut PageCachePage, data: *mut u8) -> c_int {
        unsafe { core::ptr::write_bytes(data, 0x5a, 9); }
        9
    }
    extern "C" fn fill_read_many(ctx: *mut c_void, _: u64, data: *mut u8, len: usize) -> c_int {
        unsafe {
            let state = &mut *(ctx as *mut FillCtx);
            state.reads += 1;
            core::ptr::write_bytes(data, 0x33, len);
        }
        (len - 2) as c_int
    }
    extern "C" fn fill_take(ctx: *mut c_void, _: usize) -> *mut u8 {
        unsafe { (*(ctx as *mut FillCtx)).buffer.as_mut_ptr() }
    }
    extern "C" fn fill_return(_: *mut c_void, _: *mut u8) {}

    struct WindowCtx { pages: [PageCachePage; 2], gets: usize, puts: usize, filled: usize, perf: usize }
    extern "C" fn window_get(ctx: *mut c_void, index: u64, _: c_int) -> *mut PageCachePage {
        unsafe {
            let state = &mut *(ctx as *mut WindowCtx);
            state.gets += 1;
            if index == state.pages[0].index + 1 { &mut state.pages[1] }
            else { core::ptr::null_mut() }
        }
    }
    extern "C" fn window_fill(ctx: *mut c_void, _: *mut *mut PageCachePage, count: usize) -> c_int {
        unsafe { (*(ctx as *mut WindowCtx)).filled = count; }
        0
    }
    extern "C" fn window_put(ctx: *mut c_void, _: *mut PageCachePage) {
        unsafe { (*(ctx as *mut WindowCtx)).puts += 1; }
    }
    extern "C" fn window_perf(ctx: *mut c_void, count: usize) {
        unsafe { (*(ctx as *mut WindowCtx)).perf = count; }
    }

    #[test]
    fn fill_transactions_publish_and_zero_short_reads_with_balanced_locks() {
        unsafe {
            let mut vnode_marker = 0u8;
            let vnode = &mut vnode_marker as *mut _ as *mut c_void;
            let mut one_data = [0xaau8; PAGE_SIZE];
            let mut one = mk();
            one.vnode = vnode;
            one.data = one_data.as_mut_ptr() as *mut c_void;
            let mut ctx = FillCtx { locks: 0, unlocks: 0, reads: 0, buffer: Vec::new() };
            let one_result = a20_pagecache_fill_one_transaction(
                &mut ctx as *mut _ as *mut c_void, &mut one,
                Some(fill_lock), Some(fill_unlock), Some(fill_ready),
                Some(fill_generation), Some(fill_publish), Some(fill_read_one));
            assert_eq!(one_result, 9);
            assert_eq!(one.uptodate, 1);
            assert!(one_data[..9].iter().all(|v| *v == 0x5a));
            assert!(one_data[9..].iter().all(|v| *v == 0));

            let mut data0 = [0u8; PAGE_SIZE];
            let mut data1 = [0u8; PAGE_SIZE];
            let mut pages_storage = [mk(), mk()];
            for (i, page) in pages_storage.iter_mut().enumerate() {
                page.vnode = vnode;
                page.index = i as u64;
                page.data = if i == 0 { data0.as_mut_ptr() } else { data1.as_mut_ptr() } as *mut c_void;
            }
            let mut pages = [
                &mut pages_storage[0] as *mut PageCachePage,
                &mut pages_storage[1] as *mut PageCachePage,
            ];
            ctx.buffer.resize(2 * PAGE_SIZE, 0);
            ctx.locks = 0;
            ctx.unlocks = 0;
            let many_result = a20_pagecache_fill_many_transaction(
                &mut ctx as *mut _ as *mut c_void, pages.as_mut_ptr(), pages.len(), 32,
                Some(fill_lock), Some(fill_unlock), Some(fill_ready),
                Some(fill_generation), Some(fill_publish), Some(fill_read_one),
                Some(fill_read_many), Some(fill_take), Some(fill_return));
            assert_eq!(many_result, 0);
            assert_eq!(ctx.reads, 1);
            assert_eq!((ctx.locks, ctx.unlocks), (2, 2));
            assert!(data0.iter().all(|v| *v == 0x33));
            assert!(data1[..PAGE_SIZE - 2].iter().all(|v| *v == 0x33));
            assert!(data1[PAGE_SIZE - 2..].iter().all(|v| *v == 0));
            assert!(pages_storage.iter().all(|p| p.uptodate == 1));
        }
    }

    #[test]
    fn readahead_transaction_owns_window_pins_and_releases_extra_pages() {
        unsafe {
            let mut state = WindowCtx { pages: [mk(), mk()], gets: 0, puts: 0, filled: 0, perf: 0 };
            state.pages[0].index = 3;
            state.pages[1].index = 4;
            let first = &mut state.pages[0] as *mut PageCachePage;
            let result = a20_pagecache_readahead_transaction(
                &mut state as *mut _ as *mut c_void, first, 5 * PAGE_SIZE, 8,
                Some(window_get), Some(window_fill), Some(window_put), Some(window_perf));
            assert_eq!(result, 0);
            assert_eq!((state.gets, state.puts, state.filled, state.perf), (1, 1, 2, 2));
        }
    }

    struct FileHarness {
        page: *mut PageCachePage,
        puts: usize,
        fills: usize,
        records: usize,
        seek: u64,
        locked: usize,
        unlocked: usize,
        commits: usize,
        perf_pages: usize,
    }
    extern "C" fn file_get(ctx:*mut c_void,_:u64,_:c_int)->*mut PageCachePage {
        unsafe { (*(ctx as *mut FileHarness)).page }
    }
    extern "C" fn file_ready(_: *mut c_void,p:*mut PageCachePage)->c_int {
        unsafe { (*p).uptodate }
    }
    extern "C" fn file_fill(ctx:*mut c_void,p:*mut PageCachePage)->c_int {
        unsafe { (*(ctx as *mut FileHarness)).fills+=1; (*p).uptodate=1; }
        0
    }
    extern "C" fn file_readahead(_: *mut c_void,_:*mut PageCachePage,_:usize)->c_int { 0 }
    extern "C" fn file_resumes(_: *mut c_void,_:u64)->c_int { 0 }
    extern "C" fn file_record(ctx:*mut c_void,_:u64) { unsafe { (*(ctx as *mut FileHarness)).records+=1; } }
    extern "C" fn file_put(ctx:*mut c_void,_:*mut PageCachePage) { unsafe { (*(ctx as *mut FileHarness)).puts+=1; } }
    extern "C" fn file_seek(ctx:*mut c_void,p:u64) { unsafe { (*(ctx as *mut FileHarness)).seek=p; } }
    extern "C" fn file_lock(ctx:*mut c_void,_:*mut PageCachePage)->c_int { unsafe { (*(ctx as *mut FileHarness)).locked+=1; } 0 }
    extern "C" fn file_unlock(ctx:*mut c_void,_:*mut PageCachePage)->c_int { unsafe { (*(ctx as *mut FileHarness)).unlocked+=1; } 0 }
    extern "C" fn file_dirty(ctx: *mut c_void,p:*mut PageCachePage)->c_int {
        unsafe {
            let h=&*(ctx as *mut FileHarness);
            assert_eq!(h.commits, h.locked, "each dirty page needs its EOF published first");
            assert!(h.locked > h.unlocked, "EOF and dirty must publish under the page lock");
            (*p).dirty=1; (*p).dirty_gen+=1;
        }
        0
    }
    extern "C" fn file_zero(_: *mut c_void,p:*mut PageCachePage)->c_int { unsafe { core::ptr::write_bytes((*p).data.cast::<u8>(),0,PAGE_SIZE); } 0 }
    extern "C" fn file_uptodate(_: *mut c_void,p:*mut PageCachePage)->c_int { unsafe { (*p).uptodate=1; } 0 }
    extern "C" fn file_clean(_: *mut c_void,p:*mut PageCachePage)->c_int { unsafe { (*p).dirty=0; } 0 }
    extern "C" fn file_commit(ctx:*mut c_void,_:u64) { unsafe { (*(ctx as *mut FileHarness)).commits+=1; } }
    extern "C" fn file_perf(ctx:*mut c_void,p:usize) { unsafe { (*(ctx as *mut FileHarness)).perf_pages=p; } }

    #[test]
    fn read_transaction_copies_pages_and_preserves_stream_position() {
        unsafe {
            let mut data=[0xacu8;PAGE_SIZE];
            let mut page=mk(); page.valid=1; page.uptodate=1; page.data=data.as_mut_ptr().cast();
            let mut h=FileHarness { page:&mut page, puts:0,fills:0,records:0,seek:0,locked:0,unlocked:0,commits:0,perf_pages:0 };
            let mut out=[0u8;6];
            let n=a20_pagecache_read_transaction(&mut h as *mut _ as *mut c_void,100,2,out.as_mut_ptr(),out.len(),
                Some(file_get),Some(file_ready),Some(file_fill),Some(file_readahead),Some(file_resumes),Some(file_record),Some(file_put),Some(file_seek));
            assert_eq!(n,6); assert_eq!(out,[0xac;6]);
            assert_eq!((h.puts,h.records,h.seek),(1,1,8));
        }
    }

    #[test]
    fn write_transaction_zero_fills_new_page_and_commits_partial_progress() {
        unsafe {
            let mut data=[0x55u8;PAGE_SIZE];
            let mut page=mk(); page.valid=1; page.data=data.as_mut_ptr().cast();
            let mut h=FileHarness { page:&mut page, puts:0,fills:0,records:0,seek:0,locked:0,unlocked:0,commits:0,perf_pages:0 };
            let input=*b"hello";
            let n=a20_pagecache_write_transaction(&mut h as *mut _ as *mut c_void,0,0,input.as_ptr(),input.len(),
                Some(file_get),Some(file_ready),Some(file_fill),Some(file_lock),Some(file_unlock),Some(file_dirty),Some(file_zero),Some(file_uptodate),Some(file_put),Some(file_seek),Some(file_commit),Some(file_perf));
            assert_eq!(n,5); assert_eq!(&data[..5],b"hello"); assert!(data[5..].iter().all(|b|*b==0));
            assert_eq!((h.puts,h.locked,h.unlocked,h.seek,h.commits,h.perf_pages),(1,1,1,5,1,1));
            assert_eq!(page.dirty,1);
        }
    }

    #[test]
    fn write_through_update_only_creates_pages_for_post_eof_data() {
        unsafe {
            let mut data=[0x55u8;PAGE_SIZE];
            let mut page=mk(); page.data=data.as_mut_ptr().cast();
            let mut h=FileHarness { page:&mut page, puts:0,fills:0,records:0,seek:0,locked:0,unlocked:0,commits:0,perf_pages:0 };
            let input=*b"xy";
            a20_pagecache_update_after_write(&mut h as *mut _ as *mut c_void,4096,0,input.as_ptr(),input.len(),
                Some(file_get),Some(file_ready),Some(file_lock),Some(file_unlock),Some(file_uptodate),Some(file_clean),Some(file_put),Some(file_perf));
            assert_eq!(&data[..2],b"xy"); assert!(data[2..].iter().all(|b|*b==0));
            assert_eq!((h.puts,h.locked,h.unlocked,h.perf_pages),(1,1,1,1));
            assert_eq!(page.uptodate,1); assert_eq!(page.dirty,0);
        }
    }

    static TXN_COLLECTED: AtomicUsize = AtomicUsize::new(0);
    static TXN_LOCKED: AtomicUsize = AtomicUsize::new(0);
    static TXN_UNLOCKED: AtomicUsize = AtomicUsize::new(0);
    static TXN_WRITTEN: AtomicUsize = AtomicUsize::new(0);
    static TXN_CONFIRMED: AtomicUsize = AtomicUsize::new(0);
    static TXN_PUT: AtomicUsize = AtomicUsize::new(0);
    static TXN_SERIAL: AtomicUsize = AtomicUsize::new(0);
    static TXN_VNODE_GATE: AtomicUsize = AtomicUsize::new(0);
    static TXN_TARGET: AtomicUsize = AtomicUsize::new(0);
    static TXN_PERF: AtomicUsize = AtomicUsize::new(0);
    static TXN_RESULT: AtomicI32 = AtomicI32::new(0);
    static mut TXN_PAGE: PageCachePage = PageCachePage {
        vnode: core::ptr::null_mut(), index: 0, ref_count: 1, valid: 1,
        dirty: 1, dirty_gen: 1, invalidate_gen: 0, uptodate: 1, accessed: 0,
        pfn: 0, data: core::ptr::null_mut(), prev: core::ptr::null_mut(),
        next: core::ptr::null_mut(), hnext: core::ptr::null_mut(),
        mapping_prev: core::ptr::null_mut(), mapping_next: core::ptr::null_mut(),
        dirty_prev: core::ptr::null_mut(), dirty_next: core::ptr::null_mut(),
        global_dirty_prev: core::ptr::null_mut(), global_dirty_next: core::ptr::null_mut(),
            dirty_children: [core::ptr::null_mut(); 2], dirty_parent_color: 0,
    };

    extern "C" fn txn_collect(
        vn: *mut VnodeHead, out: *mut *mut PageCachePage, max: usize,
    ) -> usize {
        if max == 0 || TXN_COLLECTED.fetch_add(1, Ordering::AcqRel) != 0 {
            return 0;
        }
        unsafe {
            let p = core::ptr::addr_of_mut!(TXN_PAGE);
            (*p).vnode = vn as *mut c_void;
            (*p).index = 7;
            (*out) = p;
        }
        1
    }
    extern "C" fn txn_lock(_: *mut *mut PageCachePage, _: usize) {
        assert_eq!(TXN_VNODE_GATE.load(Ordering::Acquire), 1);
        TXN_LOCKED.fetch_add(1, Ordering::AcqRel);
    }
    extern "C" fn txn_unlock(_: *mut *mut PageCachePage, _: usize) {
        TXN_UNLOCKED.fetch_add(1, Ordering::AcqRel);
    }
    extern "C" fn txn_no_batch(_: *mut VnodeHead, _: usize) -> c_int { 0 }
    extern "C" fn txn_write(
        _: *mut VnodeHead, _: u64, _: *mut *mut PageCachePage, _: usize,
        _: Option<LegacyWritepageFn>, _: *mut c_void, _: *mut c_void,
    ) -> c_int {
        TXN_WRITTEN.fetch_add(1, Ordering::AcqRel);
        TXN_RESULT.load(Ordering::Acquire)
    }
    extern "C" fn txn_alloc(_: usize) -> *mut c_void { core::ptr::null_mut() }
    extern "C" fn txn_free(_: *mut c_void) {}
    extern "C" fn txn_confirm(
        _: *mut *mut PageCachePage, _: usize, _: *mut VnodeHead, _: u64, _: *const u64,
    ) { TXN_CONFIRMED.fetch_add(1, Ordering::AcqRel); }
    extern "C" fn txn_put(_: *mut PageCachePage) {
        TXN_PUT.fetch_add(1, Ordering::AcqRel);
    }
    extern "C" fn txn_serial_lock(vn: *mut VnodeHead) {
        TXN_SERIAL.fetch_add(1, Ordering::AcqRel);
        if !vn.is_null() { TXN_VNODE_GATE.fetch_add(1, Ordering::AcqRel); }
    }
    extern "C" fn txn_serial_unlock(vn: *mut VnodeHead) {
        if !vn.is_null() { TXN_VNODE_GATE.fetch_sub(1, Ordering::AcqRel); }
        TXN_SERIAL.fetch_sub(1, Ordering::AcqRel);
    }
    extern "C" fn txn_perf(count: usize) {
        TXN_PERF.fetch_add(count, Ordering::AcqRel);
    }

    fn reset_txn(result: c_int) {
        for counter in [&TXN_COLLECTED, &TXN_LOCKED, &TXN_UNLOCKED,
                        &TXN_WRITTEN, &TXN_CONFIRMED, &TXN_PUT,
                        &TXN_SERIAL, &TXN_VNODE_GATE, &TXN_PERF] {
            counter.store(0, Ordering::Release);
        }
        TXN_RESULT.store(result, Ordering::Release);
    }

    #[test]
    fn writeback_transaction_releases_once_and_confirms_only_success() {
        let mut vn = mk_vn();
        let mut written = usize::MAX;
        reset_txn(-5);
        let failed = unsafe {
            a20_pagecache_writeback_transaction(
                &mut vn, None, core::ptr::null_mut(), 0, &mut written,
                Some(txn_collect), Some(txn_lock), Some(txn_unlock),
                Some(txn_no_batch), Some(txn_write), Some(txn_alloc),
                Some(txn_free), Some(txn_confirm),
                Some(txn_put), Some(txn_serial_lock), Some(txn_serial_unlock),
                Some(txn_perf),
            )
        };
        assert_eq!(failed, -5);
        assert_eq!(TXN_LOCKED.load(Ordering::Acquire), 1);
        assert_eq!(TXN_UNLOCKED.load(Ordering::Acquire), 1);
        assert_eq!(TXN_PUT.load(Ordering::Acquire), 1);
        assert_eq!(TXN_CONFIRMED.load(Ordering::Acquire), 0);
        assert_eq!(TXN_SERIAL.load(Ordering::Acquire), 0);
        assert_eq!(written, 0);

        // A positive write callback result is a successful write length, not
        // the writeback API's return value.
        reset_txn(4096);
        written = usize::MAX;
        let success = unsafe {
            a20_pagecache_writeback_transaction(
                &mut vn, None, core::ptr::null_mut(), 0, &mut written,
                Some(txn_collect), Some(txn_lock), Some(txn_unlock),
                Some(txn_no_batch), Some(txn_write), Some(txn_alloc),
                Some(txn_free), Some(txn_confirm),
                Some(txn_put), Some(txn_serial_lock), Some(txn_serial_unlock),
                Some(txn_perf),
            )
        };
        assert_eq!(success, 0);
        assert_eq!(TXN_CONFIRMED.load(Ordering::Acquire), 1);
        assert_eq!(TXN_PUT.load(Ordering::Acquire), 1);
        assert_eq!(TXN_PERF.load(Ordering::Acquire), 1);
        assert_eq!(TXN_SERIAL.load(Ordering::Acquire), 0);
        assert_eq!(written, 1);

        // A global flush must acquire the same vnode gate as fsync before
        // locking pages or publishing an inode, including its error path.
        extern "C" fn collect_global(
            _: *mut VnodeHead, out: *mut *mut PageCachePage, max: usize,
        ) -> usize {
            txn_collect(TXN_TARGET.load(Ordering::Acquire) as *mut VnodeHead, out, max)
        }
        TXN_TARGET.store(&mut vn as *mut VnodeHead as usize, Ordering::Release);
        for error in [0, -5] {
            reset_txn(error);
            let result = unsafe {
                a20_pagecache_writeback_transaction(
                    core::ptr::null_mut(), None, core::ptr::null_mut(), 0, &mut written,
                    Some(collect_global), Some(txn_lock), Some(txn_unlock),
                    Some(txn_no_batch), Some(txn_write), Some(txn_alloc),
                    Some(txn_free), Some(txn_confirm), Some(txn_put),
                    Some(txn_serial_lock), Some(txn_serial_unlock), Some(txn_perf),
                )
            };
            assert_eq!(result, error);
            assert_eq!(TXN_SERIAL.load(Ordering::Acquire), 0);
            assert_eq!(TXN_VNODE_GATE.load(Ordering::Acquire), 0);
        }
    }

    #[test]
    fn layout_prefix_matches_c() {
        // LP64 offsets asserted in C as well; these pin the Rust mirror.
        if core::mem::size_of::<usize>() == 8 {
            assert_eq!(core::mem::offset_of!(PageCachePage, index), 8);
            assert_eq!(core::mem::offset_of!(PageCachePage, accessed), 52);
            assert_eq!(core::mem::offset_of!(PageCachePage, pfn), 56);
            assert_eq!(core::mem::offset_of!(PageCachePage, data), 64);
            assert_eq!(core::mem::offset_of!(PageCachePage, prev), 72);
            assert_eq!(core::mem::offset_of!(PageCachePage, global_dirty_next), 136);
            assert_eq!(core::mem::offset_of!(PageCachePage, dirty_children), 144);
            assert_eq!(core::mem::offset_of!(PageCachePage, dirty_parent_color), 160);
            assert_eq!(core::mem::size_of::<PageCachePage>(), 168);
            assert_eq!(core::mem::offset_of!(VnodeHead, mnt), 48);
            assert_eq!(core::mem::offset_of!(VnodeHead, cache_pages), 72);
            assert_eq!(core::mem::offset_of!(VnodeHead, cache_dirty_tail), 88);
            assert_eq!(core::mem::offset_of!(VnodeHead, cache_dirty_root), 96);
        }
    }

    #[test]
    fn hash_key_masks_and_is_stable() {
        let mut vn = mk_vn();
        vn.ino = 7;
        assert_eq!(unsafe { a20_pagecache_hash_key(&mut vn, 0) } as usize & !(PAGE_CACHE_HASH_BUCKETS - 1), 0);
        for i in 0..64u64 {
            let k = unsafe { a20_pagecache_hash_key(&mut vn, i) } as usize;
            assert!(k < PAGE_CACHE_HASH_BUCKETS);
        }
        let knull = unsafe { a20_pagecache_hash_key(core::ptr::null_mut(), 3) } as usize;
        assert!(knull < PAGE_CACHE_HASH_BUCKETS);
    }

    #[test]
    fn free_take_is_lifo_and_publishes_pin() {
        unsafe {
            let mut pool = [mk(), mk(), mk()];
            let mut head_lru = mk();
            let mut tail_lru = mk();
            head_lru.next = &mut tail_lru as *mut _;
            tail_lru.prev = &mut head_lru as *mut _;
            let mut free: *mut PageCachePage = core::ptr::null_mut();
            for i in 0..3 {
                a20_pagecache_lru_insert_tail(&mut tail_lru, &mut pool[i]);
                a20_pagecache_free_insert(&mut free, &mut pool[i]);
            }
            let head = &mut head_lru as *mut _;
            let a = a20_pagecache_free_take(&mut free, head);
            assert_eq!(a, &mut pool[2] as *mut _);
            assert_eq!((*a).ref_count, 1);
            assert_eq!(head_lru.next, a);
            let b = a20_pagecache_free_take(&mut free, head);
            assert_eq!(b, &mut pool[1] as *mut _);
            let _ = a20_pagecache_free_take(&mut free, head);
            assert!(a20_pagecache_free_take(&mut free, head).is_null());
        }
    }

    // Structural checks are independent of the rotation implementation. The
    // C differential fixture separately compares all list/writeback semantics.
    unsafe fn check_dirty_tree(vn: &VnodeHead, expected: usize) {
        use std::collections::BTreeSet;
        unsafe fn walk(p: *mut PageCachePage, parent: *mut PageCachePage,
                       seen: &mut BTreeSet<usize>, keys: &mut Vec<u64>) -> usize {
            if p.is_null() { return 1; }
            assert!(seen.insert(p as usize), "cycle/duplicate tree membership");
            assert_eq!(dirty_parent(p), parent);
            assert_ne!((*p).dirty, 0);
            if dirty_red(p) {
                assert!(!dirty_red((*p).dirty_children[0]));
                assert!(!dirty_red((*p).dirty_children[1]));
            }
            let left = walk((*p).dirty_children[0], p, seen, keys);
            keys.push((*p).index);
            let right = walk((*p).dirty_children[1], p, seen, keys);
            assert_eq!(left, right, "black height");
            left + (!dirty_red(p)) as usize
        }
        assert!(!dirty_red(vn.cache_dirty_root));
        let mut seen = BTreeSet::new();
        let mut keys = Vec::new();
        walk(vn.cache_dirty_root, core::ptr::null_mut(), &mut seen, &mut keys);
        assert_eq!(seen.len(), expected);
        assert!(keys.windows(2).all(|p| p[0] <= p[1]));
        let mut p = vn.cache_dirty_pages;
        let mut prev = core::ptr::null_mut();
        let mut count = 0;
        while !p.is_null() {
            assert!(seen.remove(&(p as usize)), "list membership differs");
            assert_eq!((*p).dirty_prev, prev);
            assert_eq!((*p).index, keys[expected - count - 1]);
            prev = p;
            p = (*p).dirty_next;
            count += 1;
        }
        assert_eq!(prev, vn.cache_dirty_tail);
        assert_eq!(count, expected);
        assert!(seen.is_empty());
    }

    #[test]
    fn dirty_rb_random_reinsert_and_arbitrary_removal() {
        unsafe {
            let mut vn = mk_vn();
            let mut pages: Vec<_> = (0..257).map(|_| mk()).collect();
            let mut head = core::ptr::null_mut();
            let mut count = 0;
            let mut state = 0x123456789abcdefu64;
            for (i, p) in pages.iter_mut().enumerate() {
                p.vnode = &mut vn as *mut _ as *mut c_void;
                p.index = if i == 256 { u64::MAX } else { i as u64 * 0x100000001 };
                p.valid = 1;
            }
            for _ in 0..30000 {
                state ^= state << 13; state ^= state >> 7; state ^= state << 17;
                let i = state as usize % pages.len();
                let p = &mut pages[i];
                if p.dirty == 0 {
                    // Removed descriptors may be reused at another index;
                    // include duplicates and both u64 endpoints as well.
                    p.index = match (state >> 16) % 4 {
                        0 => 0, 1 => u64::MAX, 2 => (state >> 24) % 16,
                        _ => state,
                    };
                    a20_pagecache_dirty_insert(p, &mut head, &mut count);
                    // Idempotence: already dirty does not relink either index.
                    a20_pagecache_dirty_insert(p, &mut head, &mut count);
                } else {
                    a20_pagecache_dirty_remove(p, &mut head, &mut count);
                    assert_eq!(p.dirty_parent_color, 0);
                    assert!(p.dirty_children.iter().all(|p| p.is_null()));
                    a20_pagecache_dirty_remove(p, &mut head, &mut count);
                }
                check_dirty_tree(&vn, count);
            }
            while !vn.cache_dirty_root.is_null() {
                a20_pagecache_dirty_remove(vn.cache_dirty_root, &mut head, &mut count);
                check_dirty_tree(&vn, count);
            }
            assert!(head.is_null());
        }
    }

    #[test]
    fn dirty_rb_large_ordered_reverse_and_interleaved() {
        unsafe {
            for order in 0..3 {
                let mut vn = mk_vn();
                let mut pages: Vec<_> = (0..16384).map(|_| mk()).collect();
                let mut head = core::ptr::null_mut();
                let mut count = 0;
                for i in 0..pages.len() {
                    let at = match order { 0 => i, 1 => pages.len() - i - 1,
                                           _ => (i * 8191) % pages.len() };
                    let p = &mut pages[at];
                    p.vnode = &mut vn as *mut _ as *mut c_void;
                    p.index = at as u64;
                    p.valid = 1;
                    a20_pagecache_dirty_insert(p, &mut head, &mut count);
                    if i % 512 == 0 { check_dirty_tree(&vn, count); }
                }
                check_dirty_tree(&vn, count);
                for i in 0..pages.len() {
                    // Alternate min, max and root, including successor swaps.
                    let p = match i % 3 { 0 => vn.cache_dirty_pages,
                                         1 => vn.cache_dirty_tail,
                                         _ => vn.cache_dirty_root };
                    a20_pagecache_dirty_remove(p, &mut head, &mut count);
                    if i % 512 == 0 { check_dirty_tree(&vn, count); }
                }
                check_dirty_tree(&vn, 0);
                assert!(head.is_null());
            }
        }
    }

    #[test]
    fn dirty_lists_stay_sorted_descending() {
        unsafe {
            let mut vn = mk_vn();
            let mut pages = [mk(), mk(), mk()];
            let mut ghead: *mut PageCachePage = core::ptr::null_mut();
            let mut gcount = 0usize;
            for (i, idx) in [5u64, 2, 9].iter().enumerate() {
                pages[i].vnode = &mut vn as *mut _ as *mut c_void;
                pages[i].index = *idx;
                pages[i].valid = 1;
                a20_pagecache_dirty_insert(&mut pages[i], &mut ghead, &mut gcount);
            }
            assert_eq!(gcount, 3);
            assert_eq!(vn.cache_dirty_pages, &mut pages[2] as *mut _); // 9
            assert_eq!(vn.cache_dirty_tail, &mut pages[1] as *mut _);  // 2
            assert_eq!((*vn.cache_dirty_pages).dirty_next, &mut pages[0] as *mut _); // 5
            a20_pagecache_dirty_remove(&mut pages[0], &mut ghead, &mut gcount);
            assert_eq!(gcount, 2);
            assert_eq!((*vn.cache_dirty_pages).dirty_next, &mut pages[1] as *mut _);
            a20_pagecache_dirty_remove(&mut pages[2], &mut ghead, &mut gcount);
            a20_pagecache_dirty_remove(&mut pages[1], &mut ghead, &mut gcount);
            assert!(ghead.is_null());
            assert_eq!(gcount, 0);
            assert!(vn.cache_dirty_pages.is_null());
            assert!(vn.cache_dirty_tail.is_null());
        }
    }

    #[test]
    fn detach_clears_and_bumps_generation() {
        unsafe {
            let mut vn = mk_vn();
            let mut p = mk();
            p.vnode = &mut vn as *mut _ as *mut c_void;
            p.index = 4;
            p.valid = 1;
            p.uptodate = 1;
            let mut hash = vec![core::ptr::null_mut::<PageCachePage>(); PAGE_CACHE_HASH_BUCKETS];
            let mut ghead: *mut PageCachePage = core::ptr::null_mut();
            let mut gcount = 0usize;
            let mut valid_pages = 1usize;
            a20_pagecache_hash_insert(hash.as_mut_ptr(), &mut p);
            a20_pagecache_mapping_insert(&mut p);
            a20_pagecache_dirty_insert(&mut p, &mut ghead, &mut gcount);
            let vn_back = a20_pagecache_detach(
                &mut p, &mut ghead, &mut gcount, hash.as_mut_ptr(), &mut valid_pages,
                Some(scan_stub as ScanFn),
            );
            assert_eq!(vn_back, &mut vn as *mut _ as *mut c_void);
            assert_eq!(p.valid, 0);
            assert_eq!(p.invalidate_gen, 1);
            assert_eq!(p.uptodate, 0);
            assert_eq!(valid_pages, 0);
            assert!(vn.cache_pages.is_null());
            assert!(vn.cache_dirty_pages.is_null());
            assert_eq!(gcount, 0);
        }
    }

    struct GetHarness {
        head: *mut PageCachePage, tail: *mut PageCachePage,
        hash: *mut *mut PageCachePage, dirty: *mut *mut PageCachePage,
        dirty_count: *mut usize, valid: *mut usize,
        sweeps: usize, limit: usize, writebacks: usize, gets: usize, puts: usize,
    }
    extern "C" fn get_lock(_: *mut c_void) -> u64 { 0 }
    extern "C" fn get_unlock(_: *mut c_void, _: u64) {}
    extern "C" fn get_bucket_lock(_: *mut c_void, _: u32) -> u64 { 0 }
    extern "C" fn get_bucket_unlock(_: *mut c_void, _: u32, _: u64) {}
    extern "C" fn get_grow(_: *mut c_void, _: c_int) -> c_int { -12 }
    extern "C" fn get_zero(_: *mut c_void, _: *mut PageCachePage) {}
    extern "C" fn get_vget(ctx: *mut c_void, _: *mut c_void) {
        unsafe { (*(ctx as *mut GetHarness)).gets += 1; }
    }
    extern "C" fn get_vput(ctx: *mut c_void, _: *mut c_void) {
        unsafe { (*(ctx as *mut GetHarness)).puts += 1; }
    }
    extern "C" fn get_limit(ctx: *mut c_void) -> usize {
        unsafe { (*(ctx as *mut GetHarness)).limit }
    }
    extern "C" fn get_writeback(ctx: *mut c_void, _: usize) -> c_int {
        unsafe { (*(ctx as *mut GetHarness)).writebacks += 1; }
        0
    }
    extern "C" fn get_evict(ctx: *mut c_void, deferred: *mut *mut c_void) -> *mut PageCachePage {
        unsafe {
            let h = &mut *(ctx as *mut GetHarness);
            h.sweeps += 1;
            a20_pagecache_evict_scan(h.head, h.tail, 128,
                Some(reclaimable_yes), Some(lock_stub), Some(unlock_stub),
                h.hash, h.dirty, h.dirty_count, h.valid, Some(scan_stub), deferred)
        }
    }

    #[test]
    fn get_searches_whole_cache_before_reporting_exhaustion() {
        unsafe {
            let mut vn = mk_vn();
            let mut pool: Vec<_> = (0..1025).map(|_| mk()).collect();
            let mut head = mk();
            let mut tail = mk();
            head.next = &mut tail;
            tail.prev = &mut head;
            let mut hash = vec![core::ptr::null_mut(); PAGE_CACHE_HASH_BUCKETS];
            let mut dirty = core::ptr::null_mut();
            let mut free = core::ptr::null_mut();
            let mut dirty_count = 0;
            let mut valid = pool.len();
            let mut pins = vec![0usize; PAGE_CACHE_BUCKET_LOCKS * PIN_COUNTER_STRIDE / core::mem::size_of::<usize>()];
            for (i, p) in pool.iter_mut().enumerate() {
                p.vnode = &mut vn as *mut _ as *mut c_void;
                p.index = i as u64;
                p.valid = 1;
                p.ref_count = if i < 1024 { 1 } else { 0 };
                p.accessed = 1;
                if p.ref_count != 0 { a20_pagecache_account_pin(p, pins.as_mut_ptr()); }
                a20_pagecache_hash_insert(hash.as_mut_ptr(), p);
                a20_pagecache_mapping_insert(p);
                a20_pagecache_lru_insert_front(&mut head, p);
            }
            let mut h = GetHarness { head: &mut head, tail: &mut tail,
                hash: hash.as_mut_ptr(), dirty: &mut dirty,
                dirty_count: &mut dirty_count, valid: &mut valid,
                sweeps: 0, limit: 18, writebacks: 0, gets: 0, puts: 0 };
            let mut get = |index| a20_pagecache_get_transaction(
                &mut h as *mut _ as *mut c_void, &mut vn, index, 1,
                hash.as_mut_ptr(), &mut head, &mut tail, &mut free, &mut dirty,
                &mut dirty_count, &mut valid, pins.as_mut_ptr(), 1024,
                Some(get_lock), Some(get_unlock), Some(get_bucket_lock), Some(get_bucket_unlock),
                Some(get_grow), Some(get_evict), Some(get_limit), Some(get_writeback),
                Some(get_zero), Some(get_vget), Some(get_vput), Some(reclaimable_yes), Some(scan_stub));
            let page = get(2048);
            assert_eq!(page, &mut pool[1024] as *mut _);
            assert_eq!((*page).index, 2048);
            // Every descriptor is now pinned. Failure must terminate after
            // a complete bounded search, without dropping any mapping.
            assert!(get(2049).is_null());
            drop(get);
            assert!(h.sweeps > h.limit);
            assert_eq!(h.writebacks, 1);
            assert_eq!((h.gets, h.puts), (1, 1));
            assert_eq!(valid, 1025);
            assert_eq!(a20_pagecache_pinned_total(pins.as_ptr()), 1025);
            assert_eq!(a20_pagecache_put(page, pins.as_mut_ptr()), 1);
            assert_eq!(a20_pagecache_pinned_total(pins.as_ptr()), 1024);
        }
    }

    #[test]
    fn evict_scan_advances_past_pinned_tail() {
        unsafe {
            let mut vn = mk_vn();
            let mut pool: Vec<_> = (0..1025).map(|_| mk()).collect();
            let mut head = mk();
            let mut tail = mk();
            head.next = &mut tail;
            tail.prev = &mut head;
            let mut hash = vec![core::ptr::null_mut(); PAGE_CACHE_HASH_BUCKETS];
            let mut dirty = core::ptr::null_mut();
            let mut dirty_count = 0;
            let mut valid = pool.len();
            for (i, p) in pool.iter_mut().enumerate() {
                p.vnode = &mut vn as *mut _ as *mut c_void;
                p.index = i as u64;
                p.valid = 1;
                p.ref_count = if i < 1024 { 1 } else { 0 };
                p.accessed = 1;
                a20_pagecache_hash_insert(hash.as_mut_ptr(), p);
                a20_pagecache_mapping_insert(p);
                a20_pagecache_lru_insert_front(&mut head, p);
            }
            // Shared desktop surfaces can pin an entire tail window. Each
            // bounded sweep must advance, and the clean page beyond it must
            // survive one second chance before being reclaimed.
            let mut got = core::ptr::null_mut();
            for _ in 0..18 {
                let mut deferred = core::ptr::null_mut();
                got = a20_pagecache_evict_scan(
                    &mut head, &mut tail, 128, Some(reclaimable_yes),
                    Some(lock_stub), Some(unlock_stub), hash.as_mut_ptr(),
                    &mut dirty, &mut dirty_count, &mut valid, Some(scan_stub),
                    &mut deferred,
                );
                if !got.is_null() {
                    assert_eq!(deferred, &mut vn as *mut _ as *mut c_void);
                    break;
                }
            }
            assert_eq!(got, &mut pool[1024] as *mut _);
            assert_eq!(valid, 1024);
            assert!(pool[..1024].iter().all(|p| p.valid == 1 && p.ref_count == 1));
        }
    }

    #[test]
    fn evict_scan_reclaims_and_second_chances() {
        unsafe {
            let mut vn = mk_vn();
            let mut pool = [mk(), mk()];
            let mut head = mk();
            let mut tail = mk();
            head.next = &mut tail as *mut _;
            tail.prev = &mut head as *mut _;
            let mut hash = vec![core::ptr::null_mut::<PageCachePage>(); PAGE_CACHE_HASH_BUCKETS];
            let mut ghead: *mut PageCachePage = core::ptr::null_mut();
            let mut gcount = 0usize;
            let mut valid = 0usize;
            let mut deferred: *mut c_void = core::ptr::null_mut();

            pool[0].vnode = &mut vn as *mut _ as *mut c_void;
            pool[0].index = 1;
            pool[0].valid = 1;
            pool[0].accessed = 1;
            a20_pagecache_hash_insert(hash.as_mut_ptr(), &mut pool[0]);
            a20_pagecache_mapping_insert(&mut pool[0]);
            a20_pagecache_lru_insert_front(&mut head, &mut pool[0]);
            valid += 1;

            pool[1].vnode = &mut vn as *mut _ as *mut c_void;
            pool[1].index = 2;
            pool[1].valid = 1;
            a20_pagecache_hash_insert(hash.as_mut_ptr(), &mut pool[1]);
            a20_pagecache_mapping_insert(&mut pool[1]);
            a20_pagecache_lru_insert_front(&mut head, &mut pool[1]);
            valid += 1;

            // Tail-most page (pool[0]) gets a second chance first; next sweep
            // reaches pool[1] and reclaims it.
            let got = a20_pagecache_evict_scan(
                &mut head, &mut tail, 128, Some(reclaimable_yes), Some(lock_stub),
                Some(unlock_stub), hash.as_mut_ptr(), &mut ghead, &mut gcount,
                &mut valid, Some(scan_stub), &mut deferred,
            );
            assert_eq!(got, &mut pool[1] as *mut _);
            assert_eq!(pool[1].ref_count, 1);
            assert_eq!(pool[0].accessed, 0); // second chance spent on pool[0]
            // Refusing to reclaim anything is honoured.
            let got2 = a20_pagecache_evict_scan(
                &mut head, &mut tail, 128, Some(reclaimable_no), Some(lock_stub),
                Some(unlock_stub), hash.as_mut_ptr(), &mut ghead, &mut gcount,
                &mut valid, Some(scan_stub), &mut deferred,
            );
            assert!(got2.is_null());
        }
    }

    #[test]
    fn writeback_confirm_clears_only_matching_generation() {
        unsafe {
            let mut vn_storage = mk_vn();
            let vn = &mut vn_storage as *mut VnodeHead;
            let mut pool = [mk(), mk()];
            let mut ghead: *mut PageCachePage = core::ptr::null_mut();
            let mut gcount = 0usize;
            for i in 0..2 {
                pool[i].vnode = vn as *mut c_void;
                pool[i].index = i as u64;
                pool[i].valid = 1;
                a20_pagecache_dirty_insert(&mut pool[i], &mut ghead, &mut gcount);
            }
            pool[0].dirty_gen = 1;
            pool[1].dirty_gen = 1;
            let pages = [&mut pool[0] as *mut PageCachePage, &mut pool[1] as *mut PageCachePage];
            let gens = [1u64, 0u64];
            let cleared = a20_pagecache_writeback_confirm(
                pages.as_ptr(), 2, vn as *mut c_void, 0, gens.as_ptr(), &mut ghead, &mut gcount,
            );
            assert_eq!(cleared, 1);
            assert_eq!(pool[0].dirty, 0);
            assert_eq!(pool[1].dirty, 1);
            let _ = vn;
        }
    }
    #[test]
    fn readahead_plan_admits_clamps_and_skips() {
        let mut c = 0usize;
        unsafe {
            assert_eq!(a20_pagecache_readahead_plan(0, 8192, 0, 4096, &mut c), 0);
            assert_eq!(a20_pagecache_readahead_plan(1, 8192, 0, 0, &mut c), 0);
            assert_eq!(a20_pagecache_readahead_plan(1, 8192, 8192, 4096, &mut c), 0);
            assert_eq!(a20_pagecache_readahead_plan(1, 8192, 4096, 8192, &mut c), 1);
            assert_eq!(c, 4096); /* clamped to EOF */
            assert_eq!(a20_pagecache_readahead_plan(1, 16384, 0, 4096, &mut c), 1);
            assert_eq!(c, 4096);
        }
    }

    #[test]
    fn write_plan_geometry_and_rmw_precondition() {
        let (mut idx, mut off, mut chunk, mut fill) = (0u64, 0usize, 0usize, 0);
        unsafe {
            /* full first page: no RMW */
            a20_pagecache_write_plan(16384, 0, 0, 4096, &mut idx, &mut off, &mut chunk, &mut fill);
            assert_eq!((idx, off, chunk, fill), (0, 0, 4096, 0));
            /* partial write into an existing page: fill needed */
            a20_pagecache_write_plan(16384, 100, 0, 100, &mut idx, &mut off, &mut chunk, &mut fill);
            assert_eq!((idx, off, chunk, fill), (0, 100, 100, 1));
            /* write entirely past EOF: no fill (new page) */
            a20_pagecache_write_plan(100, 5000, 0, 10, &mut idx, &mut off, &mut chunk, &mut fill);
            assert_eq!(fill, 0);
            /* last chunk clamped to the write length */
            a20_pagecache_write_plan(16384, 4000, 0, 200, &mut idx, &mut off, &mut chunk, &mut fill);
            assert_eq!((idx, off, chunk), (0, 4000, 96));
            assert_eq!(fill, 1);
        }
    }

}
