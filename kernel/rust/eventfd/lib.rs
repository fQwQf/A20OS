#![no_std]

mod ffi;

use a20rust_support::lock::IrqSaveSpinLock;
use core::ffi::{c_char, c_int, c_uint, c_void};
use core::ptr;

use ffi::{
    wait_queue_t, vfile_ops_t, vfile_t, EFD_SEMAPHORE, O_CLOEXEC, O_NONBLOCK,
};

struct EventFdState {
    readers: wait_queue_t,
    writers: wait_queue_t,
    counter: u64,
    semaphore: bool,
    nonblock: bool,
}

struct EventFd {
    lock: IrqSaveSpinLock<EventFdState>,
}

extern "C" fn eventfd_close(vf: *mut vfile_t) -> c_int {
    unsafe { ffi::anonfd_free_priv_close(vf) }
}

static EVENTFD_OPS: vfile_ops_t = vfile_ops_t {
    read: Some(eventfd_read),
    write: Some(eventfd_write),
    lseek: None,
    readdir: None,
    ioctl: None,
    close: Some(eventfd_close),
};

impl EventFd {
    unsafe fn from_vfile(vf: *mut vfile_t) -> Option<*mut EventFd> {
        let priv_ptr = unsafe { ffi::a20_eventfd_vfile_priv(vf) };
        if priv_ptr.is_null() {
            None
        } else {
            Some(priv_ptr as *mut EventFd)
        }
    }
}

#[no_mangle]
pub extern "C" fn eventfd_read(vf: *mut vfile_t, buf: *mut c_char, count: usize) -> c_int {
    if buf.is_null() {
        return -ffi::EINVAL;
    }

    let efd_ptr = match unsafe { EventFd::from_vfile(vf) } {
        Some(p) => p,
        None => return -ffi::EBADF,
    };
    if count < core::mem::size_of::<u64>() {
        return -ffi::EINVAL;
    }

    let mut guard = unsafe { (*efd_ptr).lock.lock() };
    while guard.counter == 0 {
        if guard.nonblock {
            return -ffi::EAGAIN;
        }
        let readers_ptr = &mut guard.readers as *mut wait_queue_t;
        drop(guard);
        unsafe { ffi::wait_queue_sleep(readers_ptr) };
        guard = unsafe { (*efd_ptr).lock.lock() };
    }

    let val = if guard.semaphore { 1 } else { guard.counter };
    guard.counter -= val;
    let wake_writers = guard.counter + val == u64::MAX;
    drop(guard);

    if wake_writers {
        let mut guard = unsafe { (*efd_ptr).lock.lock() };
        unsafe { ffi::wait_queue_wake_all(&mut guard.writers) };
    }

    unsafe {
        ptr::write_unaligned(buf as *mut u64, val);
    }
    core::mem::size_of::<u64>() as c_int
}

#[no_mangle]
pub extern "C" fn eventfd_write(vf: *mut vfile_t, buf: *const c_char, count: usize) -> c_int {
    if buf.is_null() {
        return -ffi::EINVAL;
    }

    let efd_ptr = match unsafe { EventFd::from_vfile(vf) } {
        Some(p) => p,
        None => return -ffi::EBADF,
    };
    if count < core::mem::size_of::<u64>() {
        return -ffi::EINVAL;
    }

    let val = unsafe { ptr::read_unaligned(buf as *const u64) };
    if val == u64::MAX {
        return -ffi::EINVAL;
    }

    let mut guard = unsafe { (*efd_ptr).lock.lock() };
    while guard.counter + val > u64::MAX - 1 {
        if guard.nonblock {
            return -ffi::EAGAIN;
        }
        let writers_ptr = &mut guard.writers as *mut wait_queue_t;
        drop(guard);
        unsafe { ffi::wait_queue_sleep(writers_ptr) };
        guard = unsafe { (*efd_ptr).lock.lock() };
    }

    let wake_readers = guard.counter == 0;
    guard.counter += val;
    drop(guard);

    if wake_readers {
        let mut guard = unsafe { (*efd_ptr).lock.lock() };
        unsafe { ffi::wait_queue_wake_all(&mut guard.readers) };
    }

    core::mem::size_of::<u64>() as c_int
}

#[no_mangle]
pub extern "C" fn eventfd_create(initval: c_uint, flags: c_int) -> c_int {
    let allowed = O_NONBLOCK | O_CLOEXEC | EFD_SEMAPHORE;
    if flags & !allowed != 0 {
        return -ffi::EINVAL;
    }

    let efd_size = core::mem::size_of::<EventFd>();
    let efd_ptr = unsafe { ffi::kmalloc(efd_size) as *mut EventFd };
    if efd_ptr.is_null() {
        return -ffi::ENOMEM;
    }

    let mut state = EventFdState {
        readers: unsafe { core::mem::zeroed() },
        writers: unsafe { core::mem::zeroed() },
        counter: initval as u64,
        semaphore: (flags & EFD_SEMAPHORE) != 0,
        nonblock: (flags & O_NONBLOCK) != 0,
    };
    unsafe {
        ffi::wait_queue_init(&mut state.readers);
        ffi::wait_queue_init(&mut state.writers);
    }
    unsafe {
        ptr::write(ptr::addr_of_mut!((*efd_ptr).lock), IrqSaveSpinLock::new(state));
    }

    let vf = unsafe { ffi::a20_eventfd_vfile_alloc() };
    if vf.is_null() {
        unsafe {
            ffi::kfree(efd_ptr as *mut c_void);
        }
        return -ffi::ENOMEM;
    }

    let vf_flags = ffi::O_RDWR | (flags & O_NONBLOCK);
    unsafe {
        ffi::a20_eventfd_vfile_init(vf, ptr::addr_of!(EVENTFD_OPS) as *mut vfile_ops_t, efd_ptr as *mut c_void, vf_flags);
    }

    let ret = unsafe { ffi::a20_eventfd_install_vfile(vf, flags) };
    if ret < 0 {
        unsafe {
            ffi::kfree(efd_ptr as *mut c_void);
            ffi::a20_eventfd_vfile_free(vf);
        }
        return ret;
    }
    0
}
