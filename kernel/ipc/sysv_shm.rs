#![no_std]

//! SysV shared-memory object identity and lifetime transactions.
//! Page allocation, page references, user copy, and the address-space mapping
//! remain narrow C mechanisms; Rust owns namespace/key/id and attach lifetime.

use core::ffi::c_void;
use core::mem::size_of;

const MAX: usize = 32;
const MAX_PAGES: usize = 1024;
const PAGE_SIZE: usize = 4096;
const IPC_CREAT: i32 = 0o1000;
const IPC_EXCL: i32 = 0o2000;
const IPC_64: i32 = 0x100;
const IPC_RMID: i32 = 0;
const IPC_STAT: i32 = 2;
const SHM_STAT: i32 = 13;
const SHM_INFO: i32 = 14;
const SHM_STAT_ANY: i32 = 15;
const EINVAL: i32 = 22;
const ENOENT: i32 = 2;
const ENOMEM: i32 = 12;
const ENOSPC: i32 = 28;
const EEXIST: i32 = 17;

#[repr(C)]
#[derive(Copy, Clone)]
pub struct SysvShm {
    used: i32,
    namespace_ino: u64,
    marked_delete: i32,
    key: i32,
    size: usize,
    pages: *mut u32,
    npages: usize,
    nattach: i32,
}

#[repr(C)]
#[derive(Copy, Clone)]
struct IpcPerm {
    key: i32,
    uid: u32,
    gid: u32,
    cuid: u32,
    cgid: u32,
    mode: u32,
    seq: i32,
    unused1: isize,
    unused2: isize,
}

#[repr(C)]
struct ShmidDs {
    perm: IpcPerm,
    segsz: usize,
    atime: isize,
    dtime: isize,
    ctime: isize,
    cpid: i32,
    lpid: i32,
    nattch: usize,
    pad1: usize,
    pad2: usize,
}

#[repr(C)]
struct ShmInfo {
    used_ids: i32,
    tot: usize,
    rss: usize,
    swp: usize,
    swap_attempts: usize,
    swap_successes: usize,
}

const _: () = assert!(size_of::<SysvShm>() == if usize::BITS == 64 { 56 } else { 40 });
const _: () = assert!(size_of::<ShmidDs>() == if usize::BITS == 64 { 112 } else { 72 });
const _: () = assert!(size_of::<ShmInfo>() == if usize::BITS == 64 { 48 } else { 24 });

unsafe extern "C" {
    fn a20_sysv_shm_lock(ctx: *mut c_void) -> u64;
    fn a20_sysv_shm_unlock(ctx: *mut c_void, flags: u64);
    fn a20_sysv_shm_namespace(ctx: *mut c_void) -> u64;
    fn a20_sysv_shm_alloc_pages(ctx: *mut c_void, npages: usize) -> *mut u32;
    fn a20_sysv_shm_free_pages(ctx: *mut c_void, pages: *mut u32, npages: usize);
    fn a20_sysv_shm_copy_out(ctx: *mut c_void, dst: *mut c_void, src: *const c_void, len: usize) -> i32;
    fn a20_sysv_shm_pid(ctx: *mut c_void) -> i32;
}

unsafe fn release(ctx: *mut c_void, pages: *mut u32, count: usize) {
    if !pages.is_null() { unsafe { a20_sysv_shm_free_pages(ctx, pages, count) }; }
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_shm_get_txn(
    table: *mut SysvShm, ctx: *mut c_void, key: i32, size: usize, flags: i32,
) -> i32 {
    if table.is_null() { return -EINVAL; }
    let namespace = unsafe { a20_sysv_shm_namespace(ctx) };
    let irq = unsafe { a20_sysv_shm_lock(ctx) };
    if key != 0 {
        for id in 0..MAX {
            let obj = unsafe { &*table.add(id) };
            if obj.used != 0 && obj.namespace_ino == namespace && obj.marked_delete == 0 && obj.key == key {
                let rc = if size > obj.size { -EINVAL } else if flags & IPC_CREAT != 0 && flags & IPC_EXCL != 0 { -EEXIST } else { id as i32 };
                unsafe { a20_sysv_shm_unlock(ctx, irq) };
                return rc;
            }
        }
    }
    if key != 0 && flags & IPC_CREAT == 0 {
        unsafe { a20_sysv_shm_unlock(ctx, irq) };
        return -ENOENT;
    }
    if size == 0 || size > MAX_PAGES * PAGE_SIZE {
        unsafe { a20_sysv_shm_unlock(ctx, irq) };
        return if size == 0 { -EINVAL } else { -ENOMEM };
    }
    let npages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    let mut slot = None;
    for id in 0..MAX { if unsafe { (*table.add(id)).used == 0 } { slot = Some(id); break; } }
    unsafe { a20_sysv_shm_unlock(ctx, irq) };
    let Some(slot) = slot else { return -ENOSPC; };
    let pages = unsafe { a20_sysv_shm_alloc_pages(ctx, npages) };
    if pages.is_null() { return -ENOMEM; }
    let irq = unsafe { a20_sysv_shm_lock(ctx) };
    if key != 0 {
        for id in 0..MAX {
            let obj = unsafe { &*table.add(id) };
            if obj.used != 0 && obj.namespace_ino == namespace && obj.marked_delete == 0 && obj.key == key {
                let rc = if size > obj.size { -EINVAL } else if flags & IPC_EXCL != 0 { -EEXIST } else { id as i32 };
                unsafe { a20_sysv_shm_unlock(ctx, irq); release(ctx, pages, npages); }
                return rc;
            }
        }
    }
    let actual = if unsafe { (*table.add(slot)).used == 0 } { slot } else {
        match (0..MAX).find(|id| unsafe { (*table.add(*id)).used == 0 }) {
            Some(id) => id,
            None => { unsafe { a20_sysv_shm_unlock(ctx, irq); release(ctx, pages, npages); } return -ENOSPC; }
        }
    };
    unsafe {
        *table.add(actual) = SysvShm { used: 1, namespace_ino: namespace, marked_delete: 0,
            key, size: npages * PAGE_SIZE, pages, npages, nattach: 0 };
        a20_sysv_shm_unlock(ctx, irq);
    }
    actual as i32
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_shm_control_txn(
    table: *mut SysvShm, ctx: *mut c_void, id: i32, command: i32, buffer: *mut c_void,
) -> i32 {
    if table.is_null() || id < 0 || id as usize >= MAX { return -EINVAL; }
    let cmd = command & !IPC_64;
    let irq = unsafe { a20_sysv_shm_lock(ctx) };
    let namespace = unsafe { a20_sysv_shm_namespace(ctx) };
    let obj = unsafe { &mut *table.add(id as usize) };
    if obj.used == 0 || obj.namespace_ino != namespace {
        unsafe { a20_sysv_shm_unlock(ctx, irq) };
        return -EINVAL;
    }
    if cmd == IPC_RMID {
        if obj.nattach > 0 { obj.marked_delete = 1; unsafe { a20_sysv_shm_unlock(ctx, irq) }; return 0; }
        let pages = obj.pages; let npages = obj.npages;
        *obj = empty();
        unsafe { a20_sysv_shm_unlock(ctx, irq); release(ctx, pages, npages); }
        return 0;
    }
    if (cmd == IPC_STAT || cmd == SHM_STAT || cmd == SHM_STAT_ANY) && !buffer.is_null() {
        let status = ShmidDs {
            perm: IpcPerm { key: obj.key, uid: 0, gid: 0, cuid: 0, cgid: 0, mode: 0o666, seq: id, unused1: 0, unused2: 0 },
            segsz: obj.size, atime: 0, dtime: 0, ctime: 0, cpid: unsafe { a20_sysv_shm_pid(ctx) },
            lpid: unsafe { a20_sysv_shm_pid(ctx) }, nattch: obj.nattach as usize, pad1: 0, pad2: 0,
        };
        unsafe { a20_sysv_shm_unlock(ctx, irq) };
        return if unsafe { a20_sysv_shm_copy_out(ctx, buffer, &status as *const _ as *const c_void, size_of::<ShmidDs>()) } < 0 { -14 } else { 0 };
    }
    if cmd == SHM_INFO && !buffer.is_null() {
        let info = ShmInfo { used_ids: 0, tot: 0, rss: 0, swp: 0, swap_attempts: 0, swap_successes: 0 };
        unsafe { a20_sysv_shm_unlock(ctx, irq) };
        return if unsafe { a20_sysv_shm_copy_out(ctx, buffer, &info as *const _ as *const c_void, size_of::<ShmInfo>()) } < 0 { -14 } else { 0 };
    }
    unsafe { a20_sysv_shm_unlock(ctx, irq) };
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_shm_ref_txn(table: *mut SysvShm, ctx: *mut c_void, id: i32, delta: i32) -> i32 {
    if table.is_null() || id < 0 || id as usize >= MAX { return -EINVAL; }
    let irq = unsafe { a20_sysv_shm_lock(ctx) };
    let obj = unsafe { &mut *table.add(id as usize) };
    if obj.used == 0 || obj.nattach < 0 || (delta < 0 && obj.nattach == 0) {
        unsafe { a20_sysv_shm_unlock(ctx, irq) };
        return -EINVAL;
    }
    obj.nattach += delta;
    let free = obj.marked_delete != 0 && obj.nattach == 0;
    let (pages, npages) = if free { let pair = (obj.pages, obj.npages); *obj = empty(); pair } else { (core::ptr::null_mut(), 0) };
    unsafe { a20_sysv_shm_unlock(ctx, irq); release(ctx, pages, npages); }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_shm_attach_txn(table: *mut SysvShm, ctx: *mut c_void, id: i32) -> i32 {
    if table.is_null() || id < 0 || id as usize >= MAX { return -EINVAL; }
    let irq = unsafe { a20_sysv_shm_lock(ctx) };
    let obj = unsafe { &mut *table.add(id as usize) };
    if obj.used == 0 || obj.namespace_ino != unsafe { a20_sysv_shm_namespace(ctx) } || obj.marked_delete != 0 {
        unsafe { a20_sysv_shm_unlock(ctx, irq) };
        return -EINVAL;
    }
    obj.nattach += 1;
    unsafe { a20_sysv_shm_unlock(ctx, irq) };
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_shm_size_txn(table: *mut SysvShm, ctx: *mut c_void, id: i32, out: *mut usize) -> i32 {
    if table.is_null() || out.is_null() || id < 0 || id as usize >= MAX { return -EINVAL; }
    let irq = unsafe { a20_sysv_shm_lock(ctx) };
    let obj = unsafe { &*table.add(id as usize) };
    let rc = if obj.used == 0 || obj.namespace_ino != unsafe { a20_sysv_shm_namespace(ctx) } { -EINVAL } else { unsafe { *out = obj.size }; 0 };
    unsafe { a20_sysv_shm_unlock(ctx, irq) };
    rc
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_shm_count_txn(table: *mut SysvShm, ctx: *mut c_void) -> i32 {
    if table.is_null() { return 0; }
    let ns = unsafe { a20_sysv_shm_namespace(ctx) };
    let irq = unsafe { a20_sysv_shm_lock(ctx) };
    let n = (0..MAX).filter(|i| unsafe { let o = &*table.add(*i); o.used != 0 && o.namespace_ino == ns }).count() as i32;
    unsafe { a20_sysv_shm_unlock(ctx, irq) };
    n
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_shm_namespace_destroy_txn(table: *mut SysvShm, ctx: *mut c_void, ns: u64) {
    if table.is_null() { return; }
    for id in 0..MAX {
        let irq = unsafe { a20_sysv_shm_lock(ctx) };
        let obj = unsafe { &mut *table.add(id) };
        let (pages, npages) = if obj.used != 0 && obj.namespace_ino == ns {
            obj.marked_delete = 1;
            if obj.nattach == 0 { let pair = (obj.pages, obj.npages); *obj = empty(); pair } else { (core::ptr::null_mut(), 0) }
        } else { (core::ptr::null_mut(), 0) };
        unsafe { a20_sysv_shm_unlock(ctx, irq); release(ctx, pages, npages); }
    }
}

fn empty() -> SysvShm { SysvShm { used: 0, namespace_ino: 0, marked_delete: 0, key: 0, size: 0, pages: core::ptr::null_mut(), npages: 0, nattach: 0 } }

#[cfg(test)]
mod tests {
    use super::*;
    use core::alloc::Layout;
    extern crate std;

    struct Mock { locked: bool, freed: usize }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_shm_lock(ctx: *mut c_void) -> u64 {
        let m = unsafe { &mut *(ctx as *mut Mock) };
        assert!(!m.locked); m.locked = true; 0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_sysv_shm_unlock(ctx: *mut c_void, _: u64) {
        let m = unsafe { &mut *(ctx as *mut Mock) };
        assert!(m.locked); m.locked = false;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_sysv_shm_namespace(_: *mut c_void) -> u64 { 7 }
    #[no_mangle]
    unsafe extern "C" fn a20_sysv_shm_alloc_pages(_: *mut c_void, npages: usize) -> *mut u32 {
        let layout = Layout::array::<u32>(npages).unwrap();
        unsafe { std::alloc::alloc_zeroed(layout).cast() }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_sysv_shm_free_pages(ctx: *mut c_void, pages: *mut u32, npages: usize) {
        let m = unsafe { &mut *(ctx as *mut Mock) };
        let layout = Layout::array::<u32>(npages).unwrap();
        unsafe { std::alloc::dealloc(pages.cast(), layout) };
        m.freed += npages;
    }
    #[no_mangle]
    unsafe extern "C" fn a20_sysv_shm_copy_out(_: *mut c_void, dst: *mut c_void, src: *const c_void, len: usize) -> i32 {
        unsafe { core::ptr::copy_nonoverlapping(src.cast::<u8>(), dst.cast::<u8>(), len) }; 0
    }
    #[no_mangle]
    unsafe extern "C" fn a20_sysv_shm_pid(_: *mut c_void) -> i32 { 42 }

    #[test]
    fn key_lookup_remove_and_attachment_lifetime() {
        let mut table = [empty(); MAX];
        let mut mock = Mock { locked: false, freed: 0 };
        let ctx = &mut mock as *mut Mock as *mut c_void;
        unsafe {
            assert_eq!(a20_sysv_shm_get_txn(table.as_mut_ptr(), ctx, 17, 4097, IPC_CREAT), 0);
            assert_eq!(a20_sysv_shm_get_txn(table.as_mut_ptr(), ctx, 17, 1, IPC_CREAT), 0);
            assert_eq!(a20_sysv_shm_get_txn(table.as_mut_ptr(), ctx, 17, 1, IPC_CREAT | IPC_EXCL), -EEXIST);
            assert_eq!(a20_sysv_shm_get_txn(table.as_mut_ptr(), ctx, 17, 9000, 0), -EINVAL);
            assert_eq!(a20_sysv_shm_ref_txn(table.as_mut_ptr(), ctx, 0, 1), 0);
            assert_eq!(a20_sysv_shm_control_txn(table.as_mut_ptr(), ctx, 0, IPC_RMID, core::ptr::null_mut()), 0);
            assert_eq!(a20_sysv_shm_ref_txn(table.as_mut_ptr(), ctx, 0, 1), 0);
            assert_eq!(a20_sysv_shm_attach_txn(table.as_mut_ptr(), ctx, 0), -EINVAL);
        }
        assert_eq!((table[0].marked_delete, table[0].nattach, mock.freed), (1, 2, 0));
        unsafe { assert_eq!(a20_sysv_shm_ref_txn(table.as_mut_ptr(), ctx, 0, -1), 0) };
        assert_eq!((table[0].used, mock.freed), (1, 0));
        unsafe { assert_eq!(a20_sysv_shm_ref_txn(table.as_mut_ptr(), ctx, 0, -1), 0) };
        assert_eq!((table[0].used, mock.freed), (0, 2));
        assert!(!mock.locked);
    }

    #[test]
    fn namespace_destroy_defers_free_until_last_detach() {
        let mut table = [empty(); MAX];
        let mut mock = Mock { locked: false, freed: 0 };
        let ctx = &mut mock as *mut Mock as *mut c_void;
        unsafe {
            assert_eq!(a20_sysv_shm_get_txn(table.as_mut_ptr(), ctx, 0, 8, IPC_CREAT), 0);
            assert_eq!(a20_sysv_shm_ref_txn(table.as_mut_ptr(), ctx, 0, 1), 0);
            a20_sysv_shm_namespace_destroy_txn(table.as_mut_ptr(), ctx, 7);
        }
        assert_eq!(table[0].marked_delete, 1);
        unsafe { assert_eq!(a20_sysv_shm_ref_txn(table.as_mut_ptr(), ctx, 0, -1), 0) };
        assert_eq!((table[0].used, mock.freed), (0, 1));
        assert!(!mock.locked);
    }
}
