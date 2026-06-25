#![no_std]

mod ffi;

use a20rust_support::lock::IrqSaveSpinLock;
use core::ffi::{c_int, c_void};
use core::ptr;
use core::sync::atomic::{AtomicI32, Ordering};

use ffi::{
    task_t, EBADF, EMFILE, ENOMEM, ESRCH, EINVAL, FD_CLOEXEC, O_CLOEXEC,
    FDTABLE_WORDS, MAX_FILES,
};

struct FdTableInner {
    fd: [c_int; MAX_FILES],
    cloexec: [u8; MAX_FILES],
    open_mask: [u64; FDTABLE_WORDS],
    next_fd: c_int,
}

struct FdTable {
    refcount: AtomicI32,
    inner: IrqSaveSpinLock<FdTableInner>,
}

unsafe impl Send for FdTable {}

fn fd_limit(task: *mut task_t) -> c_int {
    unsafe { ffi::a20_fdtable_fd_limit(task) }
}

fn current_task_files() -> *mut c_void {
    unsafe { ffi::a20_fdtable_get_files(ffi::proc_current()) }
}

fn task_files(task: *mut task_t) -> *mut c_void {
    unsafe { ffi::a20_fdtable_get_files(task) }
}

fn set_task_files(task: *mut task_t, files: *mut FdTable) {
    unsafe { ffi::a20_fdtable_set_files(task, files as *mut c_void) };
}

fn refcount_read(r: &AtomicI32) -> c_int {
    r.load(Ordering::Relaxed)
}

fn refcount_set(r: &AtomicI32, v: c_int) {
    r.store(v, Ordering::Relaxed);
}

fn refcount_inc(r: &AtomicI32) {
    r.fetch_add(1, Ordering::Relaxed);
}

fn refcount_dec_and_test(r: &AtomicI32) -> bool {
    r.fetch_sub(1, Ordering::AcqRel) == 1
}

fn alloc_fdtable() -> *mut FdTable {
    let size = core::mem::size_of::<FdTable>();
    let ptr = unsafe { ffi::kmalloc(size) as *mut FdTable };
    if ptr.is_null() {
        unsafe { ffi::a20_fdtable_panic_oom() };
    }
    unsafe {
        let inner = FdTableInner {
            fd: [-1; MAX_FILES],
            cloexec: [0; MAX_FILES],
            open_mask: [0; FDTABLE_WORDS],
            next_fd: 0,
        };
        ptr::write(ptr::addr_of_mut!((*ptr).refcount), AtomicI32::new(1));
        ptr::write(ptr::addr_of_mut!((*ptr).inner), IrqSaveSpinLock::new(inner));
    }
    ptr
}

fn ensure_task_files(task: *mut task_t) -> *mut FdTable {
    let mut files = task_files(task) as *mut FdTable;
    if files.is_null() {
        files = alloc_fdtable();
        set_task_files(task, files);
    }
    files
}

fn mask_set(mask: &mut [u64; FDTABLE_WORDS], fd: c_int) {
    let fd = fd as usize;
    mask[fd >> 6] |= 1u64 << (fd & 63);
}

fn mask_clear(mask: &mut [u64; FDTABLE_WORDS], fd: c_int) {
    let fd = fd as usize;
    mask[fd >> 6] &= !(1u64 << (fd & 63));
}

fn ctz64(bits: u64) -> c_int {
    if bits == 0 {
        return 64;
    }
    let mut n = 0;
    let mut b = bits;
    if (b & 0xFFFFFFFF) == 0 {
        n += 32;
        b >>= 32;
    }
    if (b & 0xFFFF) == 0 {
        n += 16;
        b >>= 16;
    }
    if (b & 0xFF) == 0 {
        n += 8;
        b >>= 8;
    }
    if (b & 0xF) == 0 {
        n += 4;
        b >>= 4;
    }
    if (b & 0x3) == 0 {
        n += 2;
        b >>= 2;
    }
    if (b & 0x1) == 0 {
        n += 1;
    }
    n
}

fn find_free(inner: &FdTableInner, minfd: c_int) -> c_int {
    if minfd < 0 {
        return find_free(inner, 0);
    }
    if minfd >= MAX_FILES as c_int {
        return -1;
    }
    let minfd_u = minfd as usize;
    for word in (minfd_u >> 6)..FDTABLE_WORDS {
        let used = inner.open_mask[word];
        let mut free_bits = !used;
        if word == (minfd_u >> 6) {
            free_bits &= !0u64 << (minfd_u & 63);
        }
        if word == FDTABLE_WORDS - 1 && (MAX_FILES & 63) != 0 {
            free_bits &= (1u64 << (MAX_FILES & 63)) - 1;
        }
        if free_bits != 0 {
            return ((word << 6) as c_int) + ctz64(free_bits);
        }
    }
    -1
}

fn find_free_below(inner: &FdTableInner, minfd: c_int, limit: c_int) -> c_int {
    let fd = find_free(inner, minfd);
    if fd >= 0 && fd < limit {
        fd
    } else {
        -1
    }
}

fn note_alloc(inner: &mut FdTableInner, fd: c_int) {
    mask_set(&mut inner.open_mask, fd);
    if fd >= inner.next_fd {
        inner.next_fd = find_free(inner, fd + 1);
    }
}

fn note_free(inner: &mut FdTableInner, fd: c_int) {
    if fd < 0 || fd >= MAX_FILES as c_int {
        return;
    }
    mask_clear(&mut inner.open_mask, fd);
    if inner.next_fd < 0 || fd < inner.next_fd {
        inner.next_fd = fd;
    }
}

fn recompute_next(inner: &mut FdTableInner) {
    for i in 0..FDTABLE_WORDS {
        inner.open_mask[i] = 0;
    }
    for fd in 0..MAX_FILES {
        if inner.fd[fd] >= 0 {
            mask_set(&mut inner.open_mask, fd as c_int);
        }
    }
    inner.next_fd = find_free(inner, 0);
}

fn ref_gfd(gfd: c_int) -> c_int {
    unsafe { ffi::vfs_ref_fd(gfd) }
}

#[no_mangle]
pub extern "C" fn fdtable_init(task: *mut task_t) {
    if task.is_null() {
        return;
    }
    let old = task_files(task) as *mut FdTable;
    if !old.is_null() {
        unsafe { ffi::kfree(old as *mut c_void) };
    }
    let files = alloc_fdtable();
    set_task_files(task, files);
    fdtable_init_stdio(task);
}

#[no_mangle]
pub extern "C" fn fdtable_init_stdio(task: *mut task_t) {
    let files = ensure_task_files(task);
    if files.is_null() {
        return;
    }
    let mut guard = unsafe { (*files).inner.lock() };
    for fd in 0..3 {
        if fd >= MAX_FILES {
            break;
        }
        if guard.fd[fd] >= 0 {
            continue;
        }
        guard.fd[fd] = fd as c_int;
        guard.cloexec[fd] = 0;
        note_alloc(&mut guard, fd as c_int);
        ref_gfd(fd as c_int);
    }
}

#[no_mangle]
pub extern "C" fn fdtable_copy(dst: *mut task_t, src: *const task_t) {
    if dst.is_null() {
        return;
    }
    let old = task_files(dst) as *mut FdTable;
    if !old.is_null() {
        unsafe { ffi::kfree(old as *mut c_void) };
    }
    let dst_files = alloc_fdtable();
    set_task_files(dst, dst_files);

    let src_files = src as *mut task_t;
    let src_ptr = task_files(src_files) as *mut FdTable;
    if src_ptr.is_null() {
        fdtable_init_stdio(dst);
        return;
    }

    let mut dst_guard = unsafe { (*dst_files).inner.lock() };
    let src_guard = unsafe { (*src_ptr).inner.lock() };
    for i in 0..MAX_FILES {
        let gfd = src_guard.fd[i];
        dst_guard.fd[i] = gfd;
        dst_guard.cloexec[i] = src_guard.cloexec[i];
        if gfd >= 0 {
            ref_gfd(gfd);
        }
    }
    recompute_next(&mut dst_guard);
}

#[no_mangle]
pub extern "C" fn fdtable_share(dst: *mut task_t, src: *const task_t) {
    if dst.is_null() || src.is_null() {
        return;
    }
    let old = task_files(dst) as *mut FdTable;
    if !old.is_null() {
        fdtable_close_all(dst);
    }
    let src_files = task_files(src as *mut task_t) as *mut FdTable;
    set_task_files(dst, src_files);
    if !src_files.is_null() {
        refcount_inc(&unsafe { &*src_files }.refcount);
    }
}

#[no_mangle]
pub extern "C" fn fdtable_unshare(task: *mut task_t) -> c_int {
    if task.is_null() {
        return -ESRCH;
    }
    let old = ensure_task_files(task);
    if old.is_null() {
        return -ENOMEM;
    }
    if refcount_read(&unsafe { &*old }.refcount) == 1 {
        return 0;
    }

    let new_files = alloc_fdtable();
    {
        let old_guard = unsafe { (*old).inner.lock() };
        let mut new_guard = unsafe { (*new_files).inner.lock() };
        for i in 0..MAX_FILES {
            let gfd = old_guard.fd[i];
            new_guard.fd[i] = gfd;
            new_guard.cloexec[i] = old_guard.cloexec[i];
            if gfd >= 0 {
                let r = ref_gfd(gfd);
                if r < 0 {
                    drop(new_guard);
                    drop(old_guard);
                    for j in 0..i {
                        let g = unsafe { (*new_files).inner.lock() }.fd[j];
                        if g >= 0 {
                            unsafe { ffi::vfs_close(g) };
                        }
                    }
                    unsafe { ffi::kfree(new_files as *mut c_void) };
                    return r;
                }
            }
        }
        recompute_next(&mut new_guard);
    }
    set_task_files(task, new_files);
    refcount_dec_and_test(&unsafe { &*old }.refcount);
    0
}

#[no_mangle]
pub extern "C" fn fdtable_close_all(task: *mut task_t) {
    if task.is_null() {
        return;
    }
    let files = task_files(task) as *mut FdTable;
    if files.is_null() {
        return;
    }
    set_task_files(task, ptr::null_mut());
    if !refcount_dec_and_test(&unsafe { &*files }.refcount) {
        return;
    }

    let mut to_close = [-1i32; MAX_FILES];
    let mut close_count = 0usize;
    {
        let mut guard = unsafe { (*files).inner.lock() };
        for i in 0..MAX_FILES {
            if guard.fd[i] >= 0 {
                to_close[close_count] = guard.fd[i];
                close_count += 1;
                guard.fd[i] = -1;
                guard.cloexec[i] = 0;
                mask_clear(&mut guard.open_mask, i as c_int);
            }
        }
    }
    for i in 0..close_count {
        unsafe { ffi::vfs_close(to_close[i]) };
    }
    unsafe { ffi::kfree(files as *mut c_void) };
}

#[no_mangle]
pub extern "C" fn fdtable_close_on_exec(task: *mut task_t) {
    let files = ensure_task_files(task);
    if files.is_null() {
        return;
    }
    let mut to_close = [-1i32; MAX_FILES];
    let mut close_count = 0usize;
    {
        let mut guard = unsafe { (*files).inner.lock() };
        for i in 0..MAX_FILES {
            if guard.cloexec[i] != 0 && guard.fd[i] >= 0 {
                to_close[close_count] = guard.fd[i];
                close_count += 1;
                guard.fd[i] = -1;
                note_free(&mut guard, i as c_int);
            }
            guard.cloexec[i] = 0;
        }
        recompute_next(&mut guard);
    }
    for i in 0..close_count {
        unsafe { ffi::vfs_close(to_close[i]) };
    }
    fdtable_init_stdio(task);
}

#[no_mangle]
pub extern "C" fn fdtable_get(task: *mut task_t, fd: c_int) -> c_int {
    if task.is_null() || fd < 0 || fd >= MAX_FILES as c_int {
        return -EBADF;
    }
    let files = task_files(task) as *mut FdTable;
    if files.is_null() {
        return -EBADF;
    }
    let guard = unsafe { (*files).inner.lock() };
    let gfd = guard.fd[fd as usize];
    if gfd < 0 {
        -EBADF
    } else {
        gfd
    }
}

#[no_mangle]
pub extern "C" fn fdtable_get_current(fd: c_int) -> c_int {
    fdtable_get(unsafe { ffi::proc_current() }, fd)
}

#[no_mangle]
pub extern "C" fn fdtable_install(task: *mut task_t, gfd: c_int, flags: c_int) -> c_int {
    if task.is_null() || gfd < 0 {
        return -EBADF;
    }
    let files = ensure_task_files(task);
    if files.is_null() {
        return -ESRCH;
    }
    let mut guard = unsafe { (*files).inner.lock() };
    let limit = fd_limit(task);
    let mut fd = find_free_below(&guard, guard.next_fd, limit);
    if fd < 0 {
        fd = find_free_below(&guard, 0, limit);
    }
    if fd >= 0 {
        guard.fd[fd as usize] = gfd;
        guard.cloexec[fd as usize] = if (flags & O_CLOEXEC) != 0 { 1 } else { 0 };
        note_alloc(&mut guard, fd);
        fd
    } else {
        unsafe { ffi::vfs_close(gfd) };
        -EMFILE
    }
}

#[no_mangle]
pub extern "C" fn fdtable_install_current(gfd: c_int, flags: c_int) -> c_int {
    fdtable_install(unsafe { ffi::proc_current() }, gfd, flags)
}

#[no_mangle]
pub extern "C" fn fdtable_close(task: *mut task_t, fd: c_int) -> c_int {
    if task.is_null() || fd < 0 || fd >= MAX_FILES as c_int {
        return -EBADF;
    }
    let files = task_files(task) as *mut FdTable;
    if files.is_null() {
        return -EBADF;
    }
    let gfd;
    {
        let mut guard = unsafe { (*files).inner.lock() };
        gfd = guard.fd[fd as usize];
        if gfd < 0 {
            return -EBADF;
        }
        guard.fd[fd as usize] = -1;
        guard.cloexec[fd as usize] = 0;
        note_free(&mut guard, fd);
    }
    unsafe { ffi::vfs_close(gfd) }
}

#[no_mangle]
pub extern "C" fn fdtable_close_current(fd: c_int) -> c_int {
    fdtable_close(unsafe { ffi::proc_current() }, fd)
}

#[no_mangle]
pub extern "C" fn fdtable_dup(task: *mut task_t, oldfd: c_int, minfd: c_int, flags: c_int) -> c_int {
    if task.is_null() {
        return -ESRCH;
    }
    let files = task_files(task) as *mut FdTable;
    if files.is_null() {
        return -EBADF;
    }
    if flags & !O_CLOEXEC != 0 {
        return -EINVAL;
    }
    if oldfd < 0 || oldfd >= MAX_FILES as c_int {
        return -EBADF;
    }
    let minfd = if minfd < 0 { 0 } else { minfd };

    let mut guard = unsafe { (*files).inner.lock() };
    let limit = fd_limit(task);
    if minfd >= limit {
        return -EMFILE;
    }

    let gfd = guard.fd[oldfd as usize];
    if gfd < 0 {
        return -EBADF;
    }
    if ref_gfd(gfd) < 0 {
        return -EBADF;
    }

    let fd = find_free_below(&guard, minfd, limit);
    if fd >= 0 {
        guard.fd[fd as usize] = gfd;
        guard.cloexec[fd as usize] = if (flags & O_CLOEXEC) != 0 { 1 } else { 0 };
        note_alloc(&mut guard, fd);
        fd
    } else {
        unsafe { ffi::vfs_close(gfd) };
        -EMFILE
    }
}

#[no_mangle]
pub extern "C" fn fdtable_dup_to(task: *mut task_t, oldfd: c_int, newfd: c_int, flags: c_int) -> c_int {
    if task.is_null() {
        return -ESRCH;
    }
    let files = task_files(task) as *mut FdTable;
    if files.is_null() {
        return -EBADF;
    }
    if flags & !O_CLOEXEC != 0 {
        return -EINVAL;
    }
    if oldfd < 0 || oldfd >= MAX_FILES as c_int || newfd < 0 || newfd >= MAX_FILES as c_int {
        return -EBADF;
    }
    if newfd >= fd_limit(task) {
        return -EBADF;
    }
    if oldfd == newfd {
        return -EINVAL;
    }

    let mut guard = unsafe { (*files).inner.lock() };
    let gfd = guard.fd[oldfd as usize];
    if gfd < 0 {
        return -EBADF;
    }

    let old_new_gfd = guard.fd[newfd as usize];
    guard.fd[newfd as usize] = -1;
    guard.cloexec[newfd as usize] = 0;
    note_free(&mut guard, newfd);
    if ref_gfd(gfd) < 0 {
        if old_new_gfd >= 0 {
            unsafe { ffi::vfs_close(old_new_gfd) };
        }
        return -EBADF;
    }
    guard.fd[newfd as usize] = gfd;
    guard.cloexec[newfd as usize] = if (flags & O_CLOEXEC) != 0 { 1 } else { 0 };
    note_alloc(&mut guard, newfd);
    drop(guard);
    if old_new_gfd >= 0 {
        unsafe { ffi::vfs_close(old_new_gfd) };
    }
    newfd
}

#[no_mangle]
pub extern "C" fn fdtable_get_cloexec(task: *mut task_t, fd: c_int) -> c_int {
    if task.is_null() || fd < 0 || fd >= MAX_FILES as c_int {
        return -EBADF;
    }
    let files = task_files(task) as *mut FdTable;
    if files.is_null() {
        return -EBADF;
    }
    let guard = unsafe { (*files).inner.lock() };
    if guard.fd[fd as usize] < 0 {
        -EBADF
    } else if guard.cloexec[fd as usize] != 0 {
        FD_CLOEXEC
    } else {
        0
    }
}

#[no_mangle]
pub extern "C" fn fdtable_set_cloexec(task: *mut task_t, fd: c_int, cloexec: c_int) -> c_int {
    if task.is_null() || fd < 0 || fd >= MAX_FILES as c_int {
        return -EBADF;
    }
    let files = task_files(task) as *mut FdTable;
    if files.is_null() {
        return -EBADF;
    }
    let mut guard = unsafe { (*files).inner.lock() };
    if guard.fd[fd as usize] < 0 {
        return -EBADF;
    }
    guard.cloexec[fd as usize] = if cloexec != 0 { 1 } else { 0 };
    0
}
