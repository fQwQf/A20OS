#![no_std]

mod ffi;

use a20rust_support::lock::IrqSaveSpinLock;
use core::ffi::c_int;
use core::ptr;

use ffi::{vfile_t, EBADF, EBUSY, EINVAL, EMFILE, VFS_MAX_OPEN};

const GFILE_MAX: usize = VFS_MAX_OPEN;
const GFILE_WORDS: usize = (GFILE_MAX + 63) / 64;

struct FileTableState {
    files: [*mut vfile_t; GFILE_MAX],
    mask: [u64; GFILE_WORDS],
    next_fd: c_int,
}

static FILE_TABLE: IrqSaveSpinLock<FileTableState> = IrqSaveSpinLock::new(FileTableState {
    files: [ptr::null_mut(); GFILE_MAX],
    mask: [0; GFILE_WORDS],
    next_fd: 3,
});

unsafe impl Send for FileTableState {}

#[no_mangle]
pub extern "C" fn file_table_init() {
    unsafe { ffi::a20_file_table_init() };
    let mut guard = FILE_TABLE.lock();
    guard.files = [ptr::null_mut(); GFILE_MAX];
    guard.mask = [0; GFILE_WORDS];
    guard.next_fd = 3;
}

fn mask_set(mask: &mut [u64; GFILE_WORDS], fd: c_int) {
    let fd = fd as usize;
    mask[fd >> 6] |= 1u64 << (fd & 63);
}

fn mask_clear(mask: &mut [u64; GFILE_WORDS], fd: c_int) {
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

fn find_free_from(state: &FileTableState, minfd: c_int) -> c_int {
    let minfd = if minfd < 0 { 0 } else { minfd };
    if minfd >= GFILE_MAX as c_int {
        return -1;
    }
    let minfd_u = minfd as usize;
    for word in (minfd_u >> 6)..GFILE_WORDS {
        let mut free_bits = !state.mask[word];
        if word == (minfd_u >> 6) {
            free_bits &= !0u64 << (minfd_u & 63);
        }
        if word == GFILE_WORDS - 1 && (GFILE_MAX & 63) != 0 {
            free_bits &= (1u64 << (GFILE_MAX & 63)) - 1;
        }
        if free_bits != 0 {
            return ((word << 6) as c_int) + ctz64(free_bits);
        }
    }
    -1
}

fn note_alloc(state: &mut FileTableState, fd: c_int) {
    mask_set(&mut state.mask, fd);
    if fd >= state.next_fd {
        state.next_fd = find_free_from(state, fd + 1);
    }
}

fn note_free(state: &mut FileTableState, fd: c_int) {
    mask_clear(&mut state.mask, fd);
    if state.next_fd < 0 || fd < state.next_fd {
        state.next_fd = fd;
    }
}

#[no_mangle]
pub extern "C" fn vfile_alloc() -> *mut vfile_t {
    unsafe { ffi::a20_file_vfile_alloc() }
}

#[no_mangle]
pub extern "C" fn vfile_free(vf: *mut vfile_t) {
    unsafe { ffi::a20_file_vfile_free(vf) }
}

#[no_mangle]
pub extern "C" fn vfile_ref_init(vf: *mut vfile_t, refs: c_int) {
    if !vf.is_null() {
        unsafe { (*vf).ref_init(refs) };
    }
}

#[no_mangle]
pub extern "C" fn vfile_get(vf: *mut vfile_t) {
    if !vf.is_null() {
        unsafe { (*vf).get() };
    }
}

#[no_mangle]
pub extern "C" fn vfile_ref_read(vf: *mut vfile_t) -> c_int {
    if vf.is_null() {
        return 0;
    }
    unsafe { (*vf).ref_read() }
}

#[no_mangle]
pub extern "C" fn vfile_put_ref_only(vf: *mut vfile_t) -> c_int {
    if vf.is_null() {
        return 0;
    }
    unsafe { (*vf).put_ref_only() as c_int }
}

#[no_mangle]
pub extern "C" fn file_install_at(fd: c_int, vf: *mut vfile_t) -> c_int {
    if fd < 0 || fd >= GFILE_MAX as c_int || vf.is_null() {
        return -EBADF;
    }
    let mut guard = FILE_TABLE.lock();
    if !guard.files[fd as usize].is_null() {
        return -EBUSY;
    }
    guard.files[fd as usize] = vf;
    note_alloc(&mut guard, fd);
    fd
}

#[no_mangle]
pub extern "C" fn vfs_alloc_fd(vf: *mut vfile_t) -> c_int {
    if vf.is_null() {
        return -EINVAL;
    }
    let mut guard = FILE_TABLE.lock();
    let mut gfd = find_free_from(&guard, guard.next_fd);
    if gfd < 0 {
        gfd = find_free_from(&guard, 3);
    }
    if gfd >= 0 {
        guard.files[gfd as usize] = vf;
        note_alloc(&mut guard, gfd);
    }
    gfd
}

#[no_mangle]
pub extern "C" fn vfs_get_file(fd: c_int) -> *mut vfile_t {
    if fd < 0 || fd >= GFILE_MAX as c_int {
        return ptr::null_mut();
    }
    let guard = FILE_TABLE.lock();
    guard.files[fd as usize]
}

#[no_mangle]
pub extern "C" fn vfs_get_file_ref(fd: c_int) -> *mut vfile_t {
    if fd < 0 || fd >= GFILE_MAX as c_int {
        return ptr::null_mut();
    }
    let guard = FILE_TABLE.lock();
    let vf = guard.files[fd as usize];
    if !vf.is_null() {
        unsafe { (*vf).get() };
    }
    vf
}

#[no_mangle]
pub extern "C" fn vfs_put_file_ref(_fd: c_int, vf: *mut vfile_t) {
    if !vf.is_null() {
        unsafe { (*vf).put_ref_only() };
    }
}

#[no_mangle]
pub extern "C" fn vfs_ref_fd(fd: c_int) -> c_int {
    if fd < 0 || fd >= GFILE_MAX as c_int {
        return -EBADF;
    }
    let guard = FILE_TABLE.lock();
    let vf = guard.files[fd as usize];
    if vf.is_null() {
        return -EBADF;
    }
    unsafe { (*vf).get() };
    0
}

#[no_mangle]
pub extern "C" fn file_close_prepare(fd: c_int, closed: *mut *mut vfile_t) -> c_int {
    if !closed.is_null() {
        unsafe { *closed = ptr::null_mut() };
    }
    if fd < 0 || fd >= GFILE_MAX as c_int {
        return -EBADF;
    }
    let mut guard = FILE_TABLE.lock();
    let vf = guard.files[fd as usize];
    if vf.is_null() {
        return -EBADF;
    }
    if unsafe { (*vf).put_ref_only() } {
        guard.files[fd as usize] = ptr::null_mut();
        note_free(&mut guard, fd);
        if !closed.is_null() {
            unsafe { *closed = vf };
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn vfs_dupfd(fd: c_int, minfd: c_int) -> c_int {
    let minfd = if minfd < 0 { 0 } else { minfd };
    let mut guard = FILE_TABLE.lock();
    if fd < 0 || fd >= GFILE_MAX as c_int || guard.files[fd as usize].is_null() {
        return -EBADF;
    }
    let vf = guard.files[fd as usize];
    unsafe { (*vf).get() };
    let newfd = find_free_from(&guard, minfd);
    if newfd >= 0 {
        guard.files[newfd as usize] = vf;
        note_alloc(&mut guard, newfd);
        newfd
    } else {
        unsafe { (*vf).put_ref_only() };
        -EMFILE
    }
}

#[no_mangle]
pub extern "C" fn vfs_dup(fd: c_int) -> c_int {
    vfs_dupfd(fd, 3)
}

#[no_mangle]
pub extern "C" fn vfs_dup3(oldfd: c_int, newfd: c_int, _flags: c_int) -> c_int {
    if newfd >= GFILE_MAX as c_int || newfd < 0 {
        return -EBADF;
    }
    if oldfd == newfd {
        return -EINVAL;
    }
    let mut guard = FILE_TABLE.lock();
    if oldfd < 0 || oldfd >= GFILE_MAX as c_int || guard.files[oldfd as usize].is_null() {
        return -EBADF;
    }
    if !guard.files[newfd as usize].is_null() {
        return -EBUSY;
    }
    let vf = guard.files[oldfd as usize];
    unsafe { (*vf).get() };
    guard.files[newfd as usize] = vf;
    note_alloc(&mut guard, newfd);
    newfd
}
