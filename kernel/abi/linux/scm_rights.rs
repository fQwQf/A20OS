#![no_std]

//! Linux `SCM_RIGHTS` descriptor lifetime transaction.
//!
//! Usercopy and the concrete fd table remain in the C ABI layer.  This module
//! owns ancillary framing, bounded acquisition rollback, queue ownership
//! transfer, receive installation accounting, and abort of descriptors which
//! have not yet been exposed to userspace.

#[cfg(test)]
extern crate std;

use core::ffi::c_void;
use core::ptr;

const SOL_SOCKET: i32 = 1;
const SCM_RIGHTS: i32 = 1;
const MAX_FDS: usize = 16;
const EINVAL: i32 = 22;
// Keep in sync with kernel/include/net/socket.h: Linux MSG_CTRUNC is 0x0008.
const MSG_CTRUNC: i32 = 0x0008;
const NO_RIGHTS: i64 = i64::MIN;

#[repr(C)]
#[derive(Clone, Copy)]
struct Cmsghdr {
    len: usize,
    level: i32,
    kind: i32,
}

type AcquireFn = unsafe extern "C" fn(i32, *mut c_void, *mut *mut c_void) -> i32;
type ReleaseFn = unsafe extern "C" fn(*mut c_void, *mut c_void);
type EnqueueFn = unsafe extern "C" fn(
    *mut c_void,
    *mut *mut c_void,
    i32,
) -> i64;
type InstallFn = unsafe extern "C" fn(
    *mut c_void,
    *mut c_void,
    i32,
    *mut *mut c_void,
) -> i32;
type CloseFn = unsafe extern "C" fn(*mut c_void, i32, *mut c_void);
type QueueReleaseFn = unsafe extern "C" fn(*mut c_void, *mut c_void);

#[repr(C)]
pub struct LinuxScmRecvTxn {
    installed: [i32; MAX_FDS],
    pinned_files: [*mut c_void; MAX_FDS],
    count: usize,
    active: u32,
}

impl LinuxScmRecvTxn {
    const fn empty() -> Self {
        Self {
            installed: [-1; MAX_FDS],
            pinned_files: [ptr::null_mut(); MAX_FDS],
            count: 0,
            active: 0,
        }
    }
}

fn align_len(n: usize) -> Option<usize> {
    let a = core::mem::size_of::<usize>();
    n.checked_add(a - 1).map(|v| v & !(a - 1))
}

fn cmsg_space(payload: usize) -> Option<usize> {
    align_len(core::mem::size_of::<Cmsghdr>())?.checked_add(align_len(payload)?)
}

fn cmsg_len(payload: usize) -> Option<usize> {
    align_len(core::mem::size_of::<Cmsghdr>())?.checked_add(payload)
}

unsafe fn release_all(
    files: *mut *mut c_void,
    count: usize,
    ctx: *mut c_void,
    release: ReleaseFn,
) {
    if files.is_null() {
        return;
    }
    for i in 0..count {
        let file = ptr::read(files.add(i));
        if !file.is_null() {
            ptr::write(files.add(i), ptr::null_mut());
            release(ctx, file);
        }
    }
}

/// Validate and enqueue a send-side SCM_RIGHTS transaction.
///
/// The acquire callback must return one owned file reference in `out` only on
/// success.  The enqueue callback borrows the staged array and takes ownership
/// of all entries only when it returns a nonnegative result.  Every other
/// outcome drops every acquired reference here.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_scm_rights_send(
    control: *const u8,
    control_len: usize,
    ctx: *mut c_void,
    acquire: Option<AcquireFn>,
    release: Option<ReleaseFn>,
    enqueue: Option<EnqueueFn>,
) -> i64 {
    let (Some(acquire), Some(release), Some(enqueue)) = (acquire, release, enqueue) else {
        return -(EINVAL as i64);
    };
    if control.is_null() || control_len == 0 {
        return NO_RIGHTS;
    }
    if control_len < core::mem::size_of::<Cmsghdr>() {
        return -(EINVAL as i64);
    }

    let header_size = align_len(core::mem::size_of::<Cmsghdr>()).unwrap_or(usize::MAX);
    let mut files = [ptr::null_mut(); MAX_FDS];
    let mut count = 0usize;
    let mut offset = 0usize;
    let mut found_rights = false;

    while control_len - offset >= core::mem::size_of::<Cmsghdr>() {
        let header_ptr = control.add(offset).cast::<Cmsghdr>();
        let header = ptr::read_unaligned(header_ptr);
        let remaining = control_len - offset;
        if header.len < header_size || header.len > remaining {
            release_all(files.as_mut_ptr(), count, ctx, release);
            return -(EINVAL as i64);
        }

        if header.level == SOL_SOCKET && header.kind == SCM_RIGHTS {
            found_rights = true;
            let payload_len = header.len - header_size;
            if payload_len % core::mem::size_of::<i32>() != 0 {
                release_all(files.as_mut_ptr(), count, ctx, release);
                return -(EINVAL as i64);
            }
            let n = payload_len / core::mem::size_of::<i32>();
            if n > MAX_FDS.saturating_sub(count) {
                release_all(files.as_mut_ptr(), count, ctx, release);
                return -(EINVAL as i64);
            }
            let data = control.add(offset + header_size).cast::<i32>();
            for j in 0..n {
                let fd = ptr::read_unaligned(data.add(j));
                let mut file = ptr::null_mut();
                let rc = acquire(fd, ctx, &mut file);
                if rc < 0 || file.is_null() {
                    if !file.is_null() {
                        release(ctx, file);
                    }
                    release_all(files.as_mut_ptr(), count, ctx, release);
                    return if rc < 0 { rc as i64 } else { -(EINVAL as i64) };
                }
                files[count] = file;
                count += 1;
            }
        }

        let step = match align_len(header.len) {
            Some(step) => step,
            None => {
                release_all(files.as_mut_ptr(), count, ctx, release);
                return -(EINVAL as i64);
            }
        };
        if step > remaining || remaining - step < core::mem::size_of::<Cmsghdr>() {
            break;
        }
        offset += step;
    }

    if !found_rights || count == 0 {
        return NO_RIGHTS;
    }
    let result = enqueue(ctx, files.as_mut_ptr(), count as i32);
    if result < 0 {
        release_all(files.as_mut_ptr(), count, ctx, release);
    }
    result
}

/// Stage receive-side descriptor installation.  The install callback always
/// consumes the supplied reference: on failure it must drop that reference;
/// on success the fd slot owns it.  The Rust transaction keeps the installed
/// fd numbers until userspace copyout commits or aborts.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_scm_rights_recv_begin(
    files: *mut *mut c_void,
    count: i32,
    control_capacity: usize,
    used: usize,
    cloexec: i32,
    ctx: *mut c_void,
    install: Option<InstallFn>,
    release: Option<ReleaseFn>,
    txn: *mut LinuxScmRecvTxn,
    out_fds: *mut i32,
    out_count: *mut usize,
) -> i32 {
    let (Some(install), Some(release)) = (install, release) else {
        return -EINVAL;
    };
    if txn.is_null() || out_count.is_null() || count < 0 || count as usize > MAX_FDS {
        return -EINVAL;
    }
    ptr::write(txn, LinuxScmRecvTxn::empty());
    ptr::write(out_count, 0);
    if count == 0 {
        return 0;
    }
    if files.is_null() {
        return -EINVAL;
    }
    let n = count as usize;
    let available = control_capacity.saturating_sub(used);
    let fit_n = (1..=n)
        .rev()
        .find(|fit| cmsg_len(fit * core::mem::size_of::<i32>())
            .is_some_and(|length| length <= available))
        .unwrap_or(0);
    if fit_n == 0 {
        release_all(files, n, ctx, release);
        (*txn).active = 1;
        return MSG_CTRUNC;
    }
    if out_fds.is_null() {
        release_all(files, n, ctx, release);
        return -EINVAL;
    }

    (*txn).active = 1;
    let mut delivered = 0usize;
    let mut truncated = fit_n < n;
    for i in 0..fit_n {
        let file = ptr::read(files.add(i));
        ptr::write(files.add(i), ptr::null_mut());
        if file.is_null() {
            truncated = true;
            continue;
        }
        let mut pinned = ptr::null_mut();
        let fd = install(ctx, file, cloexec, &mut pinned);
        if fd < 0 {
            truncated = true;
            if !pinned.is_null() {
                release(ctx, pinned);
            }
            continue;
        }
        (*txn).installed[(*txn).count] = fd;
        (*txn).pinned_files[(*txn).count] = pinned;
        (*txn).count += 1;
        ptr::write(out_fds.add(delivered), fd);
        delivered += 1;
    }
    release_all(files, n, ctx, release);
    ptr::write(out_count, delivered);
    if truncated || delivered == 0 { MSG_CTRUNC } else { 0 }
}

/// Commit makes successfully copied descriptor numbers visible to userspace.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_scm_rights_recv_commit(
    txn: *mut LinuxScmRecvTxn,
    ctx: *mut c_void,
    release: Option<ReleaseFn>,
) {
    let Some(release) = release else { return };
    if !txn.is_null() && (*txn).active != 0 {
        for i in 0..(*txn).count {
            if !(*txn).pinned_files[i].is_null() {
                release(ctx, (*txn).pinned_files[i]);
                (*txn).pinned_files[i] = ptr::null_mut();
            }
        }
        (*txn).count = 0;
        (*txn).active = 0;
    }
}

/// Abort an uncommitted receive after ancillary-data copyout fails.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_scm_rights_recv_abort(
    txn: *mut LinuxScmRecvTxn,
    ctx: *mut c_void,
    close: Option<CloseFn>,
    release: Option<ReleaseFn>,
) {
    let (Some(close), Some(release)) = (close, release) else { return };
    if txn.is_null() || (*txn).active == 0 {
        return;
    }
    for i in 0..(*txn).count {
        close(ctx, (*txn).installed[i], (*txn).pinned_files[i]);
        if !(*txn).pinned_files[i].is_null() {
            release(ctx, (*txn).pinned_files[i]);
            (*txn).pinned_files[i] = ptr::null_mut();
        }
        (*txn).installed[i] = -1;
    }
    (*txn).count = 0;
    (*txn).active = 0;
}

/// Move queued message references into recv metadata at the first-byte
/// boundary.  References that do not fit remain in the queued message and are
/// released by queue_drop when the message is consumed or discarded.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_scm_queue_take(
    queued_files: *mut *mut c_void,
    queued_count: *mut i32,
    recv_files: *mut *mut c_void,
    recv_count: *mut i32,
    capacity: i32,
) -> i32 {
    if queued_count.is_null() || recv_count.is_null() || capacity < 0 {
        return -EINVAL;
    }
    let queued_n = *queued_count;
    let recv_n = *recv_count;
    if queued_n < 0 || recv_n < 0 || recv_n > capacity || queued_n as usize > MAX_FDS {
        return -EINVAL;
    }
    if queued_n == 0 {
        return 0;
    }
    if queued_files.is_null() || recv_files.is_null() {
        return -EINVAL;
    }
    let queued_n = queued_n as usize;
    let move_n = queued_n.min((capacity - recv_n) as usize);
    for i in 0..move_n {
        let file = ptr::read(queued_files.add(i));
        ptr::write(recv_files.add(recv_n as usize + i), file);
        ptr::write(queued_files.add(i), ptr::null_mut());
    }
    let left = queued_n - move_n;
    for i in 0..left {
        let file = ptr::read(queued_files.add(move_n + i));
        ptr::write(queued_files.add(i), file);
        if move_n + i != i {
            ptr::write(queued_files.add(move_n + i), ptr::null_mut());
        }
    }
    *recv_count += move_n as i32;
    *queued_count = left as i32;
    0
}

/// Attach the send transaction's references to a freshly enqueued message.
/// The caller holds the destination socket lock and has already checked the
/// queue capacity; success empties the source array atomically from the
/// transaction's point of view.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_scm_queue_attach(
    source_files: *mut *mut c_void,
    count: i32,
    queued_files: *mut *mut c_void,
    queued_capacity: i32,
) -> i32 {
    if count < 0 || count as usize > MAX_FDS || queued_capacity < count {
        return -EINVAL;
    }
    if count == 0 {
        return 0;
    }
    if source_files.is_null() || queued_files.is_null() {
        return -EINVAL;
    }
    for i in 0..count as usize {
        let file = ptr::read(source_files.add(i));
        if file.is_null() {
            return -EINVAL;
        }
    }
    for i in 0..count as usize {
        let file = ptr::read(source_files.add(i));
        ptr::write(queued_files.add(i), file);
        ptr::write(source_files.add(i), ptr::null_mut());
    }
    0
}

/// Release every reference still owned by a queued message during teardown.
#[no_mangle]
pub unsafe extern "C" fn a20_linux_scm_queue_drop(
    queued_files: *mut *mut c_void,
    queued_count: *mut i32,
    ctx: *mut c_void,
    release: Option<QueueReleaseFn>,
) {
    let Some(release) = release else { return };
    if queued_count.is_null() || queued_files.is_null() {
        return;
    }
    let n = (*queued_count).clamp(0, MAX_FDS as i32) as usize;
    for i in 0..n {
        let file = ptr::read(queued_files.add(i));
        ptr::write(queued_files.add(i), ptr::null_mut());
        if !file.is_null() {
            release(ctx, file);
        }
    }
    *queued_count = 0;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::vec::Vec;

    #[derive(Default)]
    struct SendState {
        live: Vec<i32>,
        released: Vec<i32>,
        delivered: Vec<i32>,
        fail_fd: i32,
        enqueue_result: i64,
    }

    unsafe extern "C" fn acquire(fd: i32, ctx: *mut c_void,
                                 out: *mut *mut c_void) -> i32 {
        let state = &mut *(ctx as *mut SendState);
        if fd == state.fail_fd {
            return -9;
        }
        state.live.push(fd);
        *out = (fd as usize + 1) as *mut c_void;
        0
    }

    unsafe extern "C" fn release(ctx: *mut c_void, file: *mut c_void) {
        let state = &mut *(ctx as *mut SendState);
        let id = file as usize as i32 - 1;
        state.live.retain(|value| *value != id);
        state.released.push(id);
    }

    unsafe extern "C" fn enqueue(ctx: *mut c_void, files: *mut *mut c_void,
                                  count: i32) -> i64 {
        let state = &mut *(ctx as *mut SendState);
        for i in 0..count as usize {
            state.delivered.push(*files.add(i) as usize as i32 - 1);
        }
        state.enqueue_result
    }

    fn append_cmsg(control: &mut Vec<u8>, fds: &[i32]) {
        let header_size = core::mem::size_of::<Cmsghdr>();
        let offset = align_len(control.len()).unwrap();
        control.resize(offset + header_size + fds.len() * 4, 0);
        let header = Cmsghdr {
            len: header_size + fds.len() * 4,
            level: SOL_SOCKET,
            kind: SCM_RIGHTS,
        };
        unsafe {
            ptr::write_unaligned(control.as_mut_ptr().add(offset).cast::<Cmsghdr>(), header);
            for (i, fd) in fds.iter().enumerate() {
                ptr::write_unaligned(
                    control.as_mut_ptr().add(offset + header_size + i * 4).cast::<i32>(),
                    *fd,
                );
            }
        }
    }

    #[test]
    fn send_multiple_headers_transfer_all_refs_on_enqueue_success() {
        let mut control = Vec::new();
        append_cmsg(&mut control, &[2]);
        append_cmsg(&mut control, &[4, 7]);
        let mut state = SendState { enqueue_result: 3, fail_fd: -1, ..Default::default() };
        let result = unsafe {
            a20_linux_scm_rights_send(control.as_ptr(), control.len(),
                &mut state as *mut _ as *mut c_void, Some(acquire), Some(release), Some(enqueue))
        };
        assert_eq!(result, 3);
        assert_eq!(state.delivered, [2, 4, 7]);
        assert!(state.released.is_empty());
        assert_eq!(state.live, [2, 4, 7]);
    }

    #[test]
    fn malformed_later_header_rolls_back_prior_acquisitions() {
        let mut control = Vec::new();
        append_cmsg(&mut control, &[8]);
        let offset = align_len(control.len()).unwrap();
        control.resize(offset + core::mem::size_of::<Cmsghdr>(), 0);
        unsafe {
            ptr::write_unaligned(control.as_mut_ptr().add(offset).cast::<Cmsghdr>(),
                Cmsghdr { len: usize::MAX, level: SOL_SOCKET, kind: SCM_RIGHTS });
        }
        let mut state = SendState { fail_fd: -1, ..Default::default() };
        let result = unsafe {
            a20_linux_scm_rights_send(control.as_ptr(), control.len(),
                &mut state as *mut _ as *mut c_void, Some(acquire), Some(release), Some(enqueue))
        };
        assert_eq!(result, -(EINVAL as i64));
        assert_eq!(state.released, [8]);
        assert!(state.live.is_empty());
    }

    #[test]
    fn failed_resolution_and_enqueue_release_every_staged_reference() {
        let mut control = Vec::new();
        append_cmsg(&mut control, &[3, 99]);
        let mut state = SendState { fail_fd: 99, ..Default::default() };
        let result = unsafe {
            a20_linux_scm_rights_send(control.as_ptr(), control.len(),
                &mut state as *mut _ as *mut c_void, Some(acquire), Some(release), Some(enqueue))
        };
        assert_eq!(result, -9);
        assert_eq!(state.released, [3]);
        assert!(state.live.is_empty());

        state = SendState { fail_fd: -1, enqueue_result: -11, ..Default::default() };
        let result = unsafe {
            a20_linux_scm_rights_send(control.as_ptr(), control.len(),
                &mut state as *mut _ as *mut c_void, Some(acquire), Some(release), Some(enqueue))
        };
        assert_eq!(result, -11);
        assert_eq!(state.released, [3, 99]);
        assert!(state.live.is_empty());
    }

    #[derive(Default)]
    struct RecvState {
        refs: [i32; 32],
        slots: [i32; 32],
        fail_id: i32,
        closes: Vec<i32>,
    }

    unsafe extern "C" fn recv_release(ctx: *mut c_void, file: *mut c_void) {
        let state = &mut *(ctx as *mut RecvState);
        let id = file as usize as i32 - 1;
        state.refs[id as usize] -= 1;
    }

    unsafe extern "C" fn install(ctx: *mut c_void, file: *mut c_void,
                                  _cloexec: i32, pin_out: *mut *mut c_void) -> i32 {
        let state = &mut *(ctx as *mut RecvState);
        let id = file as usize as i32 - 1;
        if id == state.fail_id {
            recv_release(ctx, file);
            return -24;
        }
        let fd = 10 + id;
        state.slots[fd as usize] = id;
        state.refs[id as usize] += 1; // retained transaction identity pin
        *pin_out = file;
        fd
    }

    unsafe extern "C" fn close_if_file(ctx: *mut c_void, fd: i32,
                                        expected: *mut c_void) {
        let state = &mut *(ctx as *mut RecvState);
        let id = expected as usize as i32 - 1;
        if state.slots[fd as usize] == id {
            state.slots[fd as usize] = 0;
            state.refs[id as usize] -= 1;
            state.closes.push(id);
        }
    }

    fn recv_files(ids: &[i32], state: &mut RecvState) -> Vec<*mut c_void> {
        ids.iter().map(|id| {
            state.refs[*id as usize] += 1;
            (*id as usize + 1) as *mut c_void
        }).collect()
    }

    #[test]
    fn receive_partial_install_and_abort_close_only_matching_slots() {
        let mut state = RecvState { fail_id: 4, ..Default::default() };
        let mut files = recv_files(&[3, 4, 5], &mut state);
        let mut txn = LinuxScmRecvTxn::empty();
        let mut out = [0; MAX_FDS];
        let mut delivered = 0usize;
        let rc = unsafe {
            a20_linux_scm_rights_recv_begin(
                files.as_mut_ptr(), files.len() as i32, 256, 0, 1,
                &mut state as *mut _ as *mut c_void, Some(install),
                Some(recv_release), &mut txn, out.as_mut_ptr(), &mut delivered)
        };
        assert_eq!(rc, MSG_CTRUNC);
        assert_eq!(delivered, 2);
        assert_eq!(&out[..delivered], &[13, 15]);
        assert_eq!(state.refs[4], 0);

        // A CLONE_FILES sibling replaced fd 13 while copyout was in flight.
        // The close callback must leave that different object alone.
        state.slots[13] = 29;
        unsafe {
            a20_linux_scm_rights_recv_abort(
                &mut txn, &mut state as *mut _ as *mut c_void,
                Some(close_if_file), Some(recv_release))
        };
        assert_eq!(state.slots[13], 29);
        assert_eq!(state.slots[15], 0);
        assert_eq!(state.closes, [5]);
        assert_eq!(state.refs[3], 1); // replacement owns its slot reference
        assert_eq!(state.refs[5], 0);
    }

    #[test]
    fn receive_truncation_drops_queue_refs_and_commit_keeps_installed_fds() {
        let mut state = RecvState { fail_id: -1, ..Default::default() };
        let mut files = recv_files(&[6], &mut state);
        let mut txn = LinuxScmRecvTxn::empty();
        let mut out = [0; MAX_FDS];
        let mut delivered = 0usize;
        let rc = unsafe {
            a20_linux_scm_rights_recv_begin(
                files.as_mut_ptr(), 1, 0, 0, 0,
                &mut state as *mut _ as *mut c_void, Some(install),
                Some(recv_release), &mut txn, out.as_mut_ptr(), &mut delivered)
        };
        assert_eq!(rc, MSG_CTRUNC);
        assert_eq!(delivered, 0);
        assert_eq!(state.refs[6], 0);

        files = recv_files(&[7], &mut state);
        let rc = unsafe {
            a20_linux_scm_rights_recv_begin(
                files.as_mut_ptr(), 1, 64, 0, 1,
                &mut state as *mut _ as *mut c_void, Some(install),
                Some(recv_release), &mut txn, out.as_mut_ptr(), &mut delivered)
        };
        assert_eq!(rc, 0);
        assert_eq!(delivered, 1);
        unsafe {
            a20_linux_scm_rights_recv_commit(
                &mut txn, &mut state as *mut _ as *mut c_void, Some(recv_release))
        };
        assert_eq!(state.slots[17], 7);
        assert_eq!(state.refs[7], 1);
        assert!(state.closes.is_empty());
    }

    #[test]
    fn receive_control_capacity_delivers_aligned_fitting_prefix_then_truncates() {
        let mut state = RecvState { fail_id: -1, ..Default::default() };
        let mut files = recv_files(&[8, 9, 10], &mut state);
        let mut txn = LinuxScmRecvTxn::empty();
        let mut out = [0; MAX_FDS];
        let mut delivered = 0usize;
        // CMSG_SPACE(2 ints) holds exactly two on all supported word sizes;
        // on x86_64 CMSG_SPACE(1) would also equal CMSG_SPACE(2).
        let two_fd_space = cmsg_space(core::mem::size_of::<i32>() * 2).unwrap();
        let rc = unsafe {
            a20_linux_scm_rights_recv_begin(
                files.as_mut_ptr(), 3, two_fd_space, 0, 0,
                &mut state as *mut _ as *mut c_void, Some(install),
                Some(recv_release), &mut txn, out.as_mut_ptr(), &mut delivered)
        };
        assert_eq!(rc, MSG_CTRUNC);
        assert_eq!(delivered, 2);
        assert_eq!(&out[..2], &[18, 19]);
        assert_eq!(state.refs[10], 0);
        unsafe {
            a20_linux_scm_rights_recv_commit(
                &mut txn, &mut state as *mut _ as *mut c_void, Some(recv_release))
        };
        assert_eq!(state.slots[18], 8);
        assert_eq!(state.slots[19], 9);
        assert_eq!(state.refs[8], 1);
        assert_eq!(state.refs[9], 1);
    }

    #[test]
    fn receive_compact_final_cmsg_delivers_one_and_truncates() {
        let mut state = RecvState { fail_id: -1, ..Default::default() };
        let mut files = recv_files(&[8, 9], &mut state);
        let mut txn = LinuxScmRecvTxn::empty();
        let mut out = [0; MAX_FDS];
        let mut delivered = 0usize;
        let compact_len = cmsg_len(core::mem::size_of::<i32>()).unwrap();
        let rc = unsafe {
            a20_linux_scm_rights_recv_begin(
                files.as_mut_ptr(), 2, compact_len, 0, 0,
                &mut state as *mut _ as *mut c_void, Some(install),
                Some(recv_release), &mut txn, out.as_mut_ptr(), &mut delivered)
        };
        assert_eq!(rc, MSG_CTRUNC);
        assert_eq!(delivered, 1);
        assert_eq!(out[0], 18);
        assert_eq!(state.refs[9], 0);
        unsafe {
            a20_linux_scm_rights_recv_commit(
                &mut txn, &mut state as *mut _ as *mut c_void, Some(recv_release))
        };
        assert_eq!(state.slots[18], 8);
        assert_eq!(state.refs[8], 1);
    }

    #[test]
    fn queued_references_move_once_and_overflow_remains_owned_for_drop() {
        let mut source = [11usize as *mut c_void, 12usize as *mut c_void,
                         13usize as *mut c_void];
        let mut queued = [ptr::null_mut(); MAX_FDS];
        assert_eq!(unsafe {
            a20_linux_scm_queue_attach(source.as_mut_ptr(), 3, queued.as_mut_ptr(),
                                      MAX_FDS as i32)
        }, 0);
        assert_eq!(source, [ptr::null_mut(); 3]);
        assert_eq!(queued[..3], [11usize as *mut c_void, 12usize as *mut c_void,
                                 13usize as *mut c_void]);

        let mut queued_count = 3;
        let mut recv = [ptr::null_mut(); MAX_FDS];
        let mut recv_count = 15;
        recv[..15].fill(1usize as *mut c_void);
        assert_eq!(unsafe {
            a20_linux_scm_queue_take(queued.as_mut_ptr(), &mut queued_count,
                                     recv.as_mut_ptr(), &mut recv_count, 16)
        }, 0);
        assert_eq!(recv_count, 16);
        assert_eq!(recv[15], 11usize as *mut c_void);
        assert_eq!(queued_count, 2);
        assert_eq!(queued[0], 12usize as *mut c_void);
        assert_eq!(queued[1], 13usize as *mut c_void);

        let mut dropped: Vec<i32> = Vec::new();
        unsafe extern "C" fn drop_file(ctx: *mut c_void, file: *mut c_void) {
            (*(ctx as *mut Vec<i32>)).push(file as usize as i32);
        }
        unsafe {
            a20_linux_scm_queue_drop(queued.as_mut_ptr(), &mut queued_count,
                                     &mut dropped as *mut _ as *mut c_void,
                                     Some(drop_file));
        }
        assert_eq!(dropped, [12, 13]);
        assert_eq!(queued_count, 0);
        assert_eq!(queued[0], ptr::null_mut());
    }
}
