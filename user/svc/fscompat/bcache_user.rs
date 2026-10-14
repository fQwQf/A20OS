//! Fixed-size page-cache policy for the user-space filesystem host.
//!
//! Rust owns page identity, references, journal holds, dirty generations,
//! replacement, writeback and retry state. The C adapter owns only sector I/O.

use core::ffi::c_void;
use core::ptr;

const PAGE_BYTES: usize = 4096;
const SECTOR_BYTES: usize = 512;
const SECTORS_PER_PAGE: usize = PAGE_BYTES / SECTOR_BYTES;
const CACHE_COUNT: usize = 8;
const PAGE_COUNT: usize = 256;

#[derive(Clone, Copy)]
struct Page {
    number: u64,
    generation: u64,
    refs: u16,
    valid: u8,
    dirty: u8,
    held: u8,
    accessed: u8,
    data: [u8; PAGE_BYTES],
}

const EMPTY_PAGE: Page = Page {
    number: u64::MAX, generation: 0, refs: 0, valid: 0, dirty: 0,
    held: 0, accessed: 0, data: [0; PAGE_BYTES],
};

struct Cache {
    dev: *mut c_void,
    pages: *mut Page,
    clock: u64,
    hand: usize,
    held_pages: [u64; PAGE_COUNT],
    held_count: usize,
    checkpoint_pages: [u64; PAGE_COUNT],
    checkpoint_count: usize,
    hold_failed: u8,
    checkpoint_failed: u8,
}

impl Cache {
    const EMPTY: Self = Self {
        dev: ptr::null_mut(), pages: ptr::null_mut(), clock: 0, hand: 0,
        held_pages: [u64::MAX; PAGE_COUNT], held_count: 0,
        checkpoint_pages: [u64::MAX; PAGE_COUNT], checkpoint_count: 0,
        hold_failed: 0, checkpoint_failed: 0,
    };
}

static mut CACHES: [Cache; CACHE_COUNT] = [const { Cache::EMPTY }; CACHE_COUNT];

unsafe extern "C" {
    fn a20_malloc(size: u64) -> *mut c_void;
    fn a20_free(ptr: *mut c_void);
    fn a20_ubc_read_sector(dev: *mut c_void, lba: u64, out: *mut u8,
                           count: u32) -> i32;
    fn a20_ubc_write_sector(dev: *mut c_void, lba: u64, data: *const u8,
                            count: u32) -> i32;
    fn a20_ubc_flush_device(dev: *mut c_void, has_flush: *mut i32) -> i32;
    fn a20_ubc_test_fail_checkpoint_flush() -> i32;
}

#[inline]
unsafe fn cache(index: usize) -> Option<&'static mut Cache> {
    if index >= CACHE_COUNT { return None; }
    let slot = unsafe { &mut *core::ptr::addr_of_mut!(CACHES).cast::<Cache>().add(index) };
    if slot.pages.is_null() || slot.dev.is_null() { None } else { Some(slot) }
}

#[inline]
unsafe fn page_at(cache: &mut Cache, index: usize) -> &mut Page {
    unsafe { &mut *cache.pages.add(index) }
}

unsafe fn held_contains(cache: &Cache, number: u64) -> bool {
    (0..cache.held_count).any(|n| unsafe {
        ptr::addr_of!(cache.held_pages).cast::<u64>().add(n).read() == number
    })
}

unsafe fn flush_page(cache: &mut Cache, index: usize, checkpoint: bool) -> i32 {
    let dev = cache.dev;
    let page = unsafe { page_at(cache, index) };
    if page.valid == 0 || page.dirty == 0 { return 0; }
    if page.held != 0 || page.refs != 0 { return -16; }
    let generation = page.generation;
    let number = page.number;
    let result = unsafe {
        a20_ubc_write_sector(dev, number * SECTORS_PER_PAGE as u64,
                             page.data.as_ptr(), SECTORS_PER_PAGE as u32)
    };
    let result = if result == 0 && checkpoint &&
        unsafe { a20_ubc_test_fail_checkpoint_flush() } != 0 {
        -5
    } else if result == 0 {
        let mut has_flush = 0;
        let flush_result = unsafe { a20_ubc_flush_device(dev, &mut has_flush) };
        if has_flush == 0 {
            if checkpoint { -95 } else { 0 }
        } else { flush_result }
    } else { result };
    /* Legacy block_dev_t.flush is optional for ordinary cache write-through:
     * no callback means writes retain the C reference's no-extra-barrier
     * behavior. Journal checkpoints require a real device barrier and must
     * propagate EOPNOTSUPP instead of claiming the commit is durable. */
    if result == 0 && page.valid != 0 && page.number == number &&
       page.generation == generation {
        page.dirty = 0;
    }
    result
}

unsafe fn find_page(cache: &mut Cache, number: u64) -> Option<usize> {
    for i in 0..PAGE_COUNT {
        let page = unsafe { page_at(cache, i) };
        if page.valid != 0 && page.number == number {
            page.accessed = 1;
            page.refs = page.refs.saturating_add(1);
            return Some(i);
        }
    }
    None
}

unsafe fn get_page(cache: &mut Cache, number: u64, full_overwrite: bool) -> Option<usize> {
    if let Some(index) = unsafe { find_page(cache, number) } { return Some(index); }
    let mut chosen = None;
    for _ in 0..(PAGE_COUNT * 2) {
        let index = cache.hand;
        cache.hand = (cache.hand + 1) % PAGE_COUNT;
        let page = unsafe { page_at(cache, index) };
        if page.valid == 0 { chosen = Some(index); break; }
        if page.refs != 0 || page.held != 0 { continue; }
        if page.accessed != 0 {
            page.accessed = 0;
            continue;
        }
        if page.dirty != 0 && unsafe { flush_page(cache, index, false) } < 0 { continue; }
        chosen = Some(index);
        break;
    }
    let index = chosen?;
    let dev = cache.dev;
    let held = unsafe { held_contains(cache, number) };
    let page = unsafe { page_at(cache, index) };
    page.valid = 0;
    page.dirty = 0;
    page.held = 0;
    page.refs = 0;
    if !full_overwrite {
        let result = unsafe {
            a20_ubc_read_sector(dev, number * SECTORS_PER_PAGE as u64,
                                page.data.as_mut_ptr(), SECTORS_PER_PAGE as u32)
        };
        if result < 0 { return None; }
    }
    page.number = number;
    page.generation = page.generation.wrapping_add(1);
    page.valid = 1;
    page.held = u8::from(held);
    page.accessed = 1;
    page.refs = 1;
    Some(index)
}

unsafe fn release_page(cache: &mut Cache, index: usize) {
    let page = unsafe { page_at(cache, index) };
    if page.refs != 0 { page.refs -= 1; }
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_create(index: u32, dev: *mut c_void) -> i32 {
    let i = index as usize;
    if i >= CACHE_COUNT || dev.is_null() { return -22; }
    let slot = unsafe { &mut *core::ptr::addr_of_mut!(CACHES).cast::<Cache>().add(i) };
    if !slot.pages.is_null() { return -16; }
    let bytes = core::mem::size_of::<Page>() * PAGE_COUNT;
    let pages = unsafe { a20_malloc(bytes as u64) }.cast::<Page>();
    if pages.is_null() { return -12; }
    for n in 0..PAGE_COUNT { unsafe { pages.add(n).write(EMPTY_PAGE) }; }
    slot.dev = dev;
    slot.pages = pages;
    slot.clock = 0;
    slot.hand = 0;
    slot.held_count = 0;
    slot.hold_failed = 0;
    slot.checkpoint_failed = 0;
    for n in 0..PAGE_COUNT {
        unsafe {
            core::ptr::addr_of_mut!(slot.held_pages).cast::<u64>().add(n).write(u64::MAX);
            core::ptr::addr_of_mut!(slot.checkpoint_pages).cast::<u64>().add(n).write(u64::MAX);
        }
    }
    slot.checkpoint_count = 0;
    slot.checkpoint_pages.fill(u64::MAX);
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_destroy(index: u32) {
    let i = index as usize;
    if i >= CACHE_COUNT { return; }
    let slot = unsafe { &mut *core::ptr::addr_of_mut!(CACHES).cast::<Cache>().add(i) };
    if slot.pages.is_null() { return; }
    // Do not discard dirty data on teardown. A subsequent sync can retry it;
    // the caller reports its sync failure before asking the backend to unmount.
    for n in 0..PAGE_COUNT {
        let _ = unsafe { flush_page(slot, n, false) };
    }
    let dirty_left = (0..PAGE_COUNT).any(|n| unsafe { page_at(slot, n).dirty != 0 });
    if dirty_left { return; }
    unsafe { a20_free(slot.pages.cast()) };
    *slot = Cache::EMPTY;
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_read(index: u32, byte_off: u64,
                                       out: *mut u8, len: usize) -> i32 {
    let Some(cache) = (unsafe { cache(index as usize) }) else { return -22; };
    if len == 0 { return 0; }
    if out.is_null() || byte_off.checked_add(len as u64).is_none() { return -22; }
    let mut offset = byte_off;
    let mut done = 0usize;
    while done < len {
        let page_no = offset / PAGE_BYTES as u64;
        let in_page = (offset % PAGE_BYTES as u64) as usize;
        let count = core::cmp::min(PAGE_BYTES - in_page, len - done);
        let Some(page_index) = (unsafe { get_page(cache, page_no, false) }) else {
            return -5;
        };
        let page = unsafe { page_at(cache, page_index) };
        unsafe { ptr::copy_nonoverlapping(page.data.as_ptr().add(in_page), out.add(done), count); }
        unsafe { release_page(cache, page_index); }
        done += count;
        offset += count as u64;
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_write(index: u32, byte_off: u64,
                                        input: *const u8, len: usize) -> i32 {
    let Some(cache) = (unsafe { cache(index as usize) }) else { return -22; };
    if len == 0 { return 0; }
    if input.is_null() || byte_off.checked_add(len as u64).is_none() { return -22; }
    if cache.hold_failed != 0 { return -28; }
    let mut offset = byte_off;
    let mut done = 0usize;
    while done < len {
        let page_no = offset / PAGE_BYTES as u64;
        let in_page = (offset % PAGE_BYTES as u64) as usize;
        let count = core::cmp::min(PAGE_BYTES - in_page, len - done);
        let full = in_page == 0 && count == PAGE_BYTES;
        let Some(page_index) = (unsafe { get_page(cache, page_no, full) }) else {
            return -5;
        };
        let page = unsafe { page_at(cache, page_index) };
        unsafe { ptr::copy_nonoverlapping(input.add(done), page.data.as_mut_ptr().add(in_page), count); }
        page.generation = page.generation.wrapping_add(1);
        page.dirty = 1;
        let held = page.held != 0;
        unsafe { release_page(cache, page_index); }
        // Preserve the C adapter's synchronous-write contract for ordinary
        // filesystem writes. Journal metadata is pre-held by ext4's Rust-host
        // adapter, so only checkpoint/replay may write those pages home.
        if !held {
            let result = unsafe { flush_page(cache, page_index, false) };
            if result < 0 { return result; }
        }
        done += count;
        offset += count as u64;
    }
    0
}

unsafe fn flush_matching(cache: &mut Cache, mode: u32, pages: *const u64,
                         count: usize) -> i32 {
    if mode == 1 && cache.checkpoint_failed != 0 { return -28; }
    let mut first_error = 0;
    for i in 0..PAGE_COUNT {
        let (valid, dirty, held, refs, number) = {
            let page = unsafe { page_at(cache, i) };
            (page.valid, page.dirty, page.held, page.refs, page.number)
        };
        if valid == 0 || dirty == 0 { continue; }
        let selected = match mode {
            1 => (0..cache.checkpoint_count).any(|n| unsafe {
                core::ptr::addr_of!(cache.checkpoint_pages).cast::<u64>().add(n).read() == number
            }),
            2 => held == 0 && (count == 0 || (0..count).any(|n| unsafe { *pages.add(n) == number })),
            _ => held == 0,
        };
        if !selected { continue; }
        if held != 0 || refs != 0 {
            if first_error == 0 { first_error = -16; }
            continue;
        }
        let result = unsafe { flush_page(cache, i, mode == 1) };
        if result < 0 { first_error = result; break; }
    }
    if mode == 1 && first_error == 0 {
        cache.checkpoint_count = 0;
        for n in 0..PAGE_COUNT {
            unsafe { core::ptr::addr_of_mut!(cache.checkpoint_pages).cast::<u64>().add(n).write(u64::MAX); }
        }
        cache.checkpoint_failed = 0;
    }
    first_error
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_sync(index: u32, mode: u32,
                                       pages: *const u64, count: usize) -> i32 {
    let Some(cache) = (unsafe { cache(index as usize) }) else { return -22; };
    if count != 0 && pages.is_null() { return -22; }
    unsafe { flush_matching(cache, mode, pages, count) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_hold(index: u32, page_no: u64) {
    let Some(cache) = (unsafe { cache(index as usize) }) else { return; };
    if unsafe { held_contains(cache, page_no) } { return; }
    if cache.held_count >= PAGE_COUNT {
        cache.hold_failed = 1;
        return;
    }
    unsafe {
        core::ptr::addr_of_mut!(cache.held_pages).cast::<u64>()
            .add(cache.held_count).write(page_no);
    }
    cache.held_count += 1;
    for n in 0..PAGE_COUNT {
        let page = unsafe { page_at(cache, n) };
        if page.valid != 0 && page.number == page_no {
            page.held = 1;
            return;
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_release_holds(index: u32) {
    let Some(cache) = (unsafe { cache(index as usize) }) else { return; };
    cache.checkpoint_failed = cache.hold_failed;
    cache.hold_failed = 0;
    for n in 0..PAGE_COUNT { unsafe { page_at(cache, n).held = 0; } }
    for n in 0..cache.held_count {
        unsafe {
            let value = core::ptr::addr_of!(cache.held_pages).cast::<u64>().add(n).read();
            let exists = (0..cache.checkpoint_count).any(|k| {
                core::ptr::addr_of!(cache.checkpoint_pages).cast::<u64>().add(k).read() == value
            });
            if !exists && cache.checkpoint_count < PAGE_COUNT {
                core::ptr::addr_of_mut!(cache.checkpoint_pages).cast::<u64>()
                    .add(cache.checkpoint_count).write(value);
                cache.checkpoint_count += 1;
            } else if !exists {
                cache.checkpoint_failed = 1;
            }
        }
    }
    for n in 0..PAGE_COUNT {
        unsafe { core::ptr::addr_of_mut!(cache.held_pages).cast::<u64>().add(n).write(u64::MAX); }
    }
    cache.held_count = 0;
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_held(index: u32) -> usize {
    let Some(cache) = (unsafe { cache(index as usize) }) else { return 0; };
    (0..PAGE_COUNT).filter(|n| unsafe {
        let page = page_at(cache, *n);
        page.valid != 0 && page.held != 0
    }).count()
}

#[no_mangle]
pub unsafe extern "C" fn a20_ubc_invalidate(index: u32, page_no: u64) {
    let Some(cache) = (unsafe { cache(index as usize) }) else { return; };
    for n in 0..PAGE_COUNT {
        let page = unsafe { page_at(cache, n) };
        if page.valid != 0 && page.number == page_no && page.refs == 0 {
            page.valid = 0;
            page.dirty = 0;
            page.held = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    static LOCK: Mutex<()> = Mutex::new(());
    static mut DISK: [[u8; PAGE_BYTES]; PAGE_COUNT * 2] = [[0; PAGE_BYTES]; PAGE_COUNT * 2];
    static mut FAIL_WRITE: bool = false;
    static mut FAIL_FLUSH: bool = false;
    static mut UNSUPPORTED_FLUSH: bool = false;
    static mut ABSENT_FLUSH: bool = false;
    static mut WRITE_CALLS: usize = 0;
    static mut FLUSH_CALLS: usize = 0;

    #[no_mangle]
    unsafe extern "C" fn a20_malloc(size: u64) -> *mut c_void {
        let layout = std::alloc::Layout::from_size_align(size as usize, 16).unwrap();
        std::alloc::alloc_zeroed(layout).cast()
    }
    #[no_mangle]
    unsafe extern "C" fn a20_free(ptr: *mut c_void) {
        let layout = std::alloc::Layout::from_size_align(
            core::mem::size_of::<Page>() * PAGE_COUNT, 16).unwrap();
        std::alloc::dealloc(ptr.cast(), layout);
    }
    #[no_mangle]
    unsafe extern "C" fn a20_ubc_read_sector(_: *mut c_void, lba: u64,
                                                out: *mut u8, count: u32) -> i32 {
        let offset = lba as usize * SECTOR_BYTES;
        unsafe { ptr::copy_nonoverlapping(ptr::addr_of!(DISK).cast::<u8>().add(offset), out,
                                          count as usize * SECTOR_BYTES); }
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_ubc_write_sector(_: *mut c_void, lba: u64,
                                                 input: *const u8, count: u32) -> i32 {
        unsafe { WRITE_CALLS += 1; }
        if unsafe { FAIL_WRITE } { return -5; }
        let offset = lba as usize * SECTOR_BYTES;
        unsafe { ptr::copy_nonoverlapping(input, ptr::addr_of_mut!(DISK).cast::<u8>().add(offset),
                                          count as usize * SECTOR_BYTES); }
        0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_ubc_flush_device(_: *mut c_void,
                                                has_flush: *mut i32) -> i32 {
        unsafe { FLUSH_CALLS += 1; }
        if unsafe { ABSENT_FLUSH } { unsafe { has_flush.write(0) }; -95 }
        else if unsafe { FAIL_FLUSH } { unsafe { has_flush.write(1) }; -5 }
        else if unsafe { UNSUPPORTED_FLUSH } { unsafe { has_flush.write(1) }; -95 }
        else { unsafe { has_flush.write(1) }; 0 }
    }
    #[no_mangle]
    extern "C" fn a20_ubc_test_fail_checkpoint_flush() -> i32 { 0 }

    fn setup(slot: u32) {
        unsafe { WRITE_CALLS = 0; FLUSH_CALLS = 0; FAIL_WRITE = false;
                 FAIL_FLUSH = false; UNSUPPORTED_FLUSH = false; ABSENT_FLUSH = false; }
        assert_eq!(unsafe { a20_ubc_create(slot, 1usize as *mut c_void) }, 0);
    }
    fn teardown(slot: u32) { unsafe { a20_ubc_destroy(slot); } }

    #[test]
    fn ordinary_write_is_persisted_before_return() {
        let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        setup(0);
        let source = [0x5a; PAGE_BYTES];
        assert_eq!(unsafe { a20_ubc_write(0, 3 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, 0);
        assert_eq!(unsafe { DISK[3] }, source);
        assert_eq!(unsafe { WRITE_CALLS }, 1);
        assert_eq!(unsafe { FLUSH_CALLS }, 1);
        let mut cached = [0; PAGE_BYTES];
        assert_eq!(unsafe { a20_ubc_read(0, 3 * PAGE_BYTES as u64,
                              cached.as_mut_ptr(), cached.len()) }, 0);
        assert_eq!(cached, source);
        assert_eq!(unsafe { DISK[3] }, source);
        teardown(0);
    }

    #[test]
    fn held_pages_are_not_evicted_or_generic_flushed() {
        let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        setup(1);
        let source = [0x3c; PAGE_BYTES];
        unsafe { a20_ubc_hold(1, 1); }
        assert_eq!(unsafe { a20_ubc_write(1, PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, 0);
        assert_eq!(unsafe { WRITE_CALLS }, 0, "journal-held page reached backend before checkpoint");
        assert_eq!(unsafe { a20_ubc_held(1) }, 1);
        assert_eq!(unsafe { a20_ubc_sync(1, 0, ptr::null(), 0) }, 0);
        assert_eq!(unsafe { DISK[1][0] }, 0);
        unsafe { a20_ubc_release_holds(1); }
        assert_eq!(unsafe { a20_ubc_sync(1, 1, ptr::null(), 0) }, 0);
        assert_eq!(unsafe { DISK[1] }, source);
        assert_eq!(unsafe { WRITE_CALLS }, 1);
        assert_eq!(unsafe { FLUSH_CALLS }, 1);
        teardown(1);
    }

    #[test]
    fn failed_writeback_keeps_dirty_copy_for_retry() {
        let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        setup(2);
        let source = [0xa6; PAGE_BYTES];
        unsafe { FAIL_WRITE = true; }
        assert_eq!(unsafe { a20_ubc_write(2, 2 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, -5);
        assert_eq!(unsafe { DISK[2][0] }, 0);
        unsafe { FAIL_WRITE = false; }
        let mut readback = [0; PAGE_BYTES];
        assert_eq!(unsafe { a20_ubc_read(2, 2 * PAGE_BYTES as u64,
                              readback.as_mut_ptr(), readback.len()) }, 0);
        assert_eq!(readback, source);
        assert_eq!(unsafe { a20_ubc_sync(2, 0, ptr::null(), 0) }, 0);
        assert_eq!(unsafe { DISK[2] }, source);
        assert_eq!(unsafe { WRITE_CALLS }, 2);
        teardown(2);
    }

    #[test]
    fn failed_device_flush_keeps_dirty_page_for_retry() {
        let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        setup(4);
        let source = [0xd2; PAGE_BYTES];
        unsafe { FAIL_FLUSH = true; }
        assert_eq!(unsafe { a20_ubc_write(4, 4 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, -5);
        assert_eq!(unsafe { WRITE_CALLS }, 1);
        assert_eq!(unsafe { FLUSH_CALLS }, 1);
        unsafe { FAIL_FLUSH = false; }
        assert_eq!(unsafe { a20_ubc_sync(4, 0, ptr::null(), 0) }, 0);
        assert_eq!(unsafe { DISK[4] }, source);
        assert_eq!(unsafe { WRITE_CALLS }, 2);
        assert_eq!(unsafe { FLUSH_CALLS }, 2);
        teardown(4);
    }

    #[test]
    fn absent_flush_is_legacy_noop_but_callback_errors_are_preserved() {
        let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        setup(6);
        let source = [0x4b; PAGE_BYTES];
        unsafe { ABSENT_FLUSH = true; }
        assert_eq!(unsafe { a20_ubc_write(6, 6 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, 0,
                   "only an absent callback gets the optional-flush legacy behavior");
        assert_eq!(unsafe { WRITE_CALLS }, 1);
        assert_eq!(unsafe { FLUSH_CALLS }, 1);

        unsafe { ABSENT_FLUSH = false; UNSUPPORTED_FLUSH = true; }
        assert_eq!(unsafe { a20_ubc_write(6, 8 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, -95,
                   "a present callback returning EOPNOTSUPP is a real failure");
        unsafe { UNSUPPORTED_FLUSH = false; FAIL_FLUSH = true; }
        assert_eq!(unsafe { a20_ubc_write(6, 9 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, -5,
                   "a present callback returning EIO remains a failure");
        unsafe { FAIL_FLUSH = false; }
        assert_eq!(unsafe { a20_ubc_write(6, 10 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, 0,
                   "successful callback is preserved");

        unsafe { a20_ubc_hold(6, 7); }
        assert_eq!(unsafe { a20_ubc_write(6, 7 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, 0);
        unsafe { a20_ubc_release_holds(6); ABSENT_FLUSH = true; }
        assert_eq!(unsafe { a20_ubc_sync(6, 1, ptr::null(), 0) }, -95,
                   "journal checkpoint cannot claim durability without a barrier");
        assert_eq!(unsafe { WRITE_CALLS }, 5);
        unsafe { ABSENT_FLUSH = false; }
        assert_eq!(unsafe { a20_ubc_sync(6, 1, ptr::null(), 0) }, 0);
        assert_eq!(unsafe { WRITE_CALLS }, 6,
                   "failed checkpoint retains the page set for a durable retry");
        teardown(6);
    }

    #[test]
    fn checkpoint_flush_failure_retains_checkpoint_set_for_retry() {
        let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        setup(5);
        let source = [0x91; PAGE_BYTES];
        unsafe { a20_ubc_hold(5, 5); }
        assert_eq!(unsafe { a20_ubc_write(5, 5 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, 0);
        unsafe { a20_ubc_release_holds(5); FAIL_FLUSH = true; }
        assert_eq!(unsafe { a20_ubc_sync(5, 1, ptr::null(), 0) }, -5);
        assert_eq!(unsafe { WRITE_CALLS }, 1);
        assert_eq!(unsafe { DISK[5] }, source);
        unsafe { FAIL_FLUSH = false; }
        assert_eq!(unsafe { a20_ubc_sync(5, 1, ptr::null(), 0) }, 0);
        assert_eq!(unsafe { WRITE_CALLS }, 2,
                   "retry must retain and rewrite the failed checkpoint page");
        assert_eq!(unsafe { FLUSH_CALLS }, 2);
        teardown(5);
    }

    #[test]
    fn dirty_eviction_writes_old_page_before_reuse() {
        let _guard = LOCK.lock().unwrap_or_else(|e| e.into_inner());
        setup(3);
        let source = [0x77; PAGE_BYTES];
        assert_eq!(unsafe { a20_ubc_write(3, 7 * PAGE_BYTES as u64,
                         source.as_ptr(), source.len()) }, 0);
        // Force replacement through the full pool.
        for p in PAGE_COUNT..(2 * PAGE_COUNT) {
            let mut one = [0u8; 1];
            assert_eq!(unsafe { a20_ubc_read(3, p as u64 * PAGE_BYTES as u64,
                              one.as_mut_ptr(), 1) }, 0);
        }
        assert_eq!(unsafe { DISK[7] }, source);
        teardown(3);
    }
}
