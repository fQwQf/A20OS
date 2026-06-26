#![no_std]
#![deny(unsafe_op_in_unsafe_fn)]
#![warn(rust_2018_idioms)]

mod ffi;

use a20rust_support::lock::raw_irqsave_lock;
use core::ffi::c_void;
use core::mem::size_of;
use core::ptr;

use ffi::{
    shm_info_ds, sysv_shm_t, EFAULT, EINVAL, EEXIST, ENOENT, ENOMEM, ENOSPC, IPC_64_BIT,
    IPC_CREAT, IPC_EXCL, IPC_RMID, IPC_STAT, PFN_NONE, SHM_INFO, SHM_MAX_PAGES, SHM_STAT_ANY,
    SYSV_SHM_MAX,
};

static mut G_SHM: [sysv_shm_t; SYSV_SHM_MAX] = [const {
    sysv_shm_t {
        used: 0,
        marked_delete: 0,
        key: 0,
        size: 0,
        pages: ptr::null_mut(),
        npages: 0,
        nattach: 0,
    }
}; SYSV_SHM_MAX];
static mut G_SHM_LOCK: ffi::spinlock_t = ffi::spinlock_t { locked: 0 };

#[inline]
unsafe fn page_size() -> usize {
    unsafe { ffi::sysv_shm_page_size() }
}

#[inline]
unsafe fn current_task_mm() -> (*mut ffi::task_t, *mut ffi::mm_struct_t) {
    let cur = unsafe { ffi::sysv_shm_proc_current() };
    if cur.is_null() {
        return (ptr::null_mut(), ptr::null_mut());
    }
    let mm = unsafe { ffi::sysv_shm_task_mm(cur) };
    (cur, mm)
}

unsafe fn free_pages_array(pages: *mut u32, npages: usize) {
    if pages.is_null() {
        return;
    }
    for p in 0..npages {
        let pfn = unsafe { *pages.add(p) };
        unsafe { ffi::sysv_shm_frame_put(pfn) };
    }
    unsafe { ffi::sysv_shm_kfree(pages.cast()) };
}

unsafe fn shm_alloc_pages(npages: usize) -> *mut u32 {
    if npages == 0 {
        return ptr::null_mut();
    }
    let pages = unsafe { ffi::sysv_shm_kcalloc(npages, size_of::<u32>()).cast::<u32>() };
    if pages.is_null() {
        return ptr::null_mut();
    }
    for p in 0..npages {
        let pfn = unsafe { ffi::sysv_shm_pfa_alloc_page() };
        if pfn == PFN_NONE {
            for j in 0..p {
                let bad = unsafe { *pages.add(j) };
                unsafe { ffi::sysv_shm_pfa_free_page(bad) };
            }
            unsafe { ffi::sysv_shm_kfree(pages.cast()) };
            return ptr::null_mut();
        }
        unsafe {
            *pages.add(p) = pfn;
            ffi::sysv_shm_zero_pfn(pfn);
        }
    }
    pages
}

unsafe fn shm_valid_locked(shmid: i32) -> bool {
    shmid >= 0 && (shmid as usize) < SYSV_SHM_MAX && unsafe { G_SHM[shmid as usize].used != 0 }
}

unsafe fn shm_free_locked(shmid: usize, pages_out: *mut *mut u32, npages_out: *mut usize) {
    unsafe {
        *pages_out = G_SHM[shmid].pages;
        *npages_out = G_SHM[shmid].npages;
        ptr::write_bytes(ptr::addr_of_mut!(G_SHM[shmid]).cast::<u8>(), 0, size_of::<sysv_shm_t>());
    }
}

#[no_mangle]
pub unsafe extern "C" fn sysv_shm_unref_attach(shmid: i32) {
    let mut free_pages: *mut u32 = ptr::null_mut();
    let mut free_npages: usize = 0;
    {
        let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SHM_LOCK)) };
        if unsafe { shm_valid_locked(shmid) } {
            let ent = unsafe { &mut G_SHM[shmid as usize] };
            if ent.nattach > 0 {
                ent.nattach -= 1;
            }
            if ent.marked_delete != 0 && ent.nattach == 0 {
                unsafe { shm_free_locked(shmid as usize, &mut free_pages, &mut free_npages) };
            }
        }
    }
    unsafe { free_pages_array(free_pages, free_npages) };
}

#[no_mangle]
pub unsafe extern "C" fn sysv_shm_ref_attach(shmid: i32) -> i32 {
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SHM_LOCK)) };
    if !unsafe { shm_valid_locked(shmid) } {
        return -EINVAL;
    }
    unsafe { G_SHM[shmid as usize].nattach += 1 };
    0
}

#[no_mangle]
pub unsafe extern "C" fn sysv_shm_get(key: i32, size: usize, shmflg: i32) -> i32 {
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SHM_LOCK)) };

    if (shmflg & IPC_CREAT) == 0 {
        for i in 0..SYSV_SHM_MAX {
            let ent = unsafe { &G_SHM[i] };
            if ent.used != 0 && ent.marked_delete == 0 && ent.key == key {
                if size > ent.size {
                    return -EINVAL;
                }
                return i as i32;
            }
        }
        return -ENOENT;
    }

    if key != 0 {
        for i in 0..SYSV_SHM_MAX {
            let ent = unsafe { &G_SHM[i] };
            if ent.used != 0 && ent.marked_delete == 0 && ent.key == key {
                if size > ent.size {
                    return -EINVAL;
                }
                if (shmflg & IPC_EXCL) != 0 {
                    return -EEXIST;
                }
                return i as i32;
            }
        }
    }

    if size == 0 {
        return -EINVAL;
    }
    let pg = unsafe { page_size() };
    if size > SHM_MAX_PAGES * pg {
        return -ENOMEM;
    }
    let npages = (size + pg - 1) / pg;
    if npages > SHM_MAX_PAGES {
        return -ENOMEM;
    }

    let mut slot: i32 = -1;
    for i in 0..SYSV_SHM_MAX {
        if unsafe { G_SHM[i].used } == 0 {
            slot = i as i32;
            break;
        }
    }
    if slot < 0 {
        return -ENOSPC;
    }

    drop(_guard);

    let pages = unsafe { shm_alloc_pages(npages) };
    if pages.is_null() {
        return -ENOMEM;
    }

    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SHM_LOCK)) };

    if key != 0 {
        for i in 0..SYSV_SHM_MAX {
            let ent = unsafe { &G_SHM[i] };
            if ent.used != 0 && ent.marked_delete == 0 && ent.key == key {
                let existing_size = ent.size;
                let existing_id = i as i32;
                drop(_guard);
                unsafe { free_pages_array(pages, npages) };
                if size > existing_size {
                    return -EINVAL;
                }
                return if (shmflg & IPC_EXCL) != 0 {
                    -EEXIST
                } else {
                    existing_id
                };
            }
        }
    }

    if unsafe { G_SHM[slot as usize].used } != 0 {
        slot = -1;
        for i in 0..SYSV_SHM_MAX {
            if unsafe { G_SHM[i].used } == 0 {
                slot = i as i32;
                break;
            }
        }
        if slot < 0 {
            drop(_guard);
            unsafe { free_pages_array(pages, npages) };
            return -ENOSPC;
        }
    }

    let idx = slot as usize;
    unsafe {
        let ent = &mut G_SHM[idx];
        ent.used = 1;
        ent.marked_delete = 0;
        ent.key = key;
        ent.size = npages * pg;
        ent.pages = pages;
        ent.npages = npages;
        ent.nattach = 0;
    }
    slot
}

#[no_mangle]
pub unsafe extern "C" fn sysv_shm_size(shmid: i32, size: *mut usize) -> i32 {
    if size.is_null() {
        return -EINVAL;
    }
    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SHM_LOCK)) };
    if !unsafe { shm_valid_locked(shmid) } {
        return -EINVAL;
    }
    unsafe { *size = G_SHM[shmid as usize].size };
    0
}

#[no_mangle]
pub unsafe extern "C" fn sysv_shm_at(shmid: i32, shmaddr: u64, _shmflg: i32) -> u64 {
    let (_cur, mm) = unsafe { current_task_mm() };
    if mm.is_null() {
        return (-EINVAL as i64) as u64;
    }

    let lk = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SHM_LOCK)) };
    if !unsafe { shm_valid_locked(shmid) } || unsafe { G_SHM[shmid as usize].marked_delete } != 0 {
        drop(lk);
        return (-EINVAL as i64) as u64;
    }
    let pages = unsafe { G_SHM[shmid as usize].pages };
    let npages = unsafe { G_SHM[shmid as usize].npages };
    let shm_size = unsafe { G_SHM[shmid as usize].size };
    unsafe { G_SHM[shmid as usize].nattach += 1 };
    drop(lk);

    let addr = unsafe { ffi::sysv_shm_do_attach(mm, shmaddr, pages, npages, shm_size, shmid) };
    if addr >= (-4096i64) as u64 {
        unsafe { sysv_shm_unref_attach(shmid) };
    }
    addr
}

#[no_mangle]
pub unsafe extern "C" fn sysv_shm_detach(shmaddr: *const c_void) -> i32 {
    if shmaddr.is_null() {
        return -EINVAL;
    }
    let (_cur, mm) = unsafe { current_task_mm() };
    if mm.is_null() {
        return -EINVAL;
    }
    let addr = shmaddr as u64;
    let vma = unsafe { ffi::sysv_shm_mm_find_vma(mm, addr) };
    let mut shmid: i32 = -1;
    if unsafe { ffi::sysv_shm_vma_matches(vma, addr, &mut shmid) } == 0 {
        return -EINVAL;
    }
    let len = unsafe { ffi::sysv_shm_vma_len(vma) };
    if shmid < 0 {
        return -EINVAL;
    }
    unsafe { ffi::sysv_shm_mm_munmap(mm, addr, len) }
}

#[no_mangle]
pub unsafe extern "C" fn sysv_shm_control(shmid: i32, cmd: i32, buf: *mut c_void) -> i32 {
    let mut cmd = cmd;
    cmd &= !IPC_64_BIT;

    let _guard = unsafe { raw_irqsave_lock(ptr::addr_of_mut!(G_SHM_LOCK)) };
    if !unsafe { shm_valid_locked(shmid) } {
        return -EINVAL;
    }

    if cmd == IPC_RMID {
        let mut free_pages: *mut u32 = ptr::null_mut();
        let mut free_npages: usize = 0;
        if unsafe { G_SHM[shmid as usize].nattach } > 0 {
            unsafe { G_SHM[shmid as usize].marked_delete = 1 };
            return 0;
        }
        unsafe { shm_free_locked(shmid as usize, &mut free_pages, &mut free_npages) };
        drop(_guard);
        unsafe { free_pages_array(free_pages, free_npages) };
        return 0;
    }

    if (cmd == IPC_STAT || cmd == SHM_STAT_ANY) && !buf.is_null() {
        let cur = unsafe { ffi::sysv_shm_proc_current() };
        let pid = if cur.is_null() { 0 } else { unsafe { ffi::sysv_shm_task_pid(cur) } };
        let key = unsafe { G_SHM[shmid as usize].key };
        let segsz = unsafe { G_SHM[shmid as usize].size };
        let nattch = unsafe { G_SHM[shmid as usize].nattach as u64 };
        drop(_guard);

        let mut ds: shm_info_ds = unsafe { core::mem::zeroed() };
        ds.perm_k = key;
        ds.perm_m = 0o666;
        ds.perm_s = shmid;
        ds.segsz = segsz;
        ds.cpid = pid;
        ds.lpid = pid;
        ds.nattch = nattch;
        if unsafe {
            ffi::sysv_shm_copy_to_user(
                buf,
                ptr::addr_of!(ds).cast(),
                size_of::<shm_info_ds>(),
            )
        } < 0
        {
            return -EFAULT;
        }
        return 0;
    }

    if cmd == SHM_INFO && !buf.is_null() {
        drop(_guard);
        let zero = [0u8; 64];
        if unsafe { ffi::sysv_shm_copy_to_user(buf, zero.as_ptr().cast(), zero.len()) } < 0 {
            return -EFAULT;
        }
        return 0;
    }

    0
}
