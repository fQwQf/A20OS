//! Rust implementation of the A20OS block cache.
//!
//! Drop-in replacement for `kernel/fs/block_cache.c` preserving the public C
//! ABI while moving the lock-protected state to safe Rust structures.

#![no_std]
#![allow(static_mut_refs)]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(rust_2018_idioms)]

mod ffi;

use core::cell::UnsafeCell;
use core::ffi::{c_int, c_void};
use core::mem::size_of;
use core::ptr;
use core::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};

use a20rust_support::lock::IrqSaveSpinLock;

use ffi::{
    bcache_stats_t, block_dev_t, BCACHE_BLOCK_SIZE, BCACHE_HASH_BUCKETS,
    BCACHE_MAX_BLOCKS, PCACHE_HASH_BUCKETS, PCACHE_MAX_PAGES, PCACHE_PAGE_SIZE,
};

const NONE: u32 = u32::MAX;
const FLAG_VALID: u32 = 1 << 0;
const FLAG_DIRTY: u32 = 1 << 1;

#[repr(C)]
pub struct bcache {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct bcache_entry {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct pcache_entry {
    _opaque: [u8; 0],
}

struct BlockEntry {
    lba: AtomicU64,
    dirty_gen: AtomicU64,
    ref_count: AtomicI32,
    flags: AtomicU32,
    prev: AtomicU32,
    next: AtomicU32,
    hnext: AtomicU32,
    data: UnsafeCell<[u8; BCACHE_BLOCK_SIZE]>,
}

struct PageEntry {
    page_no: AtomicU64,
    dirty_gen: AtomicU64,
    ref_count: AtomicI32,
    flags: AtomicU32,
    prev: AtomicU32,
    next: AtomicU32,
    hnext: AtomicU32,
    data: UnsafeCell<[u8; PCACHE_PAGE_SIZE]>,
}

struct BcacheState {
    dirty_blocks: usize,
    dirty_pages: usize,
    lru_head: u32,
    lru_tail: u32,
    page_lru_head: u32,
    page_lru_tail: u32,
    hash_heads: [u32; BCACHE_HASH_BUCKETS],
    page_hash_heads: [u32; PCACHE_HASH_BUCKETS],
}

impl BcacheState {
    const fn new() -> Self {
        Self {
            dirty_blocks: 0,
            dirty_pages: 0,
            lru_head: NONE,
            lru_tail: NONE,
            page_lru_head: NONE,
            page_lru_tail: NONE,
            hash_heads: [NONE; BCACHE_HASH_BUCKETS],
            page_hash_heads: [NONE; PCACHE_HASH_BUCKETS],
        }
    }
}

struct Bcache {
    dev: *mut block_dev_t,
    pool: *mut BlockEntry,
    page_pool: *mut PageEntry,
    pool_size: i32,
    page_pool_size: i32,
    state: IrqSaveSpinLock<BcacheState>,
}

struct GlobalState {
    list: [usize; 8],
    count: usize,
}

impl GlobalState {
    const fn new() -> Self {
        Self {
            list: [0; 8],
            count: 0,
        }
    }
}

static GLOBAL_STATE: IrqSaveSpinLock<GlobalState> = IrqSaveSpinLock::new(GlobalState::new());

fn block_hash_key(lba: u64) -> usize {
    ((lba ^ (lba >> 32)) as usize) & (BCACHE_HASH_BUCKETS - 1)
}

fn page_hash_key(page_no: u64) -> usize {
    ((page_no ^ (page_no >> 32)) as usize) & (PCACHE_HASH_BUCKETS - 1)
}

fn bcache_from_opaque(bc: *mut bcache) -> *mut Bcache {
    bc as *mut Bcache
}

fn block_from_opaque(e: *mut bcache_entry) -> *mut BlockEntry {
    e as *mut BlockEntry
}

fn block_at(bc: *mut Bcache, idx: u32) -> *mut BlockEntry {
    unsafe { (*bc).pool.add(idx as usize) }
}

fn page_at(bc: *mut Bcache, idx: u32) -> *mut PageEntry {
    unsafe { (*bc).page_pool.add(idx as usize) }
}


fn block_valid(e: *mut BlockEntry) -> bool {
    unsafe { (*e).flags.load(Ordering::Acquire) & FLAG_VALID != 0 }
}

fn block_dirty(e: *mut BlockEntry) -> bool {
    unsafe { (*e).flags.load(Ordering::Acquire) & FLAG_DIRTY != 0 }
}

fn page_valid(e: *mut PageEntry) -> bool {
    unsafe { (*e).flags.load(Ordering::Acquire) & FLAG_VALID != 0 }
}

fn page_dirty(e: *mut PageEntry) -> bool {
    unsafe { (*e).flags.load(Ordering::Acquire) & FLAG_DIRTY != 0 }
}

unsafe fn set_block_valid(e: *mut BlockEntry, value: bool) {
    let flags = unsafe { &(*e).flags };
    if value {
        flags.fetch_or(FLAG_VALID, Ordering::Release);
    } else {
        flags.fetch_and(!FLAG_VALID, Ordering::Release);
    }
}

unsafe fn set_block_dirty_flag(e: *mut BlockEntry, value: bool) {
    let flags = unsafe { &(*e).flags };
    if value {
        flags.fetch_or(FLAG_DIRTY, Ordering::Release);
    } else {
        flags.fetch_and(!FLAG_DIRTY, Ordering::Release);
    }
}

unsafe fn set_page_valid(e: *mut PageEntry, value: bool) {
    let flags = unsafe { &(*e).flags };
    if value {
        flags.fetch_or(FLAG_VALID, Ordering::Release);
    } else {
        flags.fetch_and(!FLAG_VALID, Ordering::Release);
    }
}

unsafe fn set_page_dirty_flag(e: *mut PageEntry, value: bool) {
    let flags = unsafe { &(*e).flags };
    if value {
        flags.fetch_or(FLAG_DIRTY, Ordering::Release);
    } else {
        flags.fetch_and(!FLAG_DIRTY, Ordering::Release);
    }
}

unsafe fn bcache_set_block_dirty_locked(state: &mut BcacheState, e: *mut BlockEntry, dirty: bool) {
    if dirty {
        unsafe {
            (*e).dirty_gen.fetch_add(1, Ordering::Release);
        }
        if !block_dirty(e) {
            state.dirty_blocks += 1;
        }
        unsafe { set_block_dirty_flag(e, true) };
    } else if block_dirty(e) {
        unsafe { set_block_dirty_flag(e, false) };
        if state.dirty_blocks > 0 {
            state.dirty_blocks -= 1;
        }
    }
}

unsafe fn bcache_set_page_dirty_locked(state: &mut BcacheState, e: *mut PageEntry, dirty: bool) {
    if dirty {
        unsafe {
            (*e).dirty_gen.fetch_add(1, Ordering::Release);
        }
        if !page_dirty(e) {
            state.dirty_pages += 1;
        }
        unsafe { set_page_dirty_flag(e, true) };
    } else if page_dirty(e) {
        unsafe { set_page_dirty_flag(e, false) };
        if state.dirty_pages > 0 {
            state.dirty_pages -= 1;
        }
    }
}

unsafe fn block_lru_remove(state: &mut BcacheState, bc: *mut Bcache, idx: u32) {
    let e = block_at(bc, idx);
    let prev = unsafe { (*e).prev.load(Ordering::Relaxed) };
    let next = unsafe { (*e).next.load(Ordering::Relaxed) };
    if prev != NONE {
        unsafe { (*block_at(bc, prev)).next.store(next, Ordering::Relaxed) };
    } else {
        state.lru_head = next;
    }
    if next != NONE {
        unsafe { (*block_at(bc, next)).prev.store(prev, Ordering::Relaxed) };
    } else {
        state.lru_tail = prev;
    }
    unsafe {
        (*e).prev.store(NONE, Ordering::Relaxed);
        (*e).next.store(NONE, Ordering::Relaxed);
    }
}

unsafe fn block_lru_insert_front(state: &mut BcacheState, bc: *mut Bcache, idx: u32) {
    let e = block_at(bc, idx);
    unsafe {
        (*e).prev.store(NONE, Ordering::Relaxed);
        (*e).next.store(state.lru_head, Ordering::Relaxed);
    }
    if state.lru_head != NONE {
        unsafe { (*block_at(bc, state.lru_head)).prev.store(idx, Ordering::Relaxed) };
    } else {
        state.lru_tail = idx;
    }
    state.lru_head = idx;
}

unsafe fn page_lru_remove(state: &mut BcacheState, bc: *mut Bcache, idx: u32) {
    let e = page_at(bc, idx);
    let prev = unsafe { (*e).prev.load(Ordering::Relaxed) };
    let next = unsafe { (*e).next.load(Ordering::Relaxed) };
    if prev != NONE {
        unsafe { (*page_at(bc, prev)).next.store(next, Ordering::Relaxed) };
    } else {
        state.page_lru_head = next;
    }
    if next != NONE {
        unsafe { (*page_at(bc, next)).prev.store(prev, Ordering::Relaxed) };
    } else {
        state.page_lru_tail = prev;
    }
    unsafe {
        (*e).prev.store(NONE, Ordering::Relaxed);
        (*e).next.store(NONE, Ordering::Relaxed);
    }
}

unsafe fn page_lru_insert_front(state: &mut BcacheState, bc: *mut Bcache, idx: u32) {
    let e = page_at(bc, idx);
    unsafe {
        (*e).prev.store(NONE, Ordering::Relaxed);
        (*e).next.store(state.page_lru_head, Ordering::Relaxed);
    }
    if state.page_lru_head != NONE {
        unsafe { (*page_at(bc, state.page_lru_head)).prev.store(idx, Ordering::Relaxed) };
    } else {
        state.page_lru_tail = idx;
    }
    state.page_lru_head = idx;
}

unsafe fn block_hash_insert(state: &mut BcacheState, bc: *mut Bcache, idx: u32) {
    let e = block_at(bc, idx);
    let bucket = block_hash_key(unsafe { (*e).lba.load(Ordering::Relaxed) });
    unsafe {
        (*e).hnext.store(state.hash_heads[bucket], Ordering::Relaxed);
    }
    state.hash_heads[bucket] = idx;
}

unsafe fn block_hash_remove(state: &mut BcacheState, bc: *mut Bcache, idx: u32) {
    let e = block_at(bc, idx);
    let bucket = block_hash_key(unsafe { (*e).lba.load(Ordering::Relaxed) });
    let mut cur = state.hash_heads[bucket];
    let mut prev = NONE;
    while cur != NONE {
        if cur == idx {
            let next = unsafe { (*block_at(bc, cur)).hnext.load(Ordering::Relaxed) };
            if prev != NONE {
                unsafe { (*block_at(bc, prev)).hnext.store(next, Ordering::Relaxed) };
            } else {
                state.hash_heads[bucket] = next;
            }
            unsafe { (*e).hnext.store(NONE, Ordering::Relaxed) };
            return;
        }
        prev = cur;
        cur = unsafe { (*block_at(bc, cur)).hnext.load(Ordering::Relaxed) };
    }
    unsafe { (*e).hnext.store(NONE, Ordering::Relaxed) };
}

unsafe fn page_hash_insert(state: &mut BcacheState, bc: *mut Bcache, idx: u32) {
    let e = page_at(bc, idx);
    let bucket = page_hash_key(unsafe { (*e).page_no.load(Ordering::Relaxed) });
    unsafe {
        (*e).hnext.store(state.page_hash_heads[bucket], Ordering::Relaxed);
    }
    state.page_hash_heads[bucket] = idx;
}

unsafe fn page_hash_remove(state: &mut BcacheState, bc: *mut Bcache, idx: u32) {
    let e = page_at(bc, idx);
    let bucket = page_hash_key(unsafe { (*e).page_no.load(Ordering::Relaxed) });
    let mut cur = state.page_hash_heads[bucket];
    let mut prev = NONE;
    while cur != NONE {
        if cur == idx {
            let next = unsafe { (*page_at(bc, cur)).hnext.load(Ordering::Relaxed) };
            if prev != NONE {
                unsafe { (*page_at(bc, prev)).hnext.store(next, Ordering::Relaxed) };
            } else {
                state.page_hash_heads[bucket] = next;
            }
            unsafe { (*e).hnext.store(NONE, Ordering::Relaxed) };
            return;
        }
        prev = cur;
        cur = unsafe { (*page_at(bc, cur)).hnext.load(Ordering::Relaxed) };
    }
    unsafe { (*e).hnext.store(NONE, Ordering::Relaxed) };
}

unsafe fn bcache_find_locked(state: &BcacheState, bc: *mut Bcache, lba: u64) -> Option<u32> {
    let mut cur = state.hash_heads[block_hash_key(lba)];
    while cur != NONE {
        let e = block_at(bc, cur);
        if block_valid(e) && unsafe { (*e).lba.load(Ordering::Relaxed) == lba } {
            return Some(cur);
        }
        cur = unsafe { (*e).hnext.load(Ordering::Relaxed) };
    }
    None
}

unsafe fn pcache_find_locked(state: &BcacheState, bc: *mut Bcache, page_no: u64) -> Option<u32> {
    let mut cur = state.page_hash_heads[page_hash_key(page_no)];
    while cur != NONE {
        let e = page_at(bc, cur);
        if page_valid(e) && unsafe { (*e).page_no.load(Ordering::Relaxed) == page_no } {
            return Some(cur);
        }
        cur = unsafe { (*e).hnext.load(Ordering::Relaxed) };
    }
    None
}

unsafe fn bcache_evict_locked(state: &mut BcacheState, bc: *mut Bcache) -> Option<u32> {
    let mut cur = state.lru_tail;
    while cur != NONE {
        let e = block_at(bc, cur);
        if unsafe { (*e).ref_count.load(Ordering::Relaxed) == 0 } {
            unsafe { block_lru_remove(state, bc, cur) };
            if block_valid(e) {
                unsafe { block_hash_remove(state, bc, cur) };
            }
            unsafe { (*e).ref_count.store(1, Ordering::Relaxed) };
            return Some(cur);
        }
        cur = unsafe { (*e).prev.load(Ordering::Relaxed) };
    }
    None
}

unsafe fn pcache_evict_locked(state: &mut BcacheState, bc: *mut Bcache) -> Option<u32> {
    let mut cur = state.page_lru_tail;
    while cur != NONE {
        let e = page_at(bc, cur);
        if unsafe { (*e).ref_count.load(Ordering::Relaxed) == 0 } {
            unsafe { page_lru_remove(state, bc, cur) };
            if page_valid(e) {
                unsafe { page_hash_remove(state, bc, cur) };
            }
            unsafe { (*e).ref_count.store(1, Ordering::Relaxed) };
            return Some(cur);
        }
        cur = unsafe { (*e).prev.load(Ordering::Relaxed) };
    }
    None
}

unsafe fn pcache_flush_page(bc: *mut Bcache, e: *mut PageEntry) -> c_int {
    if unsafe { (*bc).dev.is_null() } || !page_dirty(e) {
        return 0;
    }
    let page_no = unsafe { (*e).page_no.load(Ordering::Relaxed) };
    let lba = (page_no * PCACHE_PAGE_SIZE as u64) / BCACHE_BLOCK_SIZE as u64;
    unsafe { ffi::a20_block_dev_write((*bc).dev, lba, (*e).data.get().cast::<c_void>(), PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE) }
}

unsafe fn init_block_pool(bc: *mut Bcache) {
    for i in 0..BCACHE_MAX_BLOCKS as u32 {
        let e = block_at(bc, i);
        unsafe {
            ptr::write_bytes(e.cast::<u8>(), 0, size_of::<BlockEntry>());
            (*e).lba.store(u64::MAX, Ordering::Relaxed);
            (*e).prev.store(NONE, Ordering::Relaxed);
            (*e).next.store(NONE, Ordering::Relaxed);
            (*e).hnext.store(NONE, Ordering::Relaxed);
        }
    }
}

unsafe fn init_page_pool(bc: *mut Bcache) {
    for i in 0..PCACHE_MAX_PAGES as u32 {
        let e = page_at(bc, i);
        unsafe {
            ptr::write_bytes(e.cast::<u8>(), 0, size_of::<PageEntry>());
            (*e).page_no.store(u64::MAX, Ordering::Relaxed);
            (*e).prev.store(NONE, Ordering::Relaxed);
            (*e).next.store(NONE, Ordering::Relaxed);
            (*e).hnext.store(NONE, Ordering::Relaxed);
        }
    }
}

unsafe fn global_add(bc: *mut Bcache) {
    let mut global = GLOBAL_STATE.lock();
    if global.count < global.list.len() {
        let slot = global.count;
        global.list[slot] = bc as usize;
        global.count = slot + 1;
    }
}

unsafe fn global_remove(bc: *mut Bcache) {
    let mut global = GLOBAL_STATE.lock();
    let mut i = 0usize;
    while i < global.count {
        if global.list[i] == bc as usize {
            let last = global.count - 1;
            global.list[i] = global.list[last];
            global.list[last] = 0;
            global.count -= 1;
            break;
        }
        i += 1;
    }
}

#[no_mangle]
pub unsafe extern "C" fn bcache_create(dev: *mut block_dev_t) -> *mut bcache {
    let bc_ptr = unsafe { ffi::kmalloc(size_of::<Bcache>()) }.cast::<Bcache>();
    if bc_ptr.is_null() {
        return ptr::null_mut();
    }

    let pool = unsafe {
        ffi::kmalloc(size_of::<BlockEntry>() * BCACHE_MAX_BLOCKS)
    }
    .cast::<BlockEntry>();
    if pool.is_null() {
        unsafe { ffi::kfree(bc_ptr.cast::<c_void>()) };
        return ptr::null_mut();
    }

    let page_pool = unsafe {
        ffi::kmalloc(size_of::<PageEntry>() * PCACHE_MAX_PAGES)
    }
    .cast::<PageEntry>();
    if page_pool.is_null() {
        unsafe {
            ffi::kfree(pool.cast::<c_void>());
            ffi::kfree(bc_ptr.cast::<c_void>());
        }
        return ptr::null_mut();
    }

    unsafe {
        ptr::write(
            bc_ptr,
            Bcache {
                dev,
                pool,
                page_pool,
                pool_size: BCACHE_MAX_BLOCKS as i32,
                page_pool_size: PCACHE_MAX_PAGES as i32,
                state: IrqSaveSpinLock::new(BcacheState::new()),
            },
        );
        init_block_pool(bc_ptr);
        init_page_pool(bc_ptr);
        let mut state = (*bc_ptr).state.lock();
        for i in 0..BCACHE_MAX_BLOCKS as u32 {
            block_lru_insert_front(&mut state, bc_ptr, i);
        }
        for i in 0..PCACHE_MAX_PAGES as u32 {
            page_lru_insert_front(&mut state, bc_ptr, i);
        }
        global_add(bc_ptr);
    }

    bc_ptr.cast::<bcache>()
}

#[no_mangle]
pub unsafe extern "C" fn bcache_destroy(bc: *mut bcache) {
    if bc.is_null() {
        return;
    }
    let bc = bcache_from_opaque(bc);
    unsafe { bcache_sync(bc.cast::<bcache>()) };
    unsafe { global_remove(bc) };
    unsafe {
        if !(*bc).page_pool.is_null() {
            ffi::kfree((*bc).page_pool.cast::<c_void>());
        }
        if !(*bc).pool.is_null() {
            ffi::kfree((*bc).pool.cast::<c_void>());
        }
        ffi::kfree(bc.cast::<c_void>());
    }
}

#[no_mangle]
pub unsafe extern "C" fn bcache_get(bc: *mut bcache, lba: u64) -> *mut bcache_entry {
    if bc.is_null() {
        return ptr::null_mut();
    }
    let bc = bcache_from_opaque(bc);
    let mut state = unsafe { (*bc).state.lock() };
    if let Some(idx) = unsafe { bcache_find_locked(&state, bc, lba) } {
        let e = block_at(bc, idx);
        unsafe {
            (*e).ref_count.fetch_add(1, Ordering::Relaxed);
            block_lru_remove(&mut state, bc, idx);
            block_lru_insert_front(&mut state, bc, idx);
        }
        return e.cast::<bcache_entry>();
    }

    let idx = match unsafe { bcache_evict_locked(&mut state, bc) } {
        Some(idx) => idx,
        None => return ptr::null_mut(),
    };
    let e = block_at(bc, idx);
    let old_lba = unsafe { (*e).lba.load(Ordering::Relaxed) };
    let old_dirty = block_dirty(e) && block_valid(e);
    unsafe { set_block_valid(e, false) };
    drop(state);

    if old_dirty && unsafe { !(*bc).dev.is_null() } {
        if unsafe {
            ffi::a20_block_dev_write((*bc).dev, old_lba, (*e).data.get().cast::<c_void>(), 1)
        } < 0
        {
            let mut state = unsafe { (*bc).state.lock() };
            unsafe {
                (*e).ref_count.store(0, Ordering::Relaxed);
                bcache_set_block_dirty_locked(&mut state, e, true);
                set_block_valid(e, true);
                block_hash_insert(&mut state, bc, idx);
                block_lru_insert_front(&mut state, bc, idx);
            }
            return ptr::null_mut();
        }
    }

    if unsafe { !(*bc).dev.is_null() } {
        if unsafe {
            ffi::a20_block_dev_read((*bc).dev, lba, (*e).data.get().cast::<c_void>(), 1)
        } < 0
        {
            let mut state = unsafe { (*bc).state.lock() };
            unsafe {
                (*e).ref_count.store(0, Ordering::Relaxed);
                (*e).lba.store(u64::MAX, Ordering::Relaxed);
                block_lru_insert_front(&mut state, bc, idx);
            }
            return ptr::null_mut();
        }
    } else {
        unsafe {
            ffi::memset((*e).data.get().cast::<c_void>(), 0, BCACHE_BLOCK_SIZE);
        }
    }

    let mut state = unsafe { (*bc).state.lock() };
    if let Some(dup_idx) = unsafe { bcache_find_locked(&state, bc, lba) } {
        let dup = block_at(bc, dup_idx);
        unsafe {
            (*e).ref_count.store(0, Ordering::Relaxed);
            (*e).lba.store(u64::MAX, Ordering::Relaxed);
            set_block_valid(e, false);
            block_lru_insert_front(&mut state, bc, idx);
            (*dup).ref_count.fetch_add(1, Ordering::Relaxed);
            block_lru_remove(&mut state, bc, dup_idx);
            block_lru_insert_front(&mut state, bc, dup_idx);
        }
        return dup.cast::<bcache_entry>();
    }

    unsafe {
        (*e).lba.store(lba, Ordering::Relaxed);
        bcache_set_block_dirty_locked(&mut state, e, false);
        (*e).dirty_gen.store(0, Ordering::Relaxed);
        set_block_valid(e, true);
        block_hash_insert(&mut state, bc, idx);
        block_lru_insert_front(&mut state, bc, idx);
    }
    e.cast::<bcache_entry>()
}

#[no_mangle]
pub unsafe extern "C" fn bcache_release(e: *mut bcache_entry) {
    if e.is_null() {
        return;
    }
    let e = block_from_opaque(e);
    if unsafe { (*e).ref_count.load(Ordering::Relaxed) > 0 } {
        unsafe {
            (*e).ref_count.fetch_sub(1, Ordering::Release);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn bcache_mark_dirty(e: *mut bcache_entry) {
    if e.is_null() {
        return;
    }
    let e = block_from_opaque(e);
    unsafe {
        (*e).dirty_gen.fetch_add(1, Ordering::Release);
        set_block_dirty_flag(e, true);
    }
}

#[no_mangle]
pub unsafe extern "C" fn bcache_sync(bc: *mut bcache) {
    if bc.is_null() {
        return;
    }
    let bc = bcache_from_opaque(bc);
    if unsafe { (*bc).dev.is_null() } {
        return;
    }

    let mut tmp = [0u8; BCACHE_BLOCK_SIZE];
    let mut page_tmp: *mut u8 = ptr::null_mut();

    for i in 0..unsafe { (*bc).page_pool_size as u32 } {
        let page = page_at(bc, i);
        let (page_no, dirty_gen) = {
            let state = unsafe { (*bc).state.lock() };
            if state.dirty_pages == 0 {
                break;
            }
            if !page_valid(page) || !page_dirty(page) {
                continue;
            }
            if page_tmp.is_null() {
                page_tmp = unsafe { ffi::kmalloc(PCACHE_PAGE_SIZE) }.cast::<u8>();
                if page_tmp.is_null() {
                    break;
                }
            }
            unsafe {
                ffi::memcpy(
                    page_tmp.cast::<c_void>(),
                    (*page).data.get().cast::<c_void>(),
                    PCACHE_PAGE_SIZE,
                );
            }
            (
                unsafe { (*page).page_no.load(Ordering::Relaxed) },
                unsafe { (*page).dirty_gen.load(Ordering::Relaxed) },
            )
        };

        let lba = (page_no * PCACHE_PAGE_SIZE as u64) / BCACHE_BLOCK_SIZE as u64;
        if unsafe {
            ffi::a20_block_dev_write((*bc).dev, lba, page_tmp.cast::<c_void>(), PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE)
        } >= 0
        {
            let mut state = unsafe { (*bc).state.lock() };
            if page_valid(page)
                && unsafe { (*page).page_no.load(Ordering::Relaxed) == page_no }
                && unsafe { (*page).dirty_gen.load(Ordering::Relaxed) == dirty_gen }
            {
                unsafe { bcache_set_page_dirty_locked(&mut state, page, false) };
            }
        }
    }

    if !page_tmp.is_null() {
        unsafe { ffi::kfree(page_tmp.cast::<c_void>()) };
    }

    for i in 0..unsafe { (*bc).pool_size as u32 } {
        let block = block_at(bc, i);
        let (lba, dirty_gen) = {
            let state = unsafe { (*bc).state.lock() };
            if state.dirty_blocks == 0 {
                break;
            }
            if !block_valid(block) || !block_dirty(block) {
                continue;
            }
            unsafe {
                ffi::memcpy(
                    tmp.as_mut_ptr().cast::<c_void>(),
                    (*block).data.get().cast::<c_void>(),
                    BCACHE_BLOCK_SIZE,
                );
            }
            (
                unsafe { (*block).lba.load(Ordering::Relaxed) },
                unsafe { (*block).dirty_gen.load(Ordering::Relaxed) },
            )
        };

        if unsafe {
            ffi::a20_block_dev_write((*bc).dev, lba, tmp.as_ptr().cast::<c_void>(), 1)
        } >= 0
        {
            let mut state = unsafe { (*bc).state.lock() };
            if block_valid(block)
                && unsafe { (*block).lba.load(Ordering::Relaxed) == lba }
                && unsafe { (*block).dirty_gen.load(Ordering::Relaxed) == dirty_gen }
            {
                unsafe { bcache_set_block_dirty_locked(&mut state, block, false) };
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn bcache_invalidate(bc: *mut bcache, lba: u64) {
    if bc.is_null() {
        return;
    }
    let bc = bcache_from_opaque(bc);
    let mut state = unsafe { (*bc).state.lock() };
    if let Some(idx) = unsafe { bcache_find_locked(&state, bc, lba) } {
        let e = block_at(bc, idx);
        unsafe {
            block_hash_remove(&mut state, bc, idx);
            set_block_valid(e, false);
            bcache_set_block_dirty_locked(&mut state, e, false);
        }
    }
}

unsafe fn pcache_get(bc: *mut Bcache, page_no: u64, skip_read: bool) -> *mut PageEntry {
    let mut state = unsafe { (*bc).state.lock() };
    if let Some(idx) = unsafe { pcache_find_locked(&state, bc, page_no) } {
        let e = page_at(bc, idx);
        unsafe {
            (*e).ref_count.fetch_add(1, Ordering::Relaxed);
            page_lru_remove(&mut state, bc, idx);
            page_lru_insert_front(&mut state, bc, idx);
        }
        return e;
    }

    let idx = match unsafe { pcache_evict_locked(&mut state, bc) } {
        Some(idx) => idx,
        None => return ptr::null_mut(),
    };
    let e = page_at(bc, idx);
    let old_page = unsafe { (*e).page_no.load(Ordering::Relaxed) };
    let old_dirty = page_dirty(e) && page_valid(e);
    unsafe { set_page_valid(e, false) };
    drop(state);

    if old_dirty && unsafe { pcache_flush_page(bc, e) } < 0 {
        let mut state = unsafe { (*bc).state.lock() };
        unsafe {
            (*e).page_no.store(old_page, Ordering::Relaxed);
            set_page_valid(e, true);
            bcache_set_page_dirty_locked(&mut state, e, true);
            (*e).ref_count.store(0, Ordering::Relaxed);
            page_hash_insert(&mut state, bc, idx);
            page_lru_insert_front(&mut state, bc, idx);
        }
        return ptr::null_mut();
    }

    if skip_read {
    } else if unsafe { !(*bc).dev.is_null() } {
        let lba = (page_no * PCACHE_PAGE_SIZE as u64) / BCACHE_BLOCK_SIZE as u64;
        if unsafe {
            ffi::a20_block_dev_read(
                (*bc).dev,
                lba,
                (*e).data.get().cast::<c_void>(),
                PCACHE_PAGE_SIZE / BCACHE_BLOCK_SIZE,
            )
        } < 0
        {
            let mut state = unsafe { (*bc).state.lock() };
            unsafe {
                (*e).ref_count.store(0, Ordering::Relaxed);
                (*e).page_no.store(u64::MAX, Ordering::Relaxed);
                page_lru_insert_front(&mut state, bc, idx);
            }
            return ptr::null_mut();
        }
    } else {
        unsafe { ffi::memset((*e).data.get().cast::<c_void>(), 0, PCACHE_PAGE_SIZE) };
    }

    let mut state = unsafe { (*bc).state.lock() };
    if let Some(dup_idx) = unsafe { pcache_find_locked(&state, bc, page_no) } {
        let dup = page_at(bc, dup_idx);
        unsafe {
            (*e).ref_count.store(0, Ordering::Relaxed);
            (*e).page_no.store(u64::MAX, Ordering::Relaxed);
            set_page_valid(e, false);
            page_lru_insert_front(&mut state, bc, idx);
            (*dup).ref_count.fetch_add(1, Ordering::Relaxed);
            page_lru_remove(&mut state, bc, dup_idx);
            page_lru_insert_front(&mut state, bc, dup_idx);
        }
        return dup;
    }

    unsafe {
        (*e).page_no.store(page_no, Ordering::Relaxed);
        bcache_set_page_dirty_locked(&mut state, e, false);
        (*e).dirty_gen.store(0, Ordering::Relaxed);
        set_page_valid(e, true);
        page_hash_insert(&mut state, bc, idx);
        page_lru_insert_front(&mut state, bc, idx);
    }
    e
}

unsafe fn pcache_release(e: *mut PageEntry) {
    if e.is_null() {
        return;
    }
    if unsafe { (*e).ref_count.load(Ordering::Relaxed) > 0 } {
        unsafe {
            (*e).ref_count.fetch_sub(1, Ordering::Release);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn bcache_read_bytes(
    bc: *mut bcache,
    mut byte_off: u64,
    buf: *mut c_void,
    mut len: usize,
) -> c_int {
    if len == 0 {
        return 0;
    }
    if bc.is_null() || buf.is_null() {
        return -1;
    }
    let bc = bcache_from_opaque(bc);
    let mut dst = buf.cast::<u8>();
    let first_page = byte_off / PCACHE_PAGE_SIZE as u64;
    let last_page = (byte_off + len as u64 - 1) / PCACHE_PAGE_SIZE as u64;
    let sequential = (last_page - first_page + 1) >= 2;

    while len > 0 {
        let page_no = byte_off / PCACHE_PAGE_SIZE as u64;
        let off = (byte_off % PCACHE_PAGE_SIZE as u64) as usize;
        let mut chunk = PCACHE_PAGE_SIZE - off;
        if chunk > len {
            chunk = len;
        }

        let e = unsafe { pcache_get(bc, page_no, false) };
        if e.is_null() {
            return -1;
        }
        unsafe {
            ffi::memcpy(
                dst.cast::<c_void>(),
                (*e).data.get().cast::<u8>().add(off).cast::<c_void>(),
                chunk,
            );
            pcache_release(e);
        }

        unsafe {
            dst = dst.add(chunk);
        }
        byte_off += chunk as u64;
        len -= chunk;
    }

    if sequential {
        let ra_end = last_page + 1;
        let mut pn = last_page + 1;
        while pn <= ra_end {
            let e = unsafe { pcache_get(bc, pn, false) };
            if !e.is_null() {
                unsafe { pcache_release(e) };
            }
            pn += 1;
        }
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn bcache_write_bytes(
    bc: *mut bcache,
    mut byte_off: u64,
    buf: *const c_void,
    mut len: usize,
) -> c_int {
    if len == 0 {
        return 0;
    }
    if bc.is_null() || buf.is_null() {
        return -1;
    }
    let bc = bcache_from_opaque(bc);
    let mut src = buf.cast::<u8>();

    while len > 0 {
        let page_no = byte_off / PCACHE_PAGE_SIZE as u64;
        let off = (byte_off % PCACHE_PAGE_SIZE as u64) as usize;
        let mut chunk = PCACHE_PAGE_SIZE - off;
        if chunk > len {
            chunk = len;
        }

        let full_page_overwrite = off == 0 && chunk == PCACHE_PAGE_SIZE;
        let e = unsafe { pcache_get(bc, page_no, full_page_overwrite) };
        if e.is_null() {
            return -1;
        }
        unsafe {
            ffi::memcpy(
                (*e).data.get().cast::<u8>().add(off).cast::<c_void>(),
                src.cast::<c_void>(),
                chunk,
            );
        }
        {
            let mut state = unsafe { (*bc).state.lock() };
            unsafe { bcache_set_page_dirty_locked(&mut state, e, true) };
        }
        unsafe { pcache_release(e) };

        unsafe {
            src = src.add(chunk);
        }
        byte_off += chunk as u64;
        len -= chunk;
    }

    0
}

#[no_mangle]
pub unsafe extern "C" fn bcache_get_stats(stats: *mut bcache_stats_t) {
    if stats.is_null() {
        return;
    }

    unsafe {
        ffi::memset(stats.cast::<c_void>(), 0, size_of::<bcache_stats_t>());
    }

    let mut caches = [ptr::null_mut(); 8];
    let count = {
        let global = GLOBAL_STATE.lock();
        let count = global.count;
        let mut i = 0usize;
        while i < count {
            caches[i] = global.list[i] as *mut Bcache;
            i += 1;
        }
        count
    };

    unsafe {
        (*stats).caches = count;
    }

    let mut i = 0usize;
    while i < count {
        let bc = caches[i];
        if !bc.is_null() {
            unsafe {
                (*stats).block_pool_bytes += (*bc).pool_size as usize * BCACHE_BLOCK_SIZE;
                (*stats).page_pool_bytes += (*bc).page_pool_size as usize * PCACHE_PAGE_SIZE;
            }
            let _state = unsafe { (*bc).state.lock() };
            let mut j = 0u32;
            while j < unsafe { (*bc).pool_size as u32 } {
                let e = block_at(bc, j);
                if block_valid(e) {
                    unsafe {
                        (*stats).valid_blocks += 1;
                        if block_dirty(e) {
                            (*stats).dirty_blocks += 1;
                        }
                    }
                }
                j += 1;
            }
            let mut k = 0u32;
            while k < unsafe { (*bc).page_pool_size as u32 } {
                let e = page_at(bc, k);
                if page_valid(e) {
                    unsafe {
                        (*stats).valid_pages += 1;
                        if page_dirty(e) {
                            (*stats).dirty_pages += 1;
                        }
                    }
                }
                k += 1;
            }
        }
        i += 1;
    }
}
