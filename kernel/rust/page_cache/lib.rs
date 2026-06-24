//! Rust implementation of the A20OS page cache.
//!
//! Drop-in replacement for `kernel/fs/page_cache.c`.  Preserves the public C ABI
//! while replacing the manual intrusive-list / single-global-lock internals with
//! index-based lists protected by a safe irqsave spinlock wrapper, plus atomic
//! fields for refcount and per-page flags.

#![no_std]
#![allow(static_mut_refs)]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(rust_2018_idioms)]

mod ffi;

use core::ffi::{c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicBool, AtomicPtr, AtomicU32, AtomicU64, Ordering};

use a20rust_support::lock::IrqSaveSpinLock;

use ffi::{
    page_cache_stats_t, vfile_t, vnode_t, EINVAL, ENOMEM, ENOSYS,
    PFN_NONE, SEEK_SET, VFS_FT_REGULAR, PAGE_SIZE,
};

const MAX_PAGES: usize = 2048;
const HASH_BUCKETS: usize = 8192;
const NONE: u32 = u32::MAX;

const FLAG_VALID: u32 = 1 << 0;
const FLAG_DIRTY: u32 = 1 << 1;
const FLAG_UPTODATE: u32 = 1 << 2;

/// Opaque C type exposed to callers.
#[repr(C)]
pub struct page_cache_page {
    _opaque: [u8; 0],
}

/// One page-cache slot.  `pfn` and `data` are immutable after init; all other
/// fields are atomic so they can be read safely outside the lock when a
/// reference is held.
struct PageCachePage {
    vnode: AtomicPtr<vnode_t>,
    index: AtomicU64,
    pfn: u32,
    data: *mut c_void,
    ref_count: AtomicU32,
    flags: AtomicU32,
    dirty_gen: AtomicU32,
}

impl PageCachePage {
    const fn empty() -> Self {
        Self {
            vnode: AtomicPtr::new(ptr::null_mut()),
            index: AtomicU64::new(0),
            pfn: PFN_NONE,
            data: ptr::null_mut(),
            ref_count: AtomicU32::new(0),
            flags: AtomicU32::new(0),
            dirty_gen: AtomicU32::new(0),
        }
    }
}

/// Page slots, allocated statically.  A slot is recyclable only when its
/// refcount is zero.
static mut SLOTS: [PageCachePage; MAX_PAGES] = [const { PageCachePage::empty() }; MAX_PAGES];

/// Lock-protected cache state.  Hash and LRU link arrays are only accessed
/// while holding `STATE.lock()`.
struct PageCacheState {
    initialized: bool,
    lru_head: u32,
    lru_tail: u32,
    hash_heads: [u32; HASH_BUCKETS],
    lru_next: [u32; MAX_PAGES],
    lru_prev: [u32; MAX_PAGES],
    hash_next: [u32; MAX_PAGES],
}

impl PageCacheState {
    const fn new() -> Self {
        Self {
            initialized: false,
            lru_head: NONE,
            lru_tail: NONE,
            hash_heads: [NONE; HASH_BUCKETS],
            lru_next: [NONE; MAX_PAGES],
            lru_prev: [NONE; MAX_PAGES],
            hash_next: [NONE; MAX_PAGES],
        }
    }
}

static STATE: IrqSaveSpinLock<PageCacheState> = IrqSaveSpinLock::new(PageCacheState::new());
static INITIALIZED: AtomicBool = AtomicBool::new(false);

fn hash_key(vn: *mut vnode_t, index: u64) -> usize {
    let v = vn as usize;
    ((v >> 4) ^ index as usize ^ (index >> 32) as usize) & (HASH_BUCKETS - 1)
}

fn is_valid(page: &PageCachePage) -> bool {
    page.flags.load(Ordering::Acquire) & FLAG_VALID != 0
}

fn is_dirty(page: &PageCachePage) -> bool {
    page.flags.load(Ordering::Acquire) & FLAG_DIRTY != 0
}

fn is_uptodate(page: &PageCachePage) -> bool {
    page.flags.load(Ordering::Acquire) & FLAG_UPTODATE != 0
}

fn set_valid(page: &PageCachePage, v: bool) {
    set_flag(page, FLAG_VALID, v);
}

fn set_dirty(page: &PageCachePage, v: bool) {
    set_flag(page, FLAG_DIRTY, v);
}

fn set_uptodate(page: &PageCachePage, v: bool) {
    set_flag(page, FLAG_UPTODATE, v);
}

fn set_flag(page: &PageCachePage, flag: u32, v: bool) {
    if v {
        page.flags.fetch_or(flag, Ordering::Release);
    } else {
        page.flags.fetch_and(!flag, Ordering::Release);
    }
}

fn slot_ptr(idx: u32) -> *mut PageCachePage {
    unsafe { SLOTS.as_mut_ptr().add(idx as usize) }
}

fn slot_ref(idx: u32) -> &'static PageCachePage {
    unsafe { &*slot_ptr(idx) }
}

fn page_to_slot(page: *mut page_cache_page) -> *mut PageCachePage {
    page as *mut PageCachePage
}

fn lru_remove(state: &mut PageCacheState, idx: u32) {
    let prev = state.lru_prev[idx as usize];
    let next = state.lru_next[idx as usize];
    if prev != NONE {
        state.lru_next[prev as usize] = next;
    } else {
        state.lru_head = next;
    }
    if next != NONE {
        state.lru_prev[next as usize] = prev;
    } else {
        state.lru_tail = prev;
    }
    state.lru_prev[idx as usize] = NONE;
    state.lru_next[idx as usize] = NONE;
}

fn lru_insert_front(state: &mut PageCacheState, idx: u32) {
    state.lru_prev[idx as usize] = NONE;
    state.lru_next[idx as usize] = state.lru_head;
    if state.lru_head != NONE {
        state.lru_prev[state.lru_head as usize] = idx;
    } else {
        state.lru_tail = idx;
    }
    state.lru_head = idx;
}

fn hash_insert(state: &mut PageCacheState, idx: u32) {
    let page = slot_ref(idx);
    let bucket = hash_key(page.vnode.load(Ordering::Relaxed), page.index.load(Ordering::Relaxed));
    state.hash_next[idx as usize] = state.hash_heads[bucket];
    state.hash_heads[bucket] = idx;
}

fn hash_remove(state: &mut PageCacheState, idx: u32) {
    let page = slot_ref(idx);
    let bucket = hash_key(page.vnode.load(Ordering::Relaxed), page.index.load(Ordering::Relaxed));
    let mut cur = state.hash_heads[bucket];
    let mut prev: u32 = NONE;
    while cur != NONE {
        if cur == idx {
            if prev != NONE {
                state.hash_next[prev as usize] = state.hash_next[cur as usize];
            } else {
                state.hash_heads[bucket] = state.hash_next[cur as usize];
            }
            state.hash_next[cur as usize] = NONE;
            return;
        }
        prev = cur;
        cur = state.hash_next[cur as usize];
    }
    state.hash_next[idx as usize] = NONE;
}

fn find_locked(state: &PageCacheState, vn: *mut vnode_t, index: u64) -> Option<u32> {
    let bucket = hash_key(vn, index);
    let mut cur = state.hash_heads[bucket];
    while cur != NONE {
        let page = slot_ref(cur);
        if is_valid(page)
            && page.vnode.load(Ordering::Relaxed) == vn
            && page.index.load(Ordering::Relaxed) == index
        {
            return Some(cur);
        }
        cur = state.hash_next[cur as usize];
    }
    None
}

fn detach_mapping_locked(state: &mut PageCacheState, idx: u32) {
    let page = slot_ref(idx);
    if !is_valid(page) {
        return;
    }
    hash_remove(state, idx);
    let vn = page.vnode.swap(ptr::null_mut(), Ordering::Relaxed);
    page.index.store(0, Ordering::Relaxed);
    set_valid(page, false);
    set_dirty(page, false);
    page.dirty_gen.store(0, Ordering::Relaxed);
    set_uptodate(page, false);
    if !vn.is_null() {
        unsafe { ffi::vnode_put(vn) };
    }
}

fn evict_locked(state: &mut PageCacheState) -> Option<u32> {
    let mut idx = state.lru_tail;
    while idx != NONE {
        let page = slot_ref(idx);
        if page.ref_count.load(Ordering::Relaxed) == 0
            && !is_dirty(page)
            && unsafe { ffi::a20_pfn_valid(page.pfn) } != 0
            && unsafe { ffi::a20_frame_refcount(page.pfn) } <= 1
        {
            detach_mapping_locked(state, idx);
            page.ref_count.store(1, Ordering::Relaxed);
            lru_remove(state, idx);
            lru_insert_front(state, idx);
            return Some(idx);
        }
        idx = state.lru_prev[idx as usize];
    }
    None
}

#[no_mangle]
pub extern "C" fn page_cache_init() -> c_int {
    let mut state = STATE.lock();
    if state.initialized {
        return 0;
    }

    for i in 0..MAX_PAGES {
        let pfn = unsafe { ffi::pfa_alloc_page() };
        if pfn == PFN_NONE {
            return -ENOMEM;
        }
        let data = unsafe { ffi::a20_pfn_to_virt(pfn) };
        let page = unsafe { &mut *slot_ptr(i as u32) };
        page.pfn = pfn;
        page.data = data;
        page.ref_count.store(0, Ordering::Relaxed);
        lru_insert_front(&mut *state, i as u32);
    }

    state.initialized = true;
    INITIALIZED.store(true, Ordering::Release);
    0
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_get(
    vn: *mut vnode_t,
    index: u64,
    create: c_int,
) -> *mut page_cache_page {
    if !INITIALIZED.load(Ordering::Acquire) || vn.is_null() {
        return ptr::null_mut();
    }

    let mut state = STATE.lock();
    if let Some(idx) = find_locked(&*state, vn, index) {
        let page = slot_ref(idx);
        page.ref_count.fetch_add(1, Ordering::Relaxed);
        lru_remove(&mut *state, idx);
        lru_insert_front(&mut *state, idx);
        return slot_ptr(idx) as *mut page_cache_page;
    }

    if create == 0 {
        return ptr::null_mut();
    }

    let idx = match evict_locked(&mut *state) {
        Some(i) => i,
        None => return ptr::null_mut(),
    };

    let page = slot_ref(idx);
    page.vnode.store(vn, Ordering::Relaxed);
    page.index.store(index, Ordering::Relaxed);
    set_valid(page, true);
    set_dirty(page, false);
    page.dirty_gen.store(0, Ordering::Relaxed);
    set_uptodate(page, false);
    unsafe {
        ptr::write_bytes(page.data, 0, PAGE_SIZE);
        ffi::vnode_get(vn);
    }
    hash_insert(&mut *state, idx);
    slot_ptr(idx) as *mut page_cache_page
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_put(page: *mut page_cache_page) {
    if page.is_null() {
        return;
    }
    let page = unsafe { &*page_to_slot(page) };
    if page.ref_count.load(Ordering::Relaxed) > 0 {
        page.ref_count.fetch_sub(1, Ordering::Relaxed);
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_data(page: *mut page_cache_page) -> *mut c_void {
    if page.is_null() {
        return ptr::null_mut();
    }
    let page = unsafe { &*page_to_slot(page) };
    page.data
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_pfn(page: *mut page_cache_page) -> u32 {
    if page.is_null() {
        return PFN_NONE;
    }
    let page = unsafe { &*page_to_slot(page) };
    page.pfn
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_mark_uptodate(page: *mut page_cache_page) {
    if !page.is_null() {
        let page = unsafe { &*page_to_slot(page) };
        set_uptodate(page, true);
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_mark_dirty(page: *mut page_cache_page) {
    if !page.is_null() {
        let page = unsafe { &*page_to_slot(page) };
        page.dirty_gen.fetch_add(1, Ordering::Release);
        set_dirty(page, true);
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_mark_clean(page: *mut page_cache_page) {
    if !page.is_null() {
        let page = unsafe { &*page_to_slot(page) };
        set_dirty(page, false);
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_is_uptodate(page: *mut page_cache_page) -> c_int {
    if page.is_null() {
        return 0;
    }
    let page = unsafe { &*page_to_slot(page) };
    is_uptodate(page) as c_int
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_fill_vfile_page(
    vf: *mut vfile_t,
    page: *mut page_cache_page,
) -> c_int {
    if vf.is_null() {
        return -EINVAL;
    }
    let vf = unsafe { &mut *vf };
    if vf.vnode.is_null() || vf.vnode != unsafe { &*page_to_slot(page) }.vnode.load(Ordering::Relaxed) {
        return -EINVAL;
    }
    let ops = if vf.ops.is_null() {
        return -EINVAL;
    } else {
        unsafe { &*vf.ops }
    };
    if ops.read.is_none() || ops.lseek.is_none() {
        return -EINVAL;
    }

    let page = unsafe { &*page_to_slot(page) };
    let data = page.data;
    if data.is_null() {
        return -ENOMEM;
    }

    let saved = vf.offset;
    let page_base = (page.index.load(Ordering::Relaxed) as usize) * PAGE_SIZE;

    let seek_r = unsafe { ops.lseek.unwrap()(vf as *mut _, page_base as i64, SEEK_SET) };
    if seek_r < 0 {
        return seek_r as c_int;
    }

    let r = unsafe { ops.read.unwrap()(vf as *mut _, data as *mut u8, PAGE_SIZE) };
    let restore_r = unsafe { ops.lseek.unwrap()(vf as *mut _, saved as i64, SEEK_SET) };
    if restore_r < 0 && r >= 0 {
        return restore_r as c_int;
    }
    if r < 0 {
        return r;
    }

    if (r as usize) < PAGE_SIZE {
        unsafe {
            ptr::write_bytes((data as *mut u8).add(r as usize), 0, PAGE_SIZE - r as usize);
        }
    }
    set_uptodate(page, true);
    r
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_read_vfile(
    vf: *mut vfile_t,
    buf: *mut u8,
    count: usize,
) -> c_int {
    if vf.is_null() || buf.is_null() {
        return -EINVAL;
    }
    let vf = unsafe { &mut *vf };
    if vf.vnode.is_null() {
        return -EINVAL;
    }
    if vf.ops.is_null() {
        return -ENOSYS;
    }
    let ops = unsafe { &*vf.ops };
    if ops.read.is_none() || ops.lseek.is_none() {
        return -ENOSYS;
    }
    if unsafe { (*vf.vnode).type_ } != VFS_FT_REGULAR {
        return -EINVAL;
    }
    if count == 0 {
        return 0;
    }

    let file_size = unsafe { (*vf.vnode).size };
    let start = vf.offset;
    if start >= file_size {
        return 0;
    }
    let count = core::cmp::min(count, file_size - start);

    let mut done: usize = 0;
    while done < count {
        let pos = start + done;
        let index = pos / PAGE_SIZE;
        let page_off = pos % PAGE_SIZE;
        let chunk = core::cmp::min(PAGE_SIZE - page_off, count - done);

        let page = unsafe { page_cache_get(vf.vnode, index as u64, 1) };
        if page.is_null() {
            break;
        }

        if unsafe { page_cache_is_uptodate(page) } == 0 {
            let r = unsafe { page_cache_fill_vfile_page(vf as *const _ as *mut _, page) };
            if r < 0 {
                unsafe { page_cache_put(page) };
                if done == 0 {
                    return r;
                }
                break;
            }
        }

        let data = unsafe { page_cache_data(page) } as *const u8;
        unsafe {
            ptr::copy_nonoverlapping(data.add(page_off), buf.add(done), chunk);
        }
        unsafe { page_cache_put(page) };
        done += chunk;
    }

    if done > 0 {
        let new_offset = start + done;
        let seek_r = unsafe { ops.lseek.unwrap()(vf as *mut _, new_offset as i64, SEEK_SET) };
        if seek_r < 0 {
            vf.offset = new_offset;
        }
    }
    done as c_int
}

fn find_dirty_locked(_state: &PageCacheState, vn: *mut vnode_t) -> Option<u32> {
    for i in 0..MAX_PAGES {
        let page = slot_ref(i as u32);
        if !is_valid(page) || !is_dirty(page) {
            continue;
        }
        if !vn.is_null() && page.vnode.load(Ordering::Relaxed) != vn {
            continue;
        }
        page.ref_count.fetch_add(1, Ordering::Relaxed);
        return Some(i as u32);
    }
    None
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_writeback_vnode(
    vn: *mut vnode_t,
    writepage: Option<unsafe extern "C" fn(*mut vnode_t, u64, *const c_void, usize, *mut c_void) -> c_int>,
    ctx: *mut c_void,
) -> c_int {
    if vn.is_null() {
        return -EINVAL;
    }
    unsafe { page_cache_writeback_common(vn, writepage, ctx) }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_writeback_all(
    writepage: Option<unsafe extern "C" fn(*mut vnode_t, u64, *const c_void, usize, *mut c_void) -> c_int>,
    ctx: *mut c_void,
) -> c_int {
    unsafe { page_cache_writeback_common(ptr::null_mut(), writepage, ctx) }
}

unsafe fn page_cache_writeback_common(
    vn: *mut vnode_t,
    writepage: Option<unsafe extern "C" fn(*mut vnode_t, u64, *const c_void, usize, *mut c_void) -> c_int>,
    ctx: *mut c_void,
) -> c_int {
    if !INITIALIZED.load(Ordering::Acquire) {
        return 0;
    }

    loop {
        let state = STATE.lock();
        let idx = match find_dirty_locked(&*state, vn) {
            Some(i) => i,
            None => return 0,
        };
        let page = slot_ref(idx);
        let page_vn = page.vnode.load(Ordering::Acquire);
        let index = page.index.load(Ordering::Acquire);
        let data = page.data;
        let dirty_gen = page.dirty_gen.load(Ordering::Acquire);
        drop(state);

        let r = if let Some(cb) = writepage {
            unsafe { cb(page_vn, index, data, PAGE_SIZE, ctx) }
        } else if !page_vn.is_null() {
            unsafe { ffi::a20_vnode_writepage(page_vn, index, data, PAGE_SIZE) }
        } else {
            unsafe { page_cache_put(slot_ptr(idx) as *mut page_cache_page) };
            return -ENOSYS;
        };

        let state = STATE.lock();
        if r >= 0
            && is_valid(page)
            && page.vnode.load(Ordering::Acquire) == page_vn
            && page.index.load(Ordering::Acquire) == index
            && page.dirty_gen.load(Ordering::Acquire) == dirty_gen
        {
            set_dirty(page, false);
        }
        drop(state);

        unsafe { page_cache_put(slot_ptr(idx) as *mut page_cache_page) };
        if r < 0 {
            return r;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_invalidate(vn: *mut vnode_t) {
    if !INITIALIZED.load(Ordering::Acquire) || vn.is_null() {
        return;
    }
    let mut state = STATE.lock();
    for i in 0..MAX_PAGES {
        let page = slot_ref(i as u32);
        if is_valid(page)
            && page.vnode.load(Ordering::Relaxed) == vn
            && page.ref_count.load(Ordering::Relaxed) == 0
        {
            detach_mapping_locked(&mut *state, i as u32);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_invalidate_range(
    vn: *mut vnode_t,
    start_byte: u64,
    end_byte: u64,
) {
    if !INITIALIZED.load(Ordering::Acquire) || vn.is_null() || end_byte <= start_byte {
        return;
    }
    let first_idx = start_byte / PAGE_SIZE as u64;
    let last_idx = (end_byte - 1) / PAGE_SIZE as u64;
    let mut state = STATE.lock();
    for i in 0..MAX_PAGES {
        let page = slot_ref(i as u32);
        if !is_valid(page) || page.vnode.load(Ordering::Relaxed) != vn {
            continue;
        }
        let idx = page.index.load(Ordering::Relaxed);
        if idx < first_idx || idx > last_idx {
            continue;
        }
        if page.ref_count.load(Ordering::Relaxed) == 0 {
            detach_mapping_locked(&mut *state, i as u32);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_invalidate_uptodate_range(
    vn: *mut vnode_t,
    start_byte: u64,
    end_byte: u64,
) {
    if !INITIALIZED.load(Ordering::Acquire) || vn.is_null() || end_byte <= start_byte {
        return;
    }
    let first_idx = start_byte / PAGE_SIZE as u64;
    let last_idx = (end_byte - 1) / PAGE_SIZE as u64;
    let mut state = STATE.lock();
    for i in 0..MAX_PAGES {
        let page = slot_ref(i as u32);
        if !is_valid(page) || page.vnode.load(Ordering::Relaxed) != vn {
            continue;
        }
        let idx = page.index.load(Ordering::Relaxed);
        if idx < first_idx || idx > last_idx {
            continue;
        }
        if page.ref_count.load(Ordering::Relaxed) == 0 {
            detach_mapping_locked(&mut *state, i as u32);
        } else {
            set_uptodate(page, false);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_truncate(vn: *mut vnode_t, new_size: u64) {
    if !INITIALIZED.load(Ordering::Acquire) || vn.is_null() {
        return;
    }
    let first_drop = ((new_size + PAGE_SIZE as u64 - 1) / PAGE_SIZE as u64) as u64;
    let mut state = STATE.lock();
    for i in 0..MAX_PAGES {
        let page = slot_ref(i as u32);
        if !is_valid(page)
            || page.vnode.load(Ordering::Relaxed) != vn
            || page.index.load(Ordering::Relaxed) < first_drop
        {
            continue;
        }
        if page.ref_count.load(Ordering::Relaxed) == 0 {
            detach_mapping_locked(&mut *state, i as u32);
        } else {
            unsafe {
                ptr::write_bytes(page.data, 0, PAGE_SIZE);
            }
            set_uptodate(page, false);
            set_dirty(page, false);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn page_cache_get_stats(stats: *mut page_cache_stats_t) {
    if stats.is_null() {
        return;
    }
    let stats = unsafe { &mut *stats };
    stats.capacity = MAX_PAGES;
    stats.bytes = MAX_PAGES * PAGE_SIZE;
    stats.valid = 0;
    stats.dirty = 0;
    stats.pinned = 0;

    if !INITIALIZED.load(Ordering::Acquire) {
        return;
    }
    let _state = STATE.lock();
    for i in 0..MAX_PAGES {
        let page = slot_ref(i as u32);
        if !is_valid(page) {
            continue;
        }
        stats.valid += 1;
        if is_dirty(page) {
            stats.dirty += 1;
        }
        if page.ref_count.load(Ordering::Relaxed) > 0 {
            stats.pinned += 1;
        }
    }
}
