#![no_std]

//! POSIX message-queue send/receive transactions on the existing C layout.
//!
//! Rust owns priority-queue mutation, message lifetime, queue counts,
//! notification transitions and the blocking recheck/wake protocol. C retains
//! descriptor lookup, the global IRQ-save lock, allocation, copying and one-shot
//! park primitives.

use core::ffi::c_void;
use core::mem::size_of;

const MQ_NAME_MAX: usize = 256;
const MQ_MAX_QUEUES: usize = 64;
const MQ_O_NONBLOCK: i64 = 0x800;
const O_CREAT: i32 = 0x40;
const O_EXCL: i32 = 0x80;
const DEFAULT_MAXMSG: i64 = 10;
const DEFAULT_MSGSIZE: i64 = 8192;
const MAX_MSGSIZE: i64 = 8192;
const MQ_PRIO_MAX: u32 = 32;

const EINTR: i32 = 4;
const EBADF: i32 = 9;
const EAGAIN: i32 = 11;
const ENOMEM: i32 = 12;
const EINVAL: i32 = 22;
const EMSGSIZE: i32 = 90;
const ETIMEDOUT: i32 = 110;
const EEXIST: i32 = 17;
const ENOENT: i32 = 2;
const ENOSPC: i32 = 28;
const EMFILE: i32 = 24;
const EBUSY: i32 = 16;

#[repr(C)]
pub struct MqMessage {
    next: *mut MqMessage,
    prio: u32,
    size: usize,
}

const _: () = {
    assert!(size_of::<MqMessage>() == if usize::BITS == 64 { 24 } else { 12 });
    assert!(core::mem::offset_of!(MqMessage, prio) == if usize::BITS == 64 { 8 } else { 4 });
    assert!(core::mem::offset_of!(MqMessage, size) == if usize::BITS == 64 { 16 } else { 8 });
};

#[repr(C)]
#[derive(Copy, Clone)]
pub struct MqQueue {
    used: i32,
    namespace_ino: u64,
    name: [u8; MQ_NAME_MAX],
    mq_flags: i64,
    mq_maxmsg: i64,
    mq_msgsize: i64,
    mq_curmsgs: i64,
    head: *mut MqMessage,
    tail: *mut MqMessage,
    unlinked: i32,
    refcount: i32,
    notify_sig: u64,
    notify_pid: i32,
    notify_pending: i32,
    _mechanisms: [usize; if usize::BITS == 64 { 34 } else { 50 }],
}

const _: () = {
    assert!(core::mem::offset_of!(MqQueue, mq_flags) == 272);
    assert!(core::mem::offset_of!(MqQueue, head) == 304);
    assert!(core::mem::offset_of!(MqQueue, tail) == if usize::BITS == 64 { 312 } else { 308 });
    assert!(core::mem::offset_of!(MqQueue, unlinked) == if usize::BITS == 64 { 320 } else { 312 });
    assert!(core::mem::offset_of!(MqQueue, notify_sig) == if usize::BITS == 64 { 328 } else { 320 });
    assert!(core::mem::offset_of!(MqQueue, notify_pid) == if usize::BITS == 64 { 336 } else { 328 });
    assert!(core::mem::offset_of!(MqQueue, notify_pending) == if usize::BITS == 64 { 340 } else { 332 });
    assert!(core::mem::size_of::<MqQueue>() == if usize::BITS == 64 { 616 } else { 536 });
};

#[repr(C)]
#[derive(Copy, Clone)]
pub struct MqAttr {
    mq_flags: i64,
    mq_maxmsg: i64,
    mq_msgsize: i64,
    mq_curmsgs: i64,
}

unsafe extern "C" {
    fn a20_posix_mq_desc_get(context: *mut c_void, mqd: i32) -> *mut MqQueue;
    fn a20_posix_mq_desc_snapshot(context: *mut c_void, mqd: i32, nonblock: *mut i32) -> *mut MqQueue;
    fn a20_posix_mq_desc_install(context: *mut c_void, queue: *mut MqQueue, nonblock: i32) -> i32;
    fn a20_posix_mq_desc_take(context: *mut c_void, mqd: i32) -> *mut MqQueue;
    fn a20_posix_mq_desc_set_nonblock(context: *mut c_void, mqd: i32, queue: *mut MqQueue, value: i32) -> i32;
    fn a20_posix_mq_queue_reset(context: *mut c_void, queue: *mut MqQueue);
    fn a20_posix_mq_namespace(context: *mut c_void) -> u64;
    fn a20_posix_mq_pid(context: *mut c_void) -> i32;
    fn a20_posix_mq_desc_nonblock(context: *mut c_void, mqd: i32) -> i32;
    fn a20_posix_mq_txn_lock(context: *mut c_void) -> u64;
    fn a20_posix_mq_txn_unlock(context: *mut c_void, flags: u64);
    fn a20_posix_mq_txn_now(context: *mut c_void) -> u64;
    fn a20_posix_mq_message_alloc(
        context: *mut c_void,
        msg: *const c_void,
        msg_len: usize,
        prio: u32,
    ) -> *mut MqMessage;
    fn a20_posix_mq_message_free(context: *mut c_void, message: *mut MqMessage);
    fn a20_posix_mq_message_copy_out(
        context: *mut c_void,
        message: *mut MqMessage,
        msg: *mut c_void,
        msg_len: usize,
        prio: *mut u32,
    );
    fn a20_posix_mq_wait_prepare(context: *mut c_void, deadline: u64) -> *mut c_void;
    fn a20_posix_mq_wait_link(
        context: *mut c_void,
        waiter: *mut c_void,
        queue: *mut MqQueue,
        send: bool,
    ) -> bool;
    fn a20_posix_mq_wait_finish(waiter: *mut c_void, linked: bool, commit: bool) -> bool;
    fn a20_posix_mq_wake(context: *mut c_void, queue: *mut MqQueue, send: bool);
    fn a20_posix_mq_signal(context: *mut c_void, pid: i32, signo: i32);
}

fn name_matches(queue_name: &[u8; MQ_NAME_MAX], name: *const u8) -> bool {
    if name.is_null() { return false; }
    let mut index = 0;
    while index < MQ_NAME_MAX {
        let byte = unsafe { *name.add(index) };
        if queue_name[index] != byte { return false; }
        if byte == 0 { return true; }
        index += 1;
    }
    false
}

unsafe fn mq_reclaim(context: *mut c_void, queue: *mut MqQueue) {
    let mut message = unsafe { (*queue).head };
    while !message.is_null() {
        let next = unsafe { (*message).next };
        unsafe { a20_posix_mq_message_free(context, message) };
        message = next;
    }
    unsafe { a20_posix_mq_queue_reset(context, queue) };
}

#[no_mangle]
pub unsafe extern "C" fn a20_posix_mq_open_txn(
    queues: *mut MqQueue,
    count: usize,
    context: *mut c_void,
    mut name: *const u8,
    oflag: i32,
    attr: *const MqAttr,
) -> i32 {
    if queues.is_null() || count != MQ_MAX_QUEUES || name.is_null() { return -EINVAL; }
    if unsafe { *name } == b'/' { name = unsafe { name.add(1) }; }
    if unsafe { *name } == 0 { return -EINVAL; }
    let flags = unsafe { a20_posix_mq_txn_lock(context) };
    let namespace = unsafe { a20_posix_mq_namespace(context) };
    let mut queue = core::ptr::null_mut();
    for index in 0..count {
        let candidate = unsafe { queues.add(index) };
        if unsafe { (*candidate).used != 0 && (*candidate).unlinked == 0 && (*candidate).namespace_ino == namespace && name_matches(&(*candidate).name, name) } {
            queue = candidate;
            break;
        }
    }
    if !queue.is_null() && oflag & O_CREAT != 0 && oflag & O_EXCL != 0 {
        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        return -EEXIST;
    }
    let mut created = false;
    if queue.is_null() {
        if oflag & O_CREAT == 0 {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            return -ENOENT;
        }
        queue = (0..count).map(|index| unsafe { queues.add(index) }).find(|candidate| unsafe { (**candidate).used == 0 }).unwrap_or(core::ptr::null_mut());
        if queue.is_null() {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            return -ENOSPC;
        }
        unsafe { a20_posix_mq_queue_reset(context, queue) };
        unsafe {
            (*queue).used = 1;
            (*queue).namespace_ino = namespace;
            let mut index = 0;
            while index + 1 < MQ_NAME_MAX {
                let byte = *name.add(index);
                (*queue).name[index] = byte;
                if byte == 0 { break; }
                index += 1;
            }
            (*queue).name[MQ_NAME_MAX - 1] = 0;
            let attr = if attr.is_null() { None } else { Some(core::ptr::read_unaligned(attr)) };
            (*queue).mq_maxmsg = attr.map(|a| a.mq_maxmsg).filter(|v| *v > 0).unwrap_or(DEFAULT_MAXMSG);
            (*queue).mq_msgsize = attr.map(|a| a.mq_msgsize).filter(|v| *v > 0 && *v <= MAX_MSGSIZE).unwrap_or(DEFAULT_MSGSIZE);
            (*queue).mq_curmsgs = 0;
            (*queue).head = core::ptr::null_mut();
            (*queue).tail = core::ptr::null_mut();
        }
        created = true;
    }
    let mqd = unsafe { a20_posix_mq_desc_install(context, queue, ((oflag & MQ_O_NONBLOCK as i32) != 0) as i32) };
    if mqd < 0 {
        if created { unsafe { a20_posix_mq_queue_reset(context, queue) }; }
        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        return if mqd == -EMFILE { -EMFILE } else { mqd };
    }
    unsafe { (*queue).refcount += 1 };
    unsafe { a20_posix_mq_txn_unlock(context, flags) };
    mqd
}

#[no_mangle]
pub unsafe extern "C" fn a20_posix_mq_unlink_txn(
    queues: *mut MqQueue,
    count: usize,
    context: *mut c_void,
    mut name: *const u8,
) -> i32 {
    if queues.is_null() || count != MQ_MAX_QUEUES || name.is_null() { return -EINVAL; }
    if unsafe { *name } == b'/' { name = unsafe { name.add(1) }; }
    let flags = unsafe { a20_posix_mq_txn_lock(context) };
    let namespace = unsafe { a20_posix_mq_namespace(context) };
    let mut queue = core::ptr::null_mut();
    for index in 0..count {
        let candidate = unsafe { queues.add(index) };
        if unsafe { (*candidate).used != 0 && (*candidate).unlinked == 0 && (*candidate).namespace_ino == namespace && name_matches(&(*candidate).name, name) } {
            queue = candidate;
            break;
        }
    }
    if queue.is_null() { unsafe { a20_posix_mq_txn_unlock(context, flags) }; return -ENOENT; }
    unsafe {
        (*queue).unlinked = 1;
        (*queue).name = [0; MQ_NAME_MAX];
        a20_posix_mq_wake(context, queue, true);
        a20_posix_mq_wake(context, queue, false);
        if (*queue).refcount <= 0 { mq_reclaim(context, queue); }
    }
    unsafe { a20_posix_mq_txn_unlock(context, flags) };
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_posix_mq_close_txn(context: *mut c_void, mqd: i32) -> i32 {
    let flags = unsafe { a20_posix_mq_txn_lock(context) };
    let queue = unsafe { a20_posix_mq_desc_take(context, mqd) };
    if queue.is_null() { unsafe { a20_posix_mq_txn_unlock(context, flags) }; return -EBADF; }
    unsafe {
        if (*queue).used != 0 {
            (*queue).refcount -= 1;
            if (*queue).refcount <= 0 && (*queue).unlinked != 0 { mq_reclaim(context, queue); }
        }
    }
    unsafe { a20_posix_mq_txn_unlock(context, flags) };
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_posix_mq_getsetattr_txn(
    context: *mut c_void,
    mqd: i32,
    newattr: *const MqAttr,
    oldattr: *mut MqAttr,
) -> i32 {
    let flags = unsafe { a20_posix_mq_txn_lock(context) };
    let mut nonblock = 0;
    let queue = unsafe { a20_posix_mq_desc_snapshot(context, mqd, &mut nonblock) };
    if queue.is_null() || unsafe { (*queue).used == 0 } { unsafe { a20_posix_mq_txn_unlock(context, flags) }; return -EBADF; }
    let previous = unsafe { MqAttr { mq_flags: if nonblock != 0 { MQ_O_NONBLOCK } else { 0 }, mq_maxmsg: (*queue).mq_maxmsg, mq_msgsize: (*queue).mq_msgsize, mq_curmsgs: (*queue).mq_curmsgs } };
    if !oldattr.is_null() { unsafe { *oldattr = previous }; }
    if !newattr.is_null() {
        let requested = unsafe { core::ptr::read_unaligned(newattr) };
        if unsafe { a20_posix_mq_desc_set_nonblock(context, mqd, queue, ((requested.mq_flags & MQ_O_NONBLOCK) != 0) as i32) } < 0 {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            return -EBADF;
        }
    }
    unsafe { a20_posix_mq_txn_unlock(context, flags) };
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_posix_mq_notify_txn(
    context: *mut c_void,
    mqd: i32,
    sigevent: *const u8,
) -> i32 {
    let flags = unsafe { a20_posix_mq_txn_lock(context) };
    let queue = unsafe { a20_posix_mq_desc_snapshot(context, mqd, core::ptr::null_mut()) };
    if queue.is_null() || unsafe { (*queue).used == 0 } {
        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        return -EBADF;
    }
    if sigevent.is_null() {
        unsafe { (*queue).notify_pid = 0; (*queue).notify_sig = 0; }
        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        return 0;
    }
    let signo = unsafe { core::ptr::read_unaligned(sigevent.add(8).cast::<i32>()) };
    if signo <= 0 || signo >= 64 {
        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        return -EINVAL;
    }
    if unsafe { (*queue).notify_pid != 0 } {
        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        return -EBUSY;
    }
    unsafe {
        (*queue).notify_sig = signo as u64;
        (*queue).notify_pid = a20_posix_mq_pid(context);
        (*queue).notify_pending = 0;
        a20_posix_mq_txn_unlock(context, flags);
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_posix_mq_namespace_destroy_txn(
    queues: *mut MqQueue,
    count: usize,
    context: *mut c_void,
    ino: u64,
) {
    if queues.is_null() || count != MQ_MAX_QUEUES { return; }
    let flags = unsafe { a20_posix_mq_txn_lock(context) };
    for index in 0..count {
        let queue = unsafe { queues.add(index) };
        if unsafe { (*queue).used == 0 || (*queue).namespace_ino != ino } { continue; }
        unsafe {
            (*queue).unlinked = 1;
            (*queue).name = [0; MQ_NAME_MAX];
            a20_posix_mq_wake(context, queue, true);
            a20_posix_mq_wake(context, queue, false);
            if (*queue).refcount <= 0 { mq_reclaim(context, queue); }
        }
    }
    unsafe { a20_posix_mq_txn_unlock(context, flags) };
}

unsafe fn mq_message_free(context: *mut c_void, message: *mut MqMessage) {
    unsafe { a20_posix_mq_message_free(context, message) };
}

#[no_mangle]
pub unsafe extern "C" fn a20_posix_mq_send_txn(
    context: *mut c_void,
    mqd: i32,
    msg: *const c_void,
    msg_len: usize,
    prio: u32,
    deadline: u64,
) -> i32 {
    if prio >= MQ_PRIO_MAX {
        return -EINVAL;
    }
    let queue = unsafe { a20_posix_mq_desc_get(context, mqd) };
    if queue.is_null() {
        return -EBADF;
    }
    let node = unsafe { a20_posix_mq_message_alloc(context, msg, msg_len, prio) };
    if node.is_null() {
        return -ENOMEM;
    }

    let (notify_pid, notify_sig) = loop {
        let flags = unsafe { a20_posix_mq_txn_lock(context) };
        let current = unsafe { &mut *queue };
        if current.used == 0 {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            unsafe { mq_message_free(context, node) };
            return -EBADF;
        }
        if msg_len > current.mq_msgsize as usize {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            unsafe { mq_message_free(context, node) };
            return -EMSGSIZE;
        }
        if current.mq_curmsgs < current.mq_maxmsg {
            let was_empty = current.mq_curmsgs == 0;
            let mut link = &mut current.head;
            while unsafe { !(*link).is_null() && (**link).prio >= (*node).prio } {
                link = unsafe { &mut (**link).next };
            }
            unsafe {
                (*node).next = *link;
                *link = node;
                if (*node).next.is_null() {
                    current.tail = node;
                }
            }
            current.mq_curmsgs += 1;
            let notify_pid = if was_empty && current.notify_pending == 0 {
                current.notify_pid
            } else {
                0
            };
            let notify_sig = current.notify_sig;
            if notify_pid != 0 {
                current.notify_pending = 1;
                current.notify_pid = 0;
                current.notify_sig = 0;
            }
            unsafe { a20_posix_mq_wake(context, queue, false) };
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            break (notify_pid, notify_sig);
        }
        if unsafe { a20_posix_mq_desc_nonblock(context, mqd) } != 0 {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            unsafe { mq_message_free(context, node) };
            return -EAGAIN;
        }
        if deadline != 0 && unsafe { a20_posix_mq_txn_now(context) } >= deadline {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            unsafe { mq_message_free(context, node) };
            return -ETIMEDOUT;
        }

        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        let waiter = unsafe { a20_posix_mq_wait_prepare(context, deadline) };
        if waiter.is_null() {
            unsafe { mq_message_free(context, node) };
            return -EAGAIN;
        }
        let flags = unsafe { a20_posix_mq_txn_lock(context) };
        let current = unsafe { &mut *queue };
        if current.used == 0 || current.mq_curmsgs < current.mq_maxmsg {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            let _ = unsafe { a20_posix_mq_wait_finish(waiter, false, false) };
            continue;
        }
        let linked = unsafe { a20_posix_mq_wait_link(context, waiter, queue, true) };
        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        let interrupted = unsafe { a20_posix_mq_wait_finish(waiter, linked, true) };
        if interrupted {
            unsafe { mq_message_free(context, node) };
            return -EINTR;
        }
        if deadline != 0 && unsafe { a20_posix_mq_txn_now(context) } >= deadline {
            unsafe { mq_message_free(context, node) };
            return -ETIMEDOUT;
        }
    };

    if notify_pid != 0 && notify_sig != 0 {
        unsafe { a20_posix_mq_signal(context, notify_pid, notify_sig as i32) };
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_posix_mq_receive_txn(
    context: *mut c_void,
    mqd: i32,
    msg: *mut c_void,
    msg_len: usize,
    prio: *mut u32,
    deadline: u64,
) -> i64 {
    let queue = unsafe { a20_posix_mq_desc_get(context, mqd) };
    if queue.is_null() {
        return -EBADF as i64;
    }
    loop {
        let flags = unsafe { a20_posix_mq_txn_lock(context) };
        let current = unsafe { &mut *queue };
        if current.used == 0 {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            return -EBADF as i64;
        }
        let node = current.head;
        if !node.is_null() {
            let size = unsafe { (*node).size };
            if size > msg_len {
                unsafe { a20_posix_mq_txn_unlock(context, flags) };
                return -EMSGSIZE as i64;
            }
            current.head = unsafe { (*node).next };
            if current.head.is_null() {
                current.tail = core::ptr::null_mut();
            }
            current.mq_curmsgs -= 1;
            unsafe { a20_posix_mq_wake(context, queue, true) };
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            unsafe { a20_posix_mq_message_copy_out(context, node, msg, msg_len, prio) };
            unsafe { mq_message_free(context, node) };
            return size as i64;
        }
        if unsafe { a20_posix_mq_desc_nonblock(context, mqd) } != 0 {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            return -EAGAIN as i64;
        }
        if deadline != 0 && unsafe { a20_posix_mq_txn_now(context) } >= deadline {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            return -ETIMEDOUT as i64;
        }

        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        let waiter = unsafe { a20_posix_mq_wait_prepare(context, deadline) };
        if waiter.is_null() {
            return -EAGAIN as i64;
        }
        let flags = unsafe { a20_posix_mq_txn_lock(context) };
        let current = unsafe { &mut *queue };
        if current.used == 0 || !current.head.is_null() {
            unsafe { a20_posix_mq_txn_unlock(context, flags) };
            let _ = unsafe { a20_posix_mq_wait_finish(waiter, false, false) };
            continue;
        }
        let linked = unsafe { a20_posix_mq_wait_link(context, waiter, queue, false) };
        unsafe { a20_posix_mq_txn_unlock(context, flags) };
        let interrupted = unsafe { a20_posix_mq_wait_finish(waiter, linked, true) };
        if interrupted {
            return -EINTR as i64;
        }
        if deadline != 0 && unsafe { a20_posix_mq_txn_now(context) } >= deadline {
            return -ETIMEDOUT as i64;
        }
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::boxed::Box;

    #[repr(C)]
    struct TestMessage {
        next: *mut MqMessage,
        prio: u32,
        size: usize,
        data: [u8; 64],
    }

    enum FinishAction {
        MakeSpace,
        QueueMessage(u32),
    }

    struct MockMq {
        queue: *mut MqQueue,
        locked: bool,
        now: u64,
        descriptors: [*mut MqQueue; 4],
        nonblock: [i32; 4],
        desc_calls: usize,
        prepares: usize,
        links: usize,
        finishes: usize,
        wakes: [usize; 2],
        signals: usize,
        live_messages: usize,
        fail_alloc: bool,
        fail_prepare: bool,
        finish_interrupted: bool,
        finish_action: Option<FinishAction>,
    }

    impl MockMq {
        fn new(queue: &mut MqQueue) -> Self {
            Self {
                queue,
                locked: false,
                now: 0,
                descriptors: [queue, core::ptr::null_mut(), core::ptr::null_mut(), core::ptr::null_mut()],
                nonblock: [0; 4],
                desc_calls: 0,
                prepares: 0,
                links: 0,
                finishes: 0,
                wakes: [0; 2],
                signals: 0,
                live_messages: 0,
                fail_alloc: false,
                fail_prepare: false,
                finish_interrupted: false,
                finish_action: None,
            }
        }
    }

    struct MockWaiter {
        context: *mut c_void,
    }

    fn empty_queue() -> MqQueue {
        MqQueue {
            used: 1,
            namespace_ino: 7,
            name: [0; MQ_NAME_MAX],
            mq_flags: 0,
            mq_maxmsg: 4,
            mq_msgsize: 64,
            mq_curmsgs: 0,
            head: core::ptr::null_mut(),
            tail: core::ptr::null_mut(),
            unlinked: 0,
            refcount: 1,
            notify_sig: 0,
            notify_pid: 0,
            notify_pending: 0,
            _mechanisms: [0; if usize::BITS == 64 { 34 } else { 45 }],
        }
    }

    fn alloc_test_message(context: *mut c_void, msg: *const c_void, len: usize, prio: u32) -> *mut MqMessage {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        if mock.fail_alloc || len > 64 {
            return core::ptr::null_mut();
        }
        let mut message = Box::new(TestMessage {
            next: core::ptr::null_mut(),
            prio,
            size: len,
            data: [0; 64],
        });
        if len > 0 && !msg.is_null() {
            unsafe { core::ptr::copy_nonoverlapping(msg as *const u8, message.data.as_mut_ptr(), len) };
        }
        mock.live_messages += 1;
        Box::into_raw(message) as *mut MqMessage
    }

    fn free_test_message(context: *mut c_void, message: *mut MqMessage) {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        assert!(!message.is_null());
        unsafe { drop(Box::from_raw(message as *mut TestMessage)) };
        mock.live_messages -= 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_desc_get(context: *mut c_void, _mqd: i32) -> *mut MqQueue {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        mock.desc_calls += 1;
        if _mqd < 0 || _mqd as usize >= mock.descriptors.len() { core::ptr::null_mut() } else { mock.descriptors[_mqd as usize] }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_desc_nonblock(context: *mut c_void, _mqd: i32) -> i32 {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        if _mqd < 0 || _mqd as usize >= mock.nonblock.len() { 0 } else { mock.nonblock[_mqd as usize] }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_desc_snapshot(context: *mut c_void, mqd: i32, nonblock: *mut i32) -> *mut MqQueue {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        if mqd < 0 || mqd as usize >= mock.descriptors.len() { return core::ptr::null_mut(); }
        if !nonblock.is_null() { unsafe { *nonblock = mock.nonblock[mqd as usize] }; }
        mock.descriptors[mqd as usize]
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_desc_install(context: *mut c_void, queue: *mut MqQueue, nonblock: i32) -> i32 {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        if let Some(index) = mock.descriptors.iter().position(|entry| entry.is_null()) {
            mock.descriptors[index] = queue;
            mock.nonblock[index] = nonblock;
            index as i32
        } else { -EMFILE }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_desc_take(context: *mut c_void, mqd: i32) -> *mut MqQueue {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        if mqd < 0 || mqd as usize >= mock.descriptors.len() { return core::ptr::null_mut(); }
        let queue = mock.descriptors[mqd as usize];
        mock.descriptors[mqd as usize] = core::ptr::null_mut();
        mock.nonblock[mqd as usize] = 0;
        queue
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_desc_set_nonblock(context: *mut c_void, mqd: i32, queue: *mut MqQueue, value: i32) -> i32 {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        if mqd < 0 || mqd as usize >= mock.descriptors.len() || mock.descriptors[mqd as usize] != queue { return -EBADF; }
        mock.nonblock[mqd as usize] = value;
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_queue_reset(_context: *mut c_void, queue: *mut MqQueue) {
        unsafe { *queue = empty_queue(); (*queue).used = 0; (*queue).namespace_ino = 0; (*queue).refcount = 0; }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_namespace(_context: *mut c_void) -> u64 { 7 }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_pid(_context: *mut c_void) -> i32 { 55 }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_txn_lock(context: *mut c_void) -> u64 {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        assert!(!mock.locked);
        mock.locked = true;
        0x5a
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_txn_unlock(context: *mut c_void, flags: u64) {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        assert!(mock.locked);
        assert_eq!(flags, 0x5a);
        mock.locked = false;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_txn_now(context: *mut c_void) -> u64 {
        unsafe { (*(context as *mut MockMq)).now }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_message_alloc(
        context: *mut c_void,
        msg: *const c_void,
        msg_len: usize,
        prio: u32,
    ) -> *mut MqMessage {
        alloc_test_message(context, msg, msg_len, prio)
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_message_free(context: *mut c_void, message: *mut MqMessage) {
        free_test_message(context, message);
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_message_copy_out(
        context: *mut c_void,
        message: *mut MqMessage,
        msg: *mut c_void,
        msg_len: usize,
        prio: *mut u32,
    ) {
        let _ = context;
        let message = unsafe { &*(message as *mut TestMessage) };
        if message.size > 0 {
            unsafe { core::ptr::copy_nonoverlapping(message.data.as_ptr(), msg as *mut u8, message.size.min(msg_len)) };
        }
        if !prio.is_null() {
            unsafe { *prio = message.prio };
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_wait_prepare(context: *mut c_void, _deadline: u64) -> *mut c_void {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        assert!(!mock.locked);
        mock.prepares += 1;
        if mock.fail_prepare {
            return core::ptr::null_mut();
        }
        Box::into_raw(Box::new(MockWaiter { context })).cast()
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_wait_link(
        context: *mut c_void,
        _waiter: *mut c_void,
        _queue: *mut MqQueue,
        _send: bool,
    ) -> bool {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        assert!(mock.locked);
        mock.links += 1;
        true
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_wait_finish(
        waiter: *mut c_void,
        linked: bool,
        commit: bool,
    ) -> bool {
        let waiter = unsafe { Box::from_raw(waiter as *mut MockWaiter) };
        let mock = unsafe { &mut *(waiter.context as *mut MockMq) };
        assert!(!mock.locked);
        mock.finishes += 1;
        if linked && commit {
            match mock.finish_action.take() {
                Some(FinishAction::MakeSpace) => {
                    let queue = unsafe { &mut *mock.queue };
                    let old = queue.head;
                    if !old.is_null() {
                        queue.head = unsafe { (*old).next };
                        queue.mq_curmsgs -= 1;
                        free_test_message(waiter.context, old);
                    }
                    if queue.head.is_null() {
                        queue.tail = core::ptr::null_mut();
                    }
                }
                Some(FinishAction::QueueMessage(prio)) => {
                    let data = [7u8];
                    let node = alloc_test_message(waiter.context, data.as_ptr().cast(), 1, prio);
                    let queue = unsafe { &mut *mock.queue };
                    queue.head = node;
                    queue.tail = node;
                    queue.mq_curmsgs += 1;
                }
                None => {}
            }
        }
        mock.finish_interrupted
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_wake(context: *mut c_void, _queue: *mut MqQueue, send: bool) {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        assert!(mock.locked);
        mock.wakes[usize::from(send)] += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_posix_mq_signal(context: *mut c_void, _pid: i32, _signo: i32) {
        let mock = unsafe { &mut *(context as *mut MockMq) };
        assert!(!mock.locked);
        mock.signals += 1;
    }

    #[test]
    fn mq_priority_fifo_receive_and_small_buffer_rollback() {
        let mut queue = empty_queue();
        queue.notify_pid = 44;
        queue.notify_sig = 9;
        let mut mock = MockMq::new(&mut queue);
        let context = &mut mock as *mut MockMq as *mut c_void;
        for (prio, byte) in [(2, b'b'), (5, b'a'), (2, b'c')] {
            let data = [byte];
            unsafe {
                assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, prio, 0), 0);
            }
        }
        assert_eq!(queue.mq_curmsgs, 3);
        assert_eq!(queue.notify_pending, 1);
        assert_eq!(mock.signals, 1);
        assert_eq!((queue.notify_pid, queue.notify_sig), (0, 0));
        assert_eq!(mock.live_messages, 3);

        let mut out = [0u8; 4];
        let mut prio = 0;
        unsafe {
            assert_eq!(a20_posix_mq_receive_txn(context, 0, out.as_mut_ptr().cast(), 0, &mut prio, 0), -EMSGSIZE as i64);
        }
        assert_eq!(queue.mq_curmsgs, 3);
        for (want_prio, want_byte) in [(5, b'a'), (2, b'b'), (2, b'c')] {
            unsafe {
                assert_eq!(a20_posix_mq_receive_txn(context, 0, out.as_mut_ptr().cast(), out.len(), &mut prio, 0), 1);
            }
            assert_eq!((prio, out[0]), (want_prio, want_byte));
        }
        assert_eq!((queue.mq_curmsgs, mock.live_messages), (0, 0));
        assert!(!mock.locked);
    }

    #[test]
    fn mq_blocking_send_and_receive_recheck_and_release_messages() {
        let mut queue = empty_queue();
        queue.mq_maxmsg = 1;
        let mut mock = MockMq::new(&mut queue);
        let context = &mut mock as *mut MockMq as *mut c_void;
        let first = [1u8];
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, first.as_ptr().cast(), 1, 1, 0), 0);
        }
        mock.finish_action = Some(FinishAction::MakeSpace);
        let second = [2u8];
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, second.as_ptr().cast(), 1, 2, 0), 0);
        }
        assert_eq!((mock.prepares, mock.links, mock.live_messages), (1, 1, 1));

        let mut out = [0u8; 1];
        let mut prio = 0;
        unsafe {
            assert_eq!(a20_posix_mq_receive_txn(context, 0, out.as_mut_ptr().cast(), 1, &mut prio, 0), 1);
        }
        assert_eq!((out[0], prio, mock.live_messages), (2, 2, 0));

        mock.finish_action = Some(FinishAction::QueueMessage(7));
        unsafe {
            assert_eq!(a20_posix_mq_receive_txn(context, 0, out.as_mut_ptr().cast(), 1, &mut prio, 0), 1);
        }
        assert_eq!((out[0], prio, mock.live_messages), (7, 7, 0));
        assert!(!mock.locked);
    }

    #[test]
    fn mq_interrupted_and_prepare_failure_roll_back() {
        let mut queue = empty_queue();
        queue.mq_maxmsg = 1;
        let mut mock = MockMq::new(&mut queue);
        let context = &mut mock as *mut MockMq as *mut c_void;
        let mut data = [1u8];
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 0), 0);
        }
        mock.fail_prepare = true;
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 0), -EAGAIN);
        }
        assert_eq!((mock.live_messages, mock.links), (1, 0));

        mock.fail_prepare = false;
        mock.finish_interrupted = true;
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 0), -EINTR);
        }
        assert_eq!(mock.live_messages, 1);
        unsafe {
            assert_eq!(a20_posix_mq_receive_txn(context, 0, data.as_mut_ptr().cast(), 1, core::ptr::null_mut(), 0), 1);
        }
        assert_eq!(mock.live_messages, 0);

        mock.fail_prepare = true;
        unsafe {
            assert_eq!(a20_posix_mq_receive_txn(context, 0, data.as_mut_ptr().cast(), 1, core::ptr::null_mut(), 0), -EAGAIN as i64);
        }
        mock.fail_prepare = false;
        unsafe {
            assert_eq!(a20_posix_mq_receive_txn(context, 0, data.as_mut_ptr().cast(), 1, core::ptr::null_mut(), 0), -EINTR as i64);
        }
        assert!(!mock.locked);
    }

    #[test]
    fn mq_invalid_full_empty_deadline_close_and_alloc_failures() {
        let mut queue = empty_queue();
        queue.mq_maxmsg = 1;
        queue.mq_flags = MQ_O_NONBLOCK;
        let mut mock = MockMq::new(&mut queue);
        mock.nonblock[0] = 1;
        let context = &mut mock as *mut MockMq as *mut c_void;
        let mut data = [1u8];
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 32, 0), -EINVAL);
            assert_eq!(mock.desc_calls, 0);
        }
        mock.fail_alloc = true;
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 0), -ENOMEM);
        }
        mock.fail_alloc = false;
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 0), 0);
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 0), -EAGAIN);
            assert_eq!(a20_posix_mq_receive_txn(context, 0, data.as_mut_ptr().cast(), 0, core::ptr::null_mut(), 0), -EMSGSIZE as i64);
        }
        unsafe {
            assert_eq!(a20_posix_mq_receive_txn(context, 0, data.as_mut_ptr().cast(), 1, core::ptr::null_mut(), 0), 1);
            assert_eq!(a20_posix_mq_receive_txn(context, 0, data.as_mut_ptr().cast(), 1, core::ptr::null_mut(), 0), -EAGAIN as i64);
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 0), 0);
        }
        queue.mq_flags = 0;
        assert_eq!(queue.mq_flags, 0);
        mock.nonblock[0] = 0;
        mock.now = 8;
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 7), -ETIMEDOUT);
        }
        queue.used = 0;
        assert_eq!(queue.used, 0);
        unsafe {
            assert_eq!(a20_posix_mq_send_txn(context, 0, data.as_ptr().cast(), 1, 1, 0), -EBADF);
        }
        assert_eq!(mock.live_messages, 1);
        assert!(!mock.locked);
    }

    #[test]
    fn mq_open_unlink_descriptor_flags_and_close_lifecycle() {
        let mut queues = [empty_queue(); MQ_MAX_QUEUES];
        for queue in &mut queues { queue.used = 0; queue.namespace_ino = 0; }
        let mut mock = MockMq::new(&mut queues[0]);
        mock.descriptors = [core::ptr::null_mut(); 4];
        let context = &mut mock as *mut MockMq as *mut c_void;
        let name = b"lifecycle\0";
        let attr = MqAttr { mq_flags: 0, mq_maxmsg: 2, mq_msgsize: 32, mq_curmsgs: 0 };
        let mqd0 = unsafe { a20_posix_mq_open_txn(queues.as_mut_ptr(), MQ_MAX_QUEUES, context, name.as_ptr(), O_CREAT | MQ_O_NONBLOCK as i32, &attr) };
        let mqd1 = unsafe { a20_posix_mq_open_txn(queues.as_mut_ptr(), MQ_MAX_QUEUES, context, name.as_ptr(), 0, core::ptr::null()) };
        assert_eq!((mqd0, mqd1), (0, 1));
        assert_eq!((queues[0].refcount, queues[0].mq_maxmsg, queues[0].mq_msgsize), (2, 2, 32));
        assert_eq!((mock.nonblock[0], mock.nonblock[1]), (1, 0));
        let mut event = [0u8; 24];
        event[8..12].copy_from_slice(&10i32.to_ne_bytes());
        unsafe { assert_eq!(a20_posix_mq_notify_txn(context, mqd0, event.as_ptr()), 0) };
        assert_eq!((queues[0].notify_pid, queues[0].notify_sig), (55, 10));
        unsafe { assert_eq!(a20_posix_mq_notify_txn(context, mqd1, event.as_ptr()), -EBUSY) };
        unsafe { assert_eq!(a20_posix_mq_notify_txn(context, mqd1, core::ptr::null()), 0) };
        assert_eq!((queues[0].notify_pid, queues[0].notify_sig), (0, 0));
        let attr2 = MqAttr { mq_flags: 0, mq_maxmsg: 0, mq_msgsize: 0, mq_curmsgs: 0 };
        let mut old = MqAttr { mq_flags: -1, mq_maxmsg: 0, mq_msgsize: 0, mq_curmsgs: 0 };
        unsafe { assert_eq!(a20_posix_mq_getsetattr_txn(context, mqd1, &attr2, &mut old), 0) };
        assert_eq!(old.mq_flags, 0);
        assert_eq!((mock.nonblock[0], mock.nonblock[1]), (1, 0));
        unsafe { assert_eq!(a20_posix_mq_unlink_txn(queues.as_mut_ptr(), MQ_MAX_QUEUES, context, name.as_ptr()), 0) };
        assert_eq!(queues[0].unlinked, 1);
        unsafe { assert_eq!(a20_posix_mq_close_txn(context, mqd0), 0) };
        assert_eq!(queues[0].refcount, 1);
        unsafe { assert_eq!(a20_posix_mq_close_txn(context, mqd1), 0) };
        assert_eq!(queues[0].used, 0);
        assert!(!mock.locked);
    }
}
