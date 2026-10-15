#![no_std]

//! Rust-owned page iteration for cross-address-space transfers.
//!
//! Page-table policy stays in C. This module owns the pinned MM reference,
//! each short-lived frame pin, COW retries, and partial byte-count result.

use core::ffi::c_void;
use core::ptr;

const EINVAL: isize = -22;
const EFAULT: isize = -14;
const EAGAIN: i32 = -11;
const MAX_IOV: usize = 32;
const MAX_CHUNK: usize = 65536;

#[inline]
fn checked_iov_address(base: *mut c_void, offset: usize) -> Option<usize> {
    (base as usize).checked_add(offset)
}

#[repr(C)]
#[derive(Clone, Copy)]
struct LinuxIovec {
    base: *mut c_void,
    length: usize,
}

unsafe extern "C" {
    fn a20_process_vm_mm_get(task: *mut c_void) -> *mut c_void;
    fn a20_process_vm_mm_put(mm: *mut c_void);
    fn a20_process_vm_range_ok(address: usize, length: usize) -> i32;
    fn a20_process_vm_pin_leaf(
        mm: *mut c_void,
        address: usize,
        for_write: i32,
        kernel_address: *mut *mut u8,
        available: *mut usize,
        frame_token: *mut usize,
        write_guard: *mut i32,
    ) -> i32;
    fn a20_process_vm_frame_put(frame_token: usize);
    fn mm_shared_write_leave();
    fn a20_process_vm_break_cow(task: *mut c_void, mm: *mut c_void, address: usize) -> i32;
    fn a20_process_vm_mark_dirty(mm: *mut c_void, address: usize);
    fn a20_process_vm_target_get(pid: i32, task_out: *mut *mut c_void) -> i32;
    fn a20_process_vm_target_put(task: *mut c_void);
    fn a20_process_vm_copy_from_user(dst: *mut c_void, src: *const c_void, len: usize) -> isize;
    fn a20_process_vm_copy_to_user(dst: *mut c_void, src: *const c_void, len: usize) -> isize;
    fn a20_process_vm_scratch_buffer(size: usize) -> *mut u8;
}

struct MmRef(*mut c_void);
impl Drop for MmRef {
    fn drop(&mut self) {
        unsafe { a20_process_vm_mm_put(self.0) }
    }
}

struct FrameRef(usize);
impl Drop for FrameRef {
    fn drop(&mut self) {
        unsafe { a20_process_vm_frame_put(self.0) }
    }
}

struct SharedWriteGuard;
impl Drop for SharedWriteGuard {
    fn drop(&mut self) {
        unsafe { mm_shared_write_leave() }
    }
}

/// One page-bounded remote mapping. The C callback pins the frame while it
/// holds the page-table locks; this guard owns that pin across the copy and
/// releases it before dirty bookkeeping can re-resolve the mapping.
struct PinnedLeaf {
    kernel_address: *mut u8,
    available: usize,
    _frame: FrameRef,
    _write_guard: Option<SharedWriteGuard>,
}

struct Context<'a> {
    task: *mut c_void,
    mm: *mut c_void,
    _mm_ref: &'a MmRef,
}

unsafe fn pin_leaf(
    context: &Context<'_>,
    address: usize,
    for_write: bool,
) -> Result<PinnedLeaf, isize> {
    loop {
        let mut kernel_address = ptr::null_mut();
        let mut available = 0usize;
        let mut token = 0usize;
        let mut write_guard = 0i32;
        let result = unsafe {
            a20_process_vm_pin_leaf(
                context.mm,
                address,
                i32::from(for_write),
                &mut kernel_address,
                &mut available,
                &mut token,
                &mut write_guard,
            )
        };
        if result == EAGAIN && for_write {
            // Retry against the same pinned mm. The target task may have
            // exec'd since we acquired this mm reference; COW must not follow
            // task->mm to a different address space.
            if unsafe { a20_process_vm_break_cow(context.task, context.mm, address) } < 0 {
                return Err(EFAULT);
            }
            continue;
        }
        if result != 0 {
            return Err(EFAULT);
        }
        let frame = FrameRef(token);
        if kernel_address.is_null() || available == 0 {
            drop(frame);
            return Err(EFAULT);
        }
        return Ok(PinnedLeaf {
            kernel_address,
            available,
            _frame: frame,
            _write_guard: if write_guard != 0 {
                Some(SharedWriteGuard)
            } else {
                None
            },
        });
    }
}

unsafe fn transfer_on_mm(
    task: *mut c_void,
    mm_ref: &MmRef,
    user: usize,
    kernel: *mut u8,
    length: usize,
    to_user: bool,
) -> isize {
    if task.is_null() || user == 0 || kernel.is_null() {
        return EINVAL;
    }
    let mm = mm_ref.0;
    if mm.is_null() {
        return EINVAL;
    }
    if unsafe { a20_process_vm_range_ok(user, length) } == 0 {
        return EFAULT;
    }
    let context = Context { task, mm, _mm_ref: &mm_ref };
    let mut done = 0usize;
    while done < length {
        let address = match user.checked_add(done) {
            Some(address) => address,
            None => return if done > 0 { done as isize } else { EFAULT },
        };
        let leaf = match unsafe { pin_leaf(&context, address, to_user) } {
            Ok(leaf) => leaf,
            Err(error) => return if done > 0 { done as isize } else { error },
        };
        let count = leaf.available.min(length - done);
        unsafe {
            if to_user {
                ptr::copy_nonoverlapping(kernel.add(done), leaf.kernel_address, count);
            } else {
                ptr::copy_nonoverlapping(leaf.kernel_address, kernel.add(done), count);
            }
        }
        if to_user {
            unsafe { a20_process_vm_mark_dirty(context.mm, address) };
        }
        drop(leaf);
        done += count;
    }
    drop(context);
    done as isize
}

unsafe fn transfer(
    task: *mut c_void,
    user: usize,
    kernel: *mut u8,
    length: usize,
    to_user: bool,
) -> isize {
    if task.is_null() || user == 0 || kernel.is_null() {
        return EINVAL;
    }
    let mm = unsafe { a20_process_vm_mm_get(task) };
    if mm.is_null() {
        return EINVAL;
    }
    let mm_ref = MmRef(mm);
    unsafe { transfer_on_mm(task, &mm_ref, user, kernel, length, to_user) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_process_vm_read_kernel_rust(
    task: *mut c_void,
    src: *const c_void,
    dst: *mut c_void,
    length: usize,
) -> isize {
    unsafe { transfer(task, src as usize, dst.cast(), length, false) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_process_vm_write_kernel_rust(
    task: *mut c_void,
    dst: *mut c_void,
    src: *const c_void,
    length: usize,
) -> isize {
    unsafe { transfer(task, dst as usize, src.cast_mut().cast(), length, true) }
}

struct TaskRef(*mut c_void);
impl Drop for TaskRef {
    fn drop(&mut self) {
        unsafe { a20_process_vm_target_put(self.0) }
    }
}

unsafe fn io_common(
    pid: i32,
    remote_iov: *const c_void,
    remote_count: usize,
    local_iov: *const c_void,
    local_count: usize,
    to_target: bool,
) -> isize {
    if remote_count > MAX_IOV || local_count > MAX_IOV {
        return EINVAL;
    }
    let mut remote = [LinuxIovec { base: ptr::null_mut(), length: 0 }; MAX_IOV];
    let mut local = [LinuxIovec { base: ptr::null_mut(), length: 0 }; MAX_IOV];
    if remote_count != 0
        && unsafe {
            a20_process_vm_copy_from_user(
                remote.as_mut_ptr().cast(), remote_iov, remote_count * core::mem::size_of::<LinuxIovec>(),
            )
        } < 0
    {
        return EFAULT;
    }
    if local_count != 0
        && unsafe {
            a20_process_vm_copy_from_user(
                local.as_mut_ptr().cast(), local_iov, local_count * core::mem::size_of::<LinuxIovec>(),
            )
        } < 0
    {
        return EFAULT;
    }

    let mut task = ptr::null_mut();
    let lookup = unsafe { a20_process_vm_target_get(pid, &mut task) };
    if lookup < 0 {
        return lookup as isize;
    }
    if task.is_null() {
        return EFAULT;
    }
    let task_ref = TaskRef(task);
    let mut mm_ref: Option<MmRef> = None;
    let mut total = 0usize;
    let (mut ri, mut li, mut roff, mut loff) = (0usize, 0usize, 0usize, 0usize);
    while ri < remote_count && li < local_count {
        let rlen = remote[ri].length - roff;
        let llen = local[li].length - loff;
        if rlen == 0 && llen == 0 {
            ri += 1; li += 1; roff = 0; loff = 0; continue;
        }
        if rlen == 0 { ri += 1; roff = 0; continue; }
        if llen == 0 { li += 1; loff = 0; continue; }
        let pair_len = rlen.min(llen);
        let mut done = 0usize;
        while done < pair_len {
            let chunk = (pair_len - done).min(MAX_CHUNK);
            let scratch = unsafe { a20_process_vm_scratch_buffer(chunk) };
            if scratch.is_null() {
                drop(task_ref);
                return if total + done > 0 { (total + done) as isize } else { -12 };
            }
            let remote_addr = match checked_iov_address(remote[ri].base, roff + done) {
                Some(address) => address as *mut u8,
                None => {
                    drop(task_ref);
                    return if total + done > 0 { (total + done) as isize } else { EFAULT };
                }
            };
            let local_addr = match checked_iov_address(local[li].base, loff + done) {
                Some(address) => address as *mut u8,
                None => {
                    drop(task_ref);
                    return if total + done > 0 { (total + done) as isize } else { EFAULT };
                }
            };
            if to_target {
                if unsafe { a20_process_vm_copy_from_user(scratch.cast(), local_addr.cast(), chunk) } < 0 {
                    drop(task_ref);
                    return if total + done > 0 { (total + done) as isize } else { EFAULT };
                }
                if mm_ref.is_none() {
                    let mm = unsafe { a20_process_vm_mm_get(task) };
                    if mm.is_null() {
                        drop(task_ref);
                        return if total + done > 0 { (total + done) as isize } else { EFAULT };
                    }
                    mm_ref = Some(MmRef(mm));
                }
                let held_mm = match mm_ref.as_ref() {
                    Some(mm) => mm,
                    None => {
                        drop(task_ref);
                        return if total + done > 0 { (total + done) as isize } else { EFAULT };
                    }
                };
                let n = unsafe {
                    transfer_on_mm(task, held_mm, remote_addr as usize, scratch, chunk, true)
                };
                if n <= 0 {
                    drop(task_ref);
                    return if total + done > 0 { (total + done) as isize } else { n };
                }
                done += n as usize;
            } else {
                if mm_ref.is_none() {
                    let mm = unsafe { a20_process_vm_mm_get(task) };
                    if mm.is_null() {
                        drop(task_ref);
                        return if total + done > 0 { (total + done) as isize } else { EFAULT };
                    }
                    mm_ref = Some(MmRef(mm));
                }
                let held_mm = match mm_ref.as_ref() {
                    Some(mm) => mm,
                    None => {
                        drop(task_ref);
                        return if total + done > 0 { (total + done) as isize } else { EFAULT };
                    }
                };
                let n = unsafe {
                    transfer_on_mm(task, held_mm, remote_addr as usize, scratch, chunk, false)
                };
                if n <= 0 {
                    drop(task_ref);
                    return if total + done > 0 { (total + done) as isize } else { n };
                }
                if unsafe { a20_process_vm_copy_to_user(local_addr.cast(), scratch.cast(), n as usize) } < 0 {
                    drop(task_ref);
                    return if total + done > 0 { (total + done) as isize } else { EFAULT };
                }
                done += n as usize;
            }
        }
        total += done;
        roff += done;
        loff += done;
        if roff == remote[ri].length { ri += 1; roff = 0; }
        if loff == local[li].length { li += 1; loff = 0; }
    }
    drop(task_ref);
    total as isize
}

#[no_mangle]
pub unsafe extern "C" fn a20_sys_process_vm_readv_rust(
    pid: i32,
    local_iov: *const c_void,
    local_count: usize,
    remote_iov: *const c_void,
    remote_count: usize,
    _flags: usize,
) -> isize {
    unsafe { io_common(pid, remote_iov, remote_count, local_iov, local_count, false) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_sys_process_vm_writev_rust(
    pid: i32,
    local_iov: *const c_void,
    local_count: usize,
    remote_iov: *const c_void,
    remote_count: usize,
    _flags: usize,
) -> isize {
    unsafe { io_common(pid, remote_iov, remote_count, local_iov, local_count, true) }
}
