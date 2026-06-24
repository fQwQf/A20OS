#![no_std]

use core::ffi::{c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicBool, Ordering};

use a20rust_support::lock::{raw_irqsave_lock, spinlock_t};

use ffi::{
    FRAME_F_ALLOC, MAX_ORDER, PAGE_SIZE, PFN_NONE, BIG_MAGIC, SLAB_BITMAP_BITS,
    SLAB_BITMAP_WORDS, SLAB_HDR_SIZE, SLAB_MAGIC, SLAB_NR_CACHES, SLAB_SIZES,
    SLAB_SPARE_CAP, slab_stats_t,
};

mod ffi;

const SLAB_MAX_OBJ: usize = 2048;
const NONE16: u16 = u16::MAX;

const STATE_NONE: u8 = 0;
const STATE_PARTIAL: u8 = 1;
const STATE_FULL: u8 = 2;
const STATE_SPARE: u8 = 3;

#[repr(C)]
struct SlabPage {
    next: *mut SlabPage,
    prev: *mut SlabPage,
    in_use: u16,
    total: u16,
    free_head: u16,
    cache_idx: u8,
    state: u8,
    _pad: [u8; 2],
    magic: u32,
    alloc_bits: [u64; SLAB_BITMAP_WORDS],
}

#[repr(C)]
struct BigAllocHdr {
    magic: u32,
    order: u16,
    _pad: u16,
}

#[derive(Clone, Copy)]
struct Cache {
    obj_size: usize,
    objs_per_slab: u16,
    partial: *mut SlabPage,
    full: *mut SlabPage,
    spare: *mut SlabPage,
    spare_count: usize,
    lock: spinlock_t,
}

unsafe impl Send for Cache {}

static mut CACHES: [Cache; SLAB_NR_CACHES] = [Cache {
    obj_size: 0,
    objs_per_slab: 0,
    partial: ptr::null_mut(),
    full: ptr::null_mut(),
    spare: ptr::null_mut(),
    spare_count: 0,
    lock: spinlock_t { locked: 0 },
}; SLAB_NR_CACHES];

static INITIALIZED: AtomicBool = AtomicBool::new(false);

fn objs_per_slab(obj_size: usize) -> u16 {
    let n = (PAGE_SIZE - SLAB_HDR_SIZE) / obj_size;
    if n > SLAB_BITMAP_BITS {
        SLAB_BITMAP_BITS as u16
    } else {
        n as u16
    }
}

fn page_base_of(ptr: *mut c_void) -> *mut SlabPage {
    let addr = ptr as usize;
    let aligned = addr & !(PAGE_SIZE - 1);
    aligned as *mut SlabPage
}

fn bit_mask(idx: u16) -> u64 {
    1u64 << (idx & 63)
}

fn bit_test(sp: *const SlabPage, idx: u16) -> bool {
    unsafe { (*sp).alloc_bits[(idx >> 6) as usize] & bit_mask(idx) != 0 }
}

unsafe fn bit_set(sp: *mut SlabPage, idx: u16) {
    (*sp).alloc_bits[(idx >> 6) as usize] |= bit_mask(idx);
}

unsafe fn bit_clear(sp: *mut SlabPage, idx: u16) {
    (*sp).alloc_bits[(idx >> 6) as usize] &= !bit_mask(idx);
}

fn popcount64(v: u64) -> u16 {
    v.count_ones() as u16
}

fn in_use_from_bits(sp: *const SlabPage) -> u16 {
    unsafe {
        let mut n = 0u16;
        let mut i = 0usize;
        while i < SLAB_BITMAP_WORDS {
            n += popcount64((*sp).alloc_bits[i]);
            i += 1;
        }
        n
    }
}

fn page_valid(sp: *const SlabPage) -> bool {
    if sp.is_null() {
        return false;
    }
    unsafe {
        if (*sp).magic != SLAB_MAGIC {
            return false;
        }
        let idx = (*sp).cache_idx as usize;
        if idx >= SLAB_NR_CACHES {
            return false;
        }
        let state = (*sp).state;
        if state != STATE_PARTIAL && state != STATE_FULL && state != STATE_SPARE {
            return false;
        }
        let expected_total = CACHES[idx].objs_per_slab;
        if (*sp).total != expected_total {
            return false;
        }
        if (*sp).total > SLAB_BITMAP_BITS as u16 {
            return false;
        }
        if (*sp).in_use != in_use_from_bits(sp) {
            return false;
        }
    }
    true
}

unsafe fn list_remove(head: *mut *mut SlabPage, sp: *mut SlabPage) {
    if !(*sp).prev.is_null() {
        (*(*sp).prev).next = (*sp).next;
    } else {
        *head = (*sp).next;
    }
    if !(*sp).next.is_null() {
        (*(*sp).next).prev = (*sp).prev;
    }
    (*sp).prev = ptr::null_mut();
    (*sp).next = ptr::null_mut();
}

unsafe fn list_push(head: *mut *mut SlabPage, sp: *mut SlabPage) {
    (*sp).prev = ptr::null_mut();
    (*sp).next = *head;
    if !(*head).is_null() {
        (**head).prev = sp;
    }
    *head = sp;
}

unsafe fn grow(idx: usize) -> *mut SlabPage {
    let pfn = ffi::pfa_alloc_page();
    if pfn == PFN_NONE {
        return ptr::null_mut();
    }
    let page = ffi::a20_pfn_to_virt(pfn) as *mut SlabPage;
    if page.is_null() {
        ffi::a20_pfa_free_page(pfn);
        return ptr::null_mut();
    }

    ffi::memset(page as *mut c_void, 0, PAGE_SIZE);

    let total = CACHES[idx].objs_per_slab;
    (*page).cache_idx = idx as u8;
    (*page).in_use = 0;
    (*page).total = total;
    (*page).free_head = 0;
    (*page).state = STATE_NONE;
    (*page).magic = SLAB_MAGIC;

    if total == 0 {
        ffi::a20_pfa_free_page(pfn);
        return ptr::null_mut();
    }

    let base = page as usize + SLAB_HDR_SIZE;
    let obj_size = CACHES[idx].obj_size;
    let mut i = 0u16;
    while i + 1 < total {
        let obj = (base + i as usize * obj_size) as *mut u16;
        *obj = i + 1;
        i += 1;
    }
    let last = (base + (total - 1) as usize * obj_size) as *mut u16;
    *last = NONE16;

    page
}

unsafe fn spare_pop(c: *mut Cache) -> *mut SlabPage {
    let sp = (*c).spare;
    if sp.is_null() {
        return ptr::null_mut();
    }
    list_remove(&mut (*c).spare, sp);
    (*sp).state = STATE_NONE;
    (*c).spare_count -= 1;
    sp
}

unsafe fn spare_push(c: *mut Cache, sp: *mut SlabPage) {
    list_push(&mut (*c).spare, sp);
    (*c).spare_count += 1;
    (*sp).state = STATE_SPARE;
}

unsafe fn page_release(sp: *mut SlabPage) {
    let pfn = ffi::a20_virt_to_pfn(sp as *const c_void);
    (*sp).magic = 0;
    (*sp).state = STATE_NONE;
    (*sp).free_head = NONE16;
    (*sp).next = ptr::null_mut();
    (*sp).prev = ptr::null_mut();
    let mut i = 0usize;
    while i < SLAB_BITMAP_WORDS {
        (*sp).alloc_bits[i] = 0;
        i += 1;
    }
    if ffi::a20_pfn_valid(pfn) != 0 {
        ffi::a20_pfa_free_page(pfn);
    }
}

#[no_mangle]
pub unsafe extern "C" fn slab_init() {
    if core::mem::size_of::<SlabPage>() > SLAB_HDR_SIZE {
        ffi::panic(
            b"slab_init: slab header larger than SLAB_HDR_SIZE\0".as_ptr() as _);
    }

    let mut i = 0usize;
    while i < SLAB_NR_CACHES {
        let obj_size = SLAB_SIZES[i];
        let ops = objs_per_slab(obj_size);
        if ops as usize > SLAB_BITMAP_BITS {
            ffi::panic(b"slab_init: slab bitmap too small for cache\0".as_ptr() as _);
        }
        CACHES[i].obj_size = obj_size;
        CACHES[i].objs_per_slab = ops;
        CACHES[i].partial = ptr::null_mut();
        CACHES[i].full = ptr::null_mut();
        CACHES[i].spare = ptr::null_mut();
        CACHES[i].spare_count = 0;
        CACHES[i].lock.locked = 0;
        i += 1;
    }
    INITIALIZED.store(true, Ordering::Release);
}

unsafe fn cache_ptr(idx: usize) -> *mut Cache {
    ptr::addr_of_mut!(CACHES).cast::<Cache>().add(idx)
}

#[no_mangle]
pub unsafe extern "C" fn kmalloc(size: usize) -> *mut c_void {
    if size == 0 {
        return ptr::null_mut();
    }

    if size >= SLAB_MAX_OBJ {
        let need = (size + core::mem::size_of::<BigAllocHdr>() + PAGE_SIZE - 1) & !(PAGE_SIZE - 1);
        let mut order = 0;
        while ((1usize << order) * PAGE_SIZE) < need {
            order += 1;
        }
        if order > MAX_ORDER as usize {
            return ptr::null_mut();
        }
        let pfn = ffi::a20_pfa_alloc(order as c_int);
        if pfn == PFN_NONE {
            ffi::a20_oom_try_reclaim();
            return ptr::null_mut();
        }
        let hdr = ffi::a20_pfn_to_virt(pfn) as *mut BigAllocHdr;
        (*hdr).magic = BIG_MAGIC;
        (*hdr).order = order as u16;
        (*hdr)._pad = 0;
        return hdr.add(1) as *mut c_void;
    }

    let mut idx = 0usize;
    while idx < SLAB_NR_CACHES - 1 && SLAB_SIZES[idx] < size {
        idx += 1;
    }

    let c = cache_ptr(idx);
    let _g = raw_irqsave_lock(ptr::addr_of_mut!((*c).lock));

    let mut sp = (*c).partial;
    while !sp.is_null() {
        if (*sp).free_head != NONE16 {
            break;
        }
        list_remove(&mut (*c).partial, sp);
        list_push(&mut (*c).full, sp);
        (*sp).state = STATE_FULL;
        sp = (*c).partial;
    }

    if sp.is_null() {
        sp = spare_pop(c);
        if sp.is_null() {
            sp = grow(idx);
            if sp.is_null() {
                ffi::a20_oom_try_reclaim();
                return ptr::null_mut();
            }
        }
        (*sp).state = STATE_PARTIAL;
        list_push(&mut (*c).partial, sp);
    }

    if !page_valid(sp) {
        ffi::panic(b"kmalloc: invalid slab page\0".as_ptr() as _);
    }

    let obj_idx = (*sp).free_head;
    if obj_idx == NONE16 {
        ffi::panic(b"kmalloc: empty free_list\0".as_ptr() as _);
    }

    let base = sp as usize + SLAB_HDR_SIZE;
    let obj_size = (*c).obj_size;
    let obj = (base + obj_idx as usize * obj_size) as *mut c_void;
    let next = *(obj as *mut u16);

    if bit_test(sp, obj_idx) {
        ffi::panic(b"kmalloc: alloc_bits corrupted\0".as_ptr() as _);
    }

    bit_set(sp, obj_idx);
    (*sp).free_head = next;
    (*sp).in_use += 1;

    if (*sp).in_use == (*sp).total {
        list_remove(&mut (*c).partial, sp);
        (*sp).state = STATE_FULL;
        list_push(&mut (*c).full, sp);
    }

    obj
}

#[no_mangle]
pub unsafe extern "C" fn kfree(ptr: *mut c_void) {
    if ptr.is_null() {
        return;
    }

    let sp = page_base_of(ptr);
    let offset = ptr as usize - sp as usize;

    let bhdr = (ptr as *mut BigAllocHdr).offset(-1);
    if (*bhdr).magic == BIG_MAGIC && (*bhdr).order <= MAX_ORDER as u16 {
        let order = (*bhdr).order as c_int;
        let pfn = ffi::a20_virt_to_pfn(bhdr as *const c_void);
        if ffi::a20_pfn_valid(pfn) != 0
            && ffi::a20_pfa_meta_flags(pfn) == FRAME_F_ALLOC
            && ffi::a20_pfa_meta_refcount(pfn) > 0
            && (bhdr as usize) & (PAGE_SIZE - 1) == 0
        {
            ffi::a20_pfa_free(pfn, order);
            return;
        }
    }

    if !page_valid(sp) {
        ffi::panic(b"kfree: invalid pointer\0".as_ptr() as _);
    }

    if offset < SLAB_HDR_SIZE {
        ffi::panic(b"kfree: pointer inside slab header\0".as_ptr() as _);
    }

    let idx = (*sp).cache_idx as usize;
    let obj_size = CACHES[idx].obj_size;
    if (offset - SLAB_HDR_SIZE) % obj_size != 0 || offset >= PAGE_SIZE {
        ffi::panic(b"kfree: corrupted slab pointer\0".as_ptr() as _);
    }
    let obj_idx = ((offset - SLAB_HDR_SIZE) / obj_size) as u16;
    if !bit_test(sp, obj_idx) {
        ffi::panic(b"kfree: stale or double free\0".as_ptr() as _);
    }

    let c = cache_ptr(idx);
    let _g = raw_irqsave_lock(ptr::addr_of_mut!((*c).lock));

    bit_clear(sp, obj_idx);
    *(ptr as *mut u16) = (*sp).free_head;
    (*sp).free_head = obj_idx;
    (*sp).in_use -= 1;

    if (*sp).in_use == (*sp).total - 1 {
        list_remove(&mut (*c).full, sp);
        (*sp).state = STATE_PARTIAL;
        list_push(&mut (*c).partial, sp);
    }

    if (*sp).in_use == 0 {
        if (*sp).state == STATE_PARTIAL {
            list_remove(&mut (*c).partial, sp);
        } else if (*sp).state == STATE_FULL {
            list_remove(&mut (*c).full, sp);
        }
        if (*c).spare_count < SLAB_SPARE_CAP {
            spare_push(c, sp);
        } else {
            page_release(sp);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn krealloc(ptr: *mut c_void, new_size: usize) -> *mut c_void {
    if ptr.is_null() {
        return kmalloc(new_size);
    }
    if new_size == 0 {
        kfree(ptr);
        return ptr::null_mut();
    }

    let sp = page_base_of(ptr);
    let offset = ptr as usize - sp as usize;
    let old_size = if page_valid(sp) {
        if offset < SLAB_HDR_SIZE {
            ffi::panic(b"krealloc: pointer inside slab header\0".as_ptr() as _);
        }
        CACHES[(*sp).cache_idx as usize].obj_size
    } else {
        let hdr = (ptr as *mut BigAllocHdr).offset(-1);
        if (*hdr).magic != BIG_MAGIC || (*hdr).order > MAX_ORDER as u16 {
            ffi::panic(b"krealloc: invalid pointer\0".as_ptr() as _);
        }
        ((1usize << (*hdr).order) * PAGE_SIZE) - core::mem::size_of::<BigAllocHdr>()
    };

    if new_size <= old_size {
        return ptr;
    }

    let new_ptr = kmalloc(new_size);
    if new_ptr.is_null() {
        return ptr::null_mut();
    }
    ffi::memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
    new_ptr
}

#[no_mangle]
pub unsafe extern "C" fn kcalloc(nmemb: usize, size: usize) -> *mut c_void {
    let total = nmemb.wrapping_mul(size);
    if size != 0 && total / size != nmemb {
        return ptr::null_mut();
    }
    let p = kmalloc(total);
    if !p.is_null() {
        ffi::memset(p, 0, total);
    }
    p
}

#[no_mangle]
pub unsafe extern "C" fn slab_get_stats(stats: *mut slab_stats_t) {
    if stats.is_null() {
        return;
    }
    ffi::memset(stats as *mut c_void, 0, core::mem::size_of::<slab_stats_t>());

    let mut i = 0usize;
    while i < SLAB_NR_CACHES {
        let c = cache_ptr(i);
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*c).lock));

        let mut sp = (*c).partial;
        while !sp.is_null() {
            (*stats).total_pages += 1;
            (*stats).active_pages += 1;
            (*stats).allocated_objects += (*sp).in_use as usize;
            (*stats).allocated_bytes += (*sp).in_use as usize * (*c).obj_size;
            sp = (*sp).next;
        }

        sp = (*c).full;
        while !sp.is_null() {
            (*stats).total_pages += 1;
            (*stats).active_pages += 1;
            (*stats).allocated_objects += (*sp).in_use as usize;
            (*stats).allocated_bytes += (*sp).in_use as usize * (*c).obj_size;
            sp = (*sp).next;
        }

        sp = (*c).spare;
        while !sp.is_null() {
            (*stats).total_pages += 1;
            (*stats).spare_pages += 1;
            sp = (*sp).next;
        }

        i += 1;
    }

    (*stats).total_bytes = (*stats).total_pages * PAGE_SIZE;
    (*stats).reclaimable_bytes = (*stats).spare_pages * PAGE_SIZE;
}

#[no_mangle]
pub unsafe extern "C" fn slab_reclaim_spare() -> usize {
    let mut freed = 0usize;
    let mut i = 0usize;
    while i < SLAB_NR_CACHES {
        let c = cache_ptr(i);
        let _g = raw_irqsave_lock(ptr::addr_of_mut!((*c).lock));
        while !(*c).spare.is_null() {
            let sp = (*c).spare;
            list_remove(&mut (*c).spare, sp);
            (*c).spare_count -= 1;
            page_release(sp);
            freed += 1;
        }
        i += 1;
    }
    freed * PAGE_SIZE
}
