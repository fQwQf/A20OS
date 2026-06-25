#![no_std]

mod ffi;

use a20rust_support::lock::{IrqSaveGuard, IrqSaveSpinLock};
use core::ffi::{c_char, c_int, c_void};
use core::{mem, ptr};

use ffi::{task_t, vfile_ops_t, vfile_t, wait_queue_entry_t, wait_queue_t};

const PIPE_DEFAULT_SIZE: usize = 256 * ffi::PIPE_BUF_SIZE;

struct PipeState {
    data: *mut u8,
    capacity: usize,
    head: usize,
    tail: usize,
    used: usize,
    logical_size: usize,
    writer_closed: bool,
    reader_closed: bool,
    refs: c_int,
    read_waiters: wait_queue_t,
    write_waiters: wait_queue_t,
}

struct PipeBuf {
    lock: IrqSaveSpinLock<PipeState>,
}

unsafe impl Send for PipeState {}

static PIPE_READ_OPS: vfile_ops_t = vfile_ops_t {
    read: Some(pipe_read),
    write: Some(pipe_null_write),
    lseek: None,
    readdir: None,
    ioctl: None,
    close: Some(pipe_read_close),
};

static PIPE_WRITE_OPS: vfile_ops_t = vfile_ops_t {
    read: Some(pipe_null_read),
    write: Some(pipe_write),
    lseek: None,
    readdir: None,
    ioctl: None,
    close: Some(pipe_write_close),
};

unsafe fn pipe_from_vfile(vf: *mut vfile_t) -> *mut PipeBuf {
    unsafe { ffi::a20_pipe_vfile_priv(vf) as *mut PipeBuf }
}

unsafe fn current_task() -> *mut task_t {
    unsafe { ffi::proc_current() }
}

unsafe fn send_sigpipe_to_current() {
    let task = unsafe { current_task() };
    if !task.is_null() {
        let pid = unsafe { ffi::a20_pipe_task_pid(task) };
        if pid >= 0 {
            unsafe { ffi::signal_send(pid, ffi::SIGPIPE) };
        }
    }
}

unsafe fn wait_interruptible<'a>(
    pb: *mut PipeBuf,
    guard: IrqSaveGuard<'a, PipeState>,
    readers: bool,
) -> Result<IrqSaveGuard<'a, PipeState>, c_int> {
    let task = unsafe { current_task() };
    if task.is_null() {
        drop(guard);
        unsafe { ffi::proc_yield() };
        return Ok(unsafe { (*pb).lock.lock() });
    }
    if unsafe { ffi::signal_task_has_unblocked(task) } != 0 {
        return Err(-ffi::ERESTARTSYS);
    }

    let mut guard = guard;
    let mut entry = wait_queue_entry_t {
        next: ptr::null_mut(),
        prev: ptr::null_mut(),
        task: task as *mut c_void,
    };
    let waiters = if readers {
        &mut guard.read_waiters as *mut wait_queue_t
    } else {
        &mut guard.write_waiters as *mut wait_queue_t
    };

    unsafe {
        ffi::wait_queue_prepare(waiters, &mut entry);
        ffi::a20_pipe_task_set_blocked(task);
    }
    drop(guard);
    unsafe {
        ffi::sched();
        ffi::wait_queue_finish(waiters, &mut entry);
    }
    let guard = unsafe { (*pb).lock.lock() };
    if unsafe { ffi::signal_task_has_unblocked(task) } != 0 {
        Err(-ffi::ERESTARTSYS)
    } else {
        Ok(guard)
    }
}

#[no_mangle]
pub extern "C" fn pipe_read(vf: *mut vfile_t, buf: *mut c_char, count: usize) -> c_int {
    let pb = unsafe { pipe_from_vfile(vf) };
    if pb.is_null() {
        return -ffi::EBADF;
    }
    if count == 0 {
        return 0;
    }

    let nonblock = unsafe { ffi::a20_pipe_vfile_flags(vf) & ffi::O_NONBLOCK } != 0;
    let mut guard = unsafe { (*pb).lock.lock() };
    while guard.used == 0 {
        if guard.writer_closed {
            return 0;
        }
        if nonblock {
            return -ffi::EAGAIN;
        }
        guard = match unsafe { wait_interruptible(pb, guard, true) } {
            Ok(g) => g,
            Err(e) => return e,
        };
    }

    let n = core::cmp::min(guard.used, count);
    let first = core::cmp::min(guard.capacity - guard.tail, n);
    unsafe { ptr::copy_nonoverlapping(guard.data.add(guard.tail), buf as *mut u8, first) };
    let second = n - first;
    if second != 0 {
        unsafe { ptr::copy_nonoverlapping(guard.data, (buf as *mut u8).add(first), second) };
    }
    guard.tail = (guard.tail + n) % guard.capacity;
    guard.used -= n;
    let writers = &mut guard.write_waiters as *mut wait_queue_t;
    drop(guard);
    unsafe { ffi::wait_queue_wake_all(writers) };
    n as c_int
}

#[no_mangle]
pub extern "C" fn pipe_write(vf: *mut vfile_t, buf: *const c_char, count: usize) -> c_int {
    let pb = unsafe { pipe_from_vfile(vf) };
    if pb.is_null() {
        return -ffi::EBADF;
    }
    if count == 0 {
        return 0;
    }

    let nonblock = unsafe { ffi::a20_pipe_vfile_flags(vf) & ffi::O_NONBLOCK } != 0;
    let mut guard = unsafe { (*pb).lock.lock() };
    if guard.reader_closed {
        drop(guard);
        unsafe { send_sigpipe_to_current() };
        return -ffi::EPIPE;
    }

    let mut written = 0usize;
    while written < count {
        let remaining = count - written;
        let mut space = guard.capacity - guard.used;
        if remaining <= ffi::PIPE_BUF_SIZE {
            while space < remaining {
                if guard.reader_closed {
                    if written > 0 {
                        return written as c_int;
                    }
                    drop(guard);
                    unsafe { send_sigpipe_to_current() };
                    return -ffi::EPIPE;
                }
                if nonblock {
                    return if written > 0 { written as c_int } else { -ffi::EAGAIN };
                }
                guard = match unsafe { wait_interruptible(pb, guard, false) } {
                    Ok(g) => g,
                    Err(e) => return if written > 0 { written as c_int } else { e },
                };
                space = guard.capacity - guard.used;
            }
            let chunk = remaining;
            let first = core::cmp::min(guard.capacity - guard.head, chunk);
            unsafe {
                ptr::copy_nonoverlapping((buf as *const u8).add(written), guard.data.add(guard.head), first)
            };
            let second = chunk - first;
            if second != 0 {
                unsafe {
                    ptr::copy_nonoverlapping((buf as *const u8).add(written + first), guard.data, second)
                };
            }
            guard.head = (guard.head + chunk) % guard.capacity;
            guard.used += chunk;
            written += chunk;
            let readers = &mut guard.read_waiters as *mut wait_queue_t;
            drop(guard);
            unsafe { ffi::wait_queue_wake_all(readers) };
            guard = unsafe { (*pb).lock.lock() };
        } else {
            if space == 0 {
                if guard.reader_closed {
                    if written > 0 {
                        return written as c_int;
                    }
                    drop(guard);
                    unsafe { send_sigpipe_to_current() };
                    return -ffi::EPIPE;
                }
                if nonblock {
                    return if written > 0 { written as c_int } else { -ffi::EAGAIN };
                }
                guard = match unsafe { wait_interruptible(pb, guard, false) } {
                    Ok(g) => g,
                    Err(e) => return if written > 0 { written as c_int } else { e },
                };
                continue;
            }
            let chunk = core::cmp::min(remaining, space);
            let first = core::cmp::min(guard.capacity - guard.head, chunk);
            unsafe {
                ptr::copy_nonoverlapping((buf as *const u8).add(written), guard.data.add(guard.head), first)
            };
            let second = chunk - first;
            if second != 0 {
                unsafe {
                    ptr::copy_nonoverlapping((buf as *const u8).add(written + first), guard.data, second)
                };
            }
            guard.head = (guard.head + chunk) % guard.capacity;
            guard.used += chunk;
            written += chunk;
            let readers = &mut guard.read_waiters as *mut wait_queue_t;
            drop(guard);
            unsafe { ffi::wait_queue_wake_all(readers) };
            guard = unsafe { (*pb).lock.lock() };
        }
    }
    drop(guard);
    written as c_int
}

#[no_mangle]
pub extern "C" fn pipe_null_read(_vf: *mut vfile_t, _buf: *mut c_char, _count: usize) -> c_int {
    0
}

#[no_mangle]
pub extern "C" fn pipe_null_write(_vf: *mut vfile_t, _buf: *const c_char, count: usize) -> c_int {
    count as c_int
}

fn pipe_resize_locked(pb: *mut PipeBuf, mut new_capacity: usize) -> c_int {
    if new_capacity < ffi::PIPE_BUF_SIZE {
        new_capacity = ffi::PIPE_BUF_SIZE;
    }

    let mut guard = unsafe { (*pb).lock.lock() };
    if new_capacity < guard.used {
        return -ffi::EBUSY;
    }
    if new_capacity == guard.capacity {
        guard.logical_size = new_capacity;
        return new_capacity as c_int;
    }
    drop(guard);

    let new_data = unsafe { ffi::kmalloc(new_capacity) as *mut u8 };
    if new_data.is_null() {
        return -ffi::ENOMEM;
    }

    let mut guard = unsafe { (*pb).lock.lock() };
    if new_capacity < guard.used {
        drop(guard);
        unsafe { ffi::kfree(new_data as *mut c_void) };
        return -ffi::EBUSY;
    }
    if new_capacity == guard.capacity {
        guard.logical_size = new_capacity;
        drop(guard);
        unsafe { ffi::kfree(new_data as *mut c_void) };
        return new_capacity as c_int;
    }
    for i in 0..guard.used {
        unsafe { *new_data.add(i) = *guard.data.add((guard.tail + i) % guard.capacity) };
    }
    let old_data = guard.data;
    guard.data = new_data;
    guard.capacity = new_capacity;
    guard.logical_size = new_capacity;
    guard.tail = 0;
    guard.head = guard.used % guard.capacity;
    drop(guard);
    unsafe { ffi::kfree(old_data as *mut c_void) };
    new_capacity as c_int
}

#[no_mangle]
pub extern "C" fn pipe_read_close(vf: *mut vfile_t) -> c_int {
    let pb = unsafe { pipe_from_vfile(vf) };
    if pb.is_null() {
        return 0;
    }
    let (last_reader, refs, writers, data_ptr) = {
        let mut guard = unsafe { (*pb).lock.lock() };
        let last_reader = unsafe { ffi::vfile_ref_read(vf) } == 0;
        if last_reader {
            guard.reader_closed = true;
        }
        guard.refs -= 1;
        (last_reader, guard.refs, &mut guard.write_waiters as *mut wait_queue_t, guard.data)
    };
    if last_reader {
        unsafe { ffi::wait_queue_wake_all(writers) };
    }
    if refs == 0 {
        unsafe {
            if !data_ptr.is_null() {
                ffi::kfree(data_ptr as *mut c_void);
            }
            ptr::drop_in_place(pb);
            ffi::kfree(pb as *mut c_void);
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn pipe_write_close(vf: *mut vfile_t) -> c_int {
    let pb = unsafe { pipe_from_vfile(vf) };
    if pb.is_null() {
        return 0;
    }
    let (last_writer, refs, readers, data_ptr) = {
        let mut guard = unsafe { (*pb).lock.lock() };
        let last_writer = unsafe { ffi::vfile_ref_read(vf) } == 0;
        if last_writer {
            guard.writer_closed = true;
        }
        guard.refs -= 1;
        (last_writer, guard.refs, &mut guard.read_waiters as *mut wait_queue_t, guard.data)
    };
    if last_writer {
        unsafe { ffi::wait_queue_wake_all(readers) };
    }
    if refs == 0 {
        unsafe {
            if !data_ptr.is_null() {
                ffi::kfree(data_ptr as *mut c_void);
            }
            ptr::drop_in_place(pb);
            ffi::kfree(pb as *mut c_void);
        }
    }
    0
}

#[no_mangle]
pub extern "C" fn pipe_vfile_is(vf: *mut vfile_t) -> c_int {
    if vf.is_null() {
        return 0;
    }
    unsafe {
        if ffi::a20_pipe_vfile_ops_eq(vf, ptr::addr_of!(PIPE_READ_OPS) as *mut vfile_ops_t) != 0
            || ffi::a20_pipe_vfile_ops_eq(vf, ptr::addr_of!(PIPE_WRITE_OPS) as *mut vfile_ops_t) != 0
        {
            1
        } else {
            0
        }
    }
}

#[no_mangle]
pub extern "C" fn pipe_poll_events(vf: *mut vfile_t, events: i16) -> c_int {
    if pipe_vfile_is(vf) == 0 {
        return ffi::POLLNVAL as c_int;
    }
    let pb = unsafe { pipe_from_vfile(vf) };
    if pb.is_null() {
        return ffi::POLLNVAL as c_int;
    }
    let guard = unsafe { (*pb).lock.lock() };
    let mut revents: i16 = 0;
    let is_reader = unsafe { ffi::a20_pipe_vfile_ops_eq(vf, ptr::addr_of!(PIPE_READ_OPS) as *mut vfile_ops_t) } != 0;
    if is_reader {
        if (events & ffi::POLLIN) != 0 && (guard.used > 0 || guard.writer_closed) {
            revents |= ffi::POLLIN;
        }
        if guard.writer_closed {
            revents |= ffi::POLLHUP;
        }
    } else {
        if (events & ffi::POLLOUT) != 0 && guard.used < guard.capacity && !guard.reader_closed {
            revents |= ffi::POLLOUT;
        }
        if guard.reader_closed {
            revents |= ffi::POLLERR;
        }
    }
    revents as c_int
}

#[no_mangle]
pub extern "C" fn pipe_get_size(vf: *mut vfile_t) -> c_int {
    if pipe_vfile_is(vf) == 0 {
        return -ffi::EINVAL;
    }
    let pb = unsafe { pipe_from_vfile(vf) };
    if pb.is_null() {
        return -ffi::EINVAL;
    }
    let guard = unsafe { (*pb).lock.lock() };
    if guard.logical_size != 0 { guard.logical_size as c_int } else { ffi::PIPE_BUF_SIZE as c_int }
}

#[no_mangle]
pub extern "C" fn pipe_set_size(vf: *mut vfile_t, size: usize) -> c_int {
    if pipe_vfile_is(vf) == 0 {
        return -ffi::EINVAL;
    }
    let pb = unsafe { pipe_from_vfile(vf) };
    if pb.is_null() {
        return -ffi::EINVAL;
    }
    pipe_resize_locked(pb, size)
}

#[no_mangle]
pub extern "C" fn pipe_create(pipefd: *mut c_int) -> c_int {
    let pb = unsafe { ffi::kmalloc(mem::size_of::<PipeBuf>()) as *mut PipeBuf };
    if pb.is_null() {
        return -ffi::ENOMEM;
    }
    let data = unsafe { ffi::kmalloc(PIPE_DEFAULT_SIZE) as *mut u8 };
    if data.is_null() {
        unsafe { ffi::kfree(pb as *mut c_void) };
        return -ffi::ENOMEM;
    }

    let mut state = PipeState {
        data,
        capacity: PIPE_DEFAULT_SIZE,
        head: 0,
        tail: 0,
        used: 0,
        logical_size: PIPE_DEFAULT_SIZE,
        writer_closed: false,
        reader_closed: false,
        refs: 2,
        read_waiters: unsafe { mem::zeroed() },
        write_waiters: unsafe { mem::zeroed() },
    };
    unsafe {
        ffi::wait_queue_init(&mut state.read_waiters);
        ffi::wait_queue_init(&mut state.write_waiters);
        ptr::write(pb, PipeBuf { lock: IrqSaveSpinLock::new(state) });
    }

    let rd = unsafe { ffi::vfile_alloc() };
    let wr = unsafe { ffi::vfile_alloc() };
    if rd.is_null() || wr.is_null() {
        unsafe {
            if !rd.is_null() { ffi::vfile_free(rd) };
            if !wr.is_null() { ffi::vfile_free(wr) };
            ffi::kfree(data as *mut c_void);
            ptr::drop_in_place(pb);
            ffi::kfree(pb as *mut c_void);
        }
        return -ffi::ENOMEM;
    }

    unsafe {
        ffi::a20_pipe_vfile_init(rd, ptr::addr_of!(PIPE_READ_OPS) as *mut vfile_ops_t, pb as *mut c_void, ffi::O_RDONLY);
        ffi::a20_pipe_vfile_init(wr, ptr::addr_of!(PIPE_WRITE_OPS) as *mut vfile_ops_t, pb as *mut c_void, ffi::O_WRONLY);
    }

    let fdrd = unsafe { ffi::vfs_alloc_fd(rd) };
    let fdwr = unsafe { ffi::vfs_alloc_fd(wr) };
    if fdrd < 0 || fdwr < 0 {
        unsafe {
            if fdrd >= 0 { ffi::vfs_close(fdrd); } else { ffi::vfile_free(rd); }
            if fdwr >= 0 { ffi::vfs_close(fdwr); } else { ffi::vfile_free(wr); }
            let guard = (*pb).lock.lock();
            let refs = guard.refs;
            let data_ptr = guard.data;
            drop(guard);
            if refs > 0 {
                if !data_ptr.is_null() {
                    ffi::kfree(data_ptr as *mut c_void);
                }
                ptr::drop_in_place(pb);
                ffi::kfree(pb as *mut c_void);
            }
        }
        return -ffi::EMFILE;
    }

    unsafe {
        *pipefd.add(0) = fdrd;
        *pipefd.add(1) = fdwr;
    }
    0
}
