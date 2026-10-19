#![no_std]

//! SysV message-queue transactions on the existing C queue layout.
//!
//! Rust owns type selection, queue accounting, blocking rechecks, message
//! claims, commit/rollback and removal-driven lifetime transitions. C retains
//! the registry storage, IRQ-save lock, allocation, usercopy and park/wake
//! primitives. The queue's used word tracks active/removal state plus claims;
//! claimed messages and linked waiters keep a removed queue alive until every
//! in-flight operation has left its wait queue and released its node.

use core::ffi::c_void;
use core::mem::size_of;

const SYSV_MSG_MAX: usize = 32;
const MSG_MAX: usize = 8192;
const IPC_NOWAIT: i32 = 0o4000;
const MSG_NOERROR: i32 = 0o10000;
const IPC_CREAT: i32 = 0o1000;
const IPC_EXCL: i32 = 0o2000;
const IPC_64_BIT: i32 = 0x100;
const IPC_RMID: i32 = 0;
const IPC_SET: i32 = 1;
const IPC_STAT: i32 = 2;
const MSG_STAT: i32 = 11;
const MSG_STAT_ANY: i32 = 13;
const MSG_QBYTES: usize = 16384;

const USED_STATE: i32 = 3;
const USED_ACTIVE: i32 = 1;
const USED_REMOVING: i32 = 2;
const CLAIM_INC: i32 = 4;
const NODE_CLAIMED: usize = 1usize << (usize::BITS - 1);

const E2BIG: i32 = 7;
const EAGAIN: i32 = 11;
const ENOMEM: i32 = 12;
const EINVAL: i32 = 22;
const EIDRM: i32 = 43;
const ENOMSG: i32 = 42;
const EINTR: i32 = 4;

#[repr(C)]
pub struct SysvMsgNode {
    next: *mut SysvMsgNode,
    msg_type: i64,
    size: usize,
}

const _: () = {
    assert!(core::mem::offset_of!(SysvMsgNode, msg_type) == 8);
    assert!(core::mem::offset_of!(SysvMsgNode, size) == 16);
    assert!(size_of::<SysvMsgNode>() == 24);
};

#[repr(C)]
#[derive(Default, Copy, Clone)]
struct SysvMsqidDs {
    perm: SysvIpcPerm,
    stime: i64,
    rtime: i64,
    ctime: i64,
    cbytes: u64,
    qnum: u64,
    qbytes: u64,
    lspid: i32,
    lrpid: i32,
    unused: [u64; 2],
}

const _: () = assert!(size_of::<SysvMsqidDs>() == if usize::BITS == 64 { 120 } else { 112 });

#[repr(C)]
#[derive(Default, Copy, Clone)]
pub struct SysvIpcPerm {
    key: i32,
    uid: u32,
    gid: u32,
    cuid: u32,
    cgid: u32,
    mode: u32,
    seq: i32,
    pad: u32,
    unused1: usize,
    unused2: usize,
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_msg_get_txn(
    queues: *mut SysvMsgQueue,
    count: usize,
    context: *mut c_void,
    key: i32,
    msgflg: i32,
) -> i32 {
    if queues.is_null() || count != SYSV_MSG_MAX {
        return -EINVAL;
    }
    let flags = unsafe { a20_sysv_msg_txn_lock(context) };
    let namespace = unsafe { a20_sysv_msg_txn_namespace(context) };
    for id in 0..count {
        let queue = unsafe { &*queues.add(id) };
        if queue_state(queue) == USED_ACTIVE && queue.namespace_ino == namespace && key != 0 && queue.key == key {
            let result = if msgflg & IPC_CREAT != 0 && msgflg & IPC_EXCL != 0 { -17 } else { id as i32 };
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            return result;
        }
    }
    if key != 0 && msgflg & IPC_CREAT == 0 {
        unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        return -2;
    }
    for id in 0..count {
        let queue = unsafe { &mut *queues.add(id) };
        if queue.used == 0 {
            unsafe { a20_sysv_msg_queue_init(queue as *mut SysvMsgQueue as *mut c_void) };
            queue.used = USED_ACTIVE;
            queue.namespace_ino = namespace;
            queue.key = key;
            queue.perm.key = key;
            queue.perm.uid = unsafe { a20_sysv_msg_txn_uid(context) };
            queue.perm.gid = unsafe { a20_sysv_msg_txn_gid(context) };
            queue.perm.cuid = queue.perm.uid;
            queue.perm.cgid = queue.perm.gid;
            queue.perm.mode = msgflg as u32 & 0o777;
            queue.ctime = unsafe { a20_sysv_msg_txn_now(context) };
            queue.qbytes = MSG_QBYTES;
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            return id as i32;
        }
    }
    unsafe { a20_sysv_msg_txn_unlock(context, flags) };
    -28
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_msg_control_txn(
    queues: *mut SysvMsgQueue,
    count: usize,
    context: *mut c_void,
    msqid: i32,
    command: i32,
    arg: *mut c_void,
) -> i32 {
    if queues.is_null() || count != SYSV_MSG_MAX || msqid < 0 || msqid as usize >= count {
        return -EINVAL;
    }
    let cmd = command & !IPC_64_BIT;
    let flags = unsafe { a20_sysv_msg_txn_lock(context) };
    let namespace = unsafe { a20_sysv_msg_txn_namespace(context) };
    let queue = unsafe { &mut *queues.add(msqid as usize) };
    if queue_state(queue) != USED_ACTIVE || queue.namespace_ino != namespace {
        unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        return -EINVAL;
    }
    match cmd {
        IPC_RMID => {
            unsafe { a20_sysv_msg_remove_locked(context, queue) };
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            0
        }
        IPC_STAT | MSG_STAT | MSG_STAT_ANY => {
            let ds = SysvMsqidDs {
                perm: unsafe { core::ptr::read(&queue.perm) },
                stime: queue.stime as i64,
                rtime: queue.rtime as i64,
                ctime: queue.ctime as i64,
                cbytes: queue.cbytes as u64,
                qnum: queue.qnum as u64,
                qbytes: queue.qbytes as u64,
                lspid: queue.lspid,
                lrpid: queue.lrpid,
                unused: [0; 2],
            };
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            if !arg.is_null() && unsafe { a20_sysv_msg_copy_to_user(context, arg, &ds as *const SysvMsqidDs as *const c_void, size_of::<SysvMsqidDs>()) } < 0 {
                return -14;
            }
            if cmd == IPC_STAT { 0 } else { msqid }
        }
        IPC_SET => {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            if arg.is_null() { return -EINVAL; }
            let mut ds = SysvMsqidDs::default();
            if unsafe { a20_sysv_msg_copy_from_user(context, &mut ds as *mut SysvMsqidDs as *mut c_void, arg, size_of::<SysvMsqidDs>()) } < 0 { return -14; }
            let flags = unsafe { a20_sysv_msg_txn_lock(context) };
            let queue = unsafe { &mut *queues.add(msqid as usize) };
            if queue_state(queue) != USED_ACTIVE || queue.namespace_ino != unsafe { a20_sysv_msg_txn_namespace(context) } {
                unsafe { a20_sysv_msg_txn_unlock(context, flags) };
                return -EINVAL;
            }
            queue.perm.uid = ds.perm.uid;
            queue.perm.gid = ds.perm.gid;
            queue.perm.mode = ds.perm.mode;
            queue.ctime = unsafe { a20_sysv_msg_txn_now(context) };
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            0
        }
        _ => { unsafe { a20_sysv_msg_txn_unlock(context, flags) }; -EINVAL }
    }
}

#[repr(C)]
pub struct SysvMsgQueue {
    used: i32,
    namespace_ino: u64,
    key: i32,
    perm: SysvIpcPerm,
    stime: u64,
    rtime: u64,
    ctime: u64,
    cbytes: usize,
    qnum: usize,
    qbytes: usize,
    lspid: i32,
    lrpid: i32,
    head: *mut SysvMsgNode,
    tail: *mut SysvMsgNode,
    _waiters: [usize; if usize::BITS == 64 { 24 } else { 36 }],
}

const _: () = {
    assert!(core::mem::offset_of!(SysvMsgQueue, namespace_ino) == 8);
    assert!(core::mem::offset_of!(SysvMsgQueue, perm) == if usize::BITS == 64 { 24 } else { 20 });
    assert!(core::mem::offset_of!(SysvMsgQueue, stime) == if usize::BITS == 64 { 72 } else { 64 });
    assert!(core::mem::offset_of!(SysvMsgQueue, cbytes) == if usize::BITS == 64 { 96 } else { 88 });
    assert!(core::mem::offset_of!(SysvMsgQueue, qnum) == if usize::BITS == 64 { 104 } else { 92 });
    assert!(core::mem::offset_of!(SysvMsgQueue, qbytes) == if usize::BITS == 64 { 112 } else { 96 });
    assert!(core::mem::offset_of!(SysvMsgQueue, lspid) == if usize::BITS == 64 { 120 } else { 100 });
    assert!(core::mem::offset_of!(SysvMsgQueue, head) == if usize::BITS == 64 { 128 } else { 108 });
    assert!(core::mem::offset_of!(SysvMsgQueue, tail) == if usize::BITS == 64 { 136 } else { 112 });
    assert!(core::mem::size_of::<SysvMsgQueue>() == if usize::BITS == 64 { 336 } else { 264 });
};

unsafe extern "C" {
    fn a20_sysv_msg_txn_lock(context: *mut c_void) -> u64;
    fn a20_sysv_msg_txn_unlock(context: *mut c_void, flags: u64);
    fn a20_sysv_msg_txn_namespace(context: *mut c_void) -> u64;
    fn a20_sysv_msg_txn_pid(context: *mut c_void) -> i32;
    fn a20_sysv_msg_txn_uid(context: *mut c_void) -> u32;
    fn a20_sysv_msg_txn_gid(context: *mut c_void) -> u32;
    fn a20_sysv_msg_txn_now(context: *mut c_void) -> u64;
    fn a20_sysv_msg_queue_init(queue: *mut c_void);
    fn a20_sysv_msg_copy_from_user(context: *mut c_void, dst: *mut c_void, src: *const c_void, len: usize) -> i32;
    fn a20_sysv_msg_copy_to_user(context: *mut c_void, dst: *mut c_void, src: *const c_void, len: usize) -> i32;
    fn a20_sysv_msg_read_type(
        context: *mut c_void,
        msgp: *const c_void,
        msg_type: *mut i64,
    ) -> i32;
    fn a20_sysv_msg_message_alloc(
        context: *mut c_void,
        msgp: *const c_void,
        msgsz: usize,
        status: *mut i32,
    ) -> *mut SysvMsgNode;
    fn a20_sysv_msg_message_free(context: *mut c_void, node: *mut SysvMsgNode);
    fn a20_sysv_msg_message_copy_out(
        context: *mut c_void,
        node: *mut SysvMsgNode,
        msgp: *mut c_void,
        len: usize,
    ) -> i32;
    fn a20_sysv_msg_wait_prepare(context: *mut c_void) -> *mut c_void;
    fn a20_sysv_msg_wait_link(
        context: *mut c_void,
        waiter: *mut c_void,
        queue: *mut SysvMsgQueue,
        send: bool,
    ) -> bool;
    fn a20_sysv_msg_wait_finish(waiter: *mut c_void, linked: bool, commit: bool) -> bool;
    fn a20_sysv_msg_wake(context: *mut c_void, queue: *mut SysvMsgQueue, send: bool, all: bool);
    fn a20_sysv_msg_queue_reset(context: *mut c_void, queue: *mut SysvMsgQueue);
}

fn node_size(node: *const SysvMsgNode) -> usize {
    unsafe { (*node).size & !NODE_CLAIMED }
}

fn queue_state(queue: *const SysvMsgQueue) -> i32 {
    unsafe { (*queue).used & USED_STATE }
}

fn queue_claims(queue: *const SysvMsgQueue) -> i32 {
    unsafe { (*queue).used >> 2 }
}

unsafe fn claim_add(queue: *mut SysvMsgQueue) {
    unsafe { (*queue).used += CLAIM_INC };
}

unsafe fn claim_drop(context: *mut c_void, queue: *mut SysvMsgQueue) {
    unsafe { (*queue).used -= CLAIM_INC };
    if queue_claims(queue) == 0 && queue_state(queue) == USED_REMOVING {
        unsafe { a20_sysv_msg_queue_reset(context, queue) };
    }
}

unsafe fn active_queue(
    queues: *mut SysvMsgQueue,
    count: usize,
    context: *mut c_void,
    msqid: i32,
) -> *mut SysvMsgQueue {
    if msqid < 0 || msqid as usize >= count {
        return core::ptr::null_mut();
    }
    let queue = unsafe { queues.add(msqid as usize) };
    let namespace = unsafe { a20_sysv_msg_txn_namespace(context) };
    if queue_state(queue) == USED_ACTIVE && unsafe { (*queue).namespace_ino == namespace } {
        queue
    } else {
        core::ptr::null_mut()
    }
}

fn matches(node: *const SysvMsgNode, msgtyp: i64) -> bool {
    if unsafe { (*node).size & NODE_CLAIMED != 0 } {
        return false;
    }
    let msg_type = unsafe { (*node).msg_type };
    if msgtyp == 0 {
        true
    } else if msgtyp > 0 {
        msg_type == msgtyp
    } else {
        msg_type <= msgtyp.saturating_neg()
    }
}

fn find_match(queue: *mut SysvMsgQueue, msgtyp: i64) -> *mut SysvMsgNode {
    let mut node = unsafe { (*queue).head };
    while !node.is_null() {
        if matches(node, msgtyp) {
            break;
        }
        node = unsafe { (*node).next };
    }
    node
}

unsafe fn unlink_node(queue: *mut SysvMsgQueue, node: *mut SysvMsgNode) -> bool {
    let mut prev: *mut SysvMsgNode = core::ptr::null_mut();
    let mut current = unsafe { (*queue).head };
    while !current.is_null() {
        if current == node {
            let next = unsafe { (*current).next };
            if prev.is_null() {
                unsafe { (*queue).head = next };
            } else {
                unsafe { (*prev).next = next };
            }
            if next.is_null() {
                unsafe { (*queue).tail = prev };
            }
            return true;
        }
        prev = current;
        current = unsafe { (*current).next };
    }
    false
}

unsafe fn cancel_wait(waiter: *mut c_void) {
    let _ = unsafe { a20_sysv_msg_wait_finish(waiter, false, false) };
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_msg_send_txn(
    queues: *mut SysvMsgQueue,
    count: usize,
    context: *mut c_void,
    msqid: i32,
    msgp: *const c_void,
    msgsz: usize,
    msgflg: i32,
) -> i32 {
    if queues.is_null() || count != SYSV_MSG_MAX || msgp.is_null() || msgsz > MSG_MAX {
        return -EINVAL;
    }
    let mut msg_type = 0i64;
    let read = unsafe { a20_sysv_msg_read_type(context, msgp, &mut msg_type) };
    if read < 0 {
        return read;
    }
    if msg_type <= 0 {
        return -EINVAL;
    }
    let mut alloc_status = -ENOMEM;
    let node = unsafe { a20_sysv_msg_message_alloc(context, msgp, msgsz, &mut alloc_status) };
    if node.is_null() {
        return alloc_status;
    }
    unsafe {
        (*node).next = core::ptr::null_mut();
        (*node).msg_type = msg_type;
        (*node).size = msgsz;
    }

    let mut after_wait = false;
    loop {
        let flags = unsafe { a20_sysv_msg_txn_lock(context) };
        let queue = unsafe { active_queue(queues, count, context, msqid) };
        if queue.is_null() {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            unsafe { a20_sysv_msg_message_free(context, node) };
            return if after_wait { -EIDRM } else { -EINVAL };
        }
        let has_space = msgsz <= unsafe { (*queue).qbytes.saturating_sub((*queue).cbytes) };
        if has_space {
            unsafe {
                if (*queue).tail.is_null() {
                    (*queue).head = node;
                    (*queue).tail = node;
                } else {
                    (*(*queue).tail).next = node;
                    (*queue).tail = node;
                }
                (*queue).cbytes += msgsz;
                (*queue).qnum += 1;
                (*queue).stime = a20_sysv_msg_txn_now(context);
                (*queue).lspid = a20_sysv_msg_txn_pid(context);
                a20_sysv_msg_wake(context, queue, false, false);
                a20_sysv_msg_txn_unlock(context, flags);
            }
            return 0;
        }
        if msgflg & IPC_NOWAIT != 0 {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            unsafe { a20_sysv_msg_message_free(context, node) };
            return -EAGAIN;
        }

        unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        let waiter = unsafe { a20_sysv_msg_wait_prepare(context) };
        if waiter.is_null() {
            unsafe { a20_sysv_msg_message_free(context, node) };
            return -EAGAIN;
        }
        let flags = unsafe { a20_sysv_msg_txn_lock(context) };
        let queue = unsafe { active_queue(queues, count, context, msqid) };
        if queue.is_null() {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            unsafe { cancel_wait(waiter) };
            unsafe { a20_sysv_msg_message_free(context, node) };
            return -EIDRM;
        }
        let has_space = msgsz <= unsafe { (*queue).qbytes.saturating_sub((*queue).cbytes) };
        if has_space {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            unsafe { cancel_wait(waiter) };
            continue;
        }
        let linked = unsafe { a20_sysv_msg_wait_link(context, waiter, queue, true) };
        if linked {
            unsafe { claim_add(queue) };
        }
        unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        let interrupted = unsafe { a20_sysv_msg_wait_finish(waiter, linked, true) };
        let flags = unsafe { a20_sysv_msg_txn_lock(context) };
        if linked {
            unsafe { claim_drop(context, queue) };
        }
        let still_active = !unsafe { active_queue(queues, count, context, msqid) }.is_null();
        unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        if !still_active {
            unsafe { a20_sysv_msg_message_free(context, node) };
            return -EIDRM;
        }
        if interrupted {
            unsafe { a20_sysv_msg_message_free(context, node) };
            return -EINTR;
        }
        after_wait = true;
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_sysv_msg_receive_txn(
    queues: *mut SysvMsgQueue,
    count: usize,
    context: *mut c_void,
    msqid: i32,
    msgp: *mut c_void,
    msgsz: usize,
    msgtyp: i64,
    msgflg: i32,
) -> i64 {
    if queues.is_null() || count != SYSV_MSG_MAX || msgp.is_null() {
        return -EINVAL as i64;
    }
    let mut after_wait = false;
    loop {
        let flags = unsafe { a20_sysv_msg_txn_lock(context) };
        let queue = unsafe { active_queue(queues, count, context, msqid) };
        if queue.is_null() {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            return if after_wait { -EIDRM as i64 } else { -EINVAL as i64 };
        }
        let node = find_match(queue, msgtyp);
        if !node.is_null() {
            let size = node_size(node);
            if size > msgsz && msgflg & MSG_NOERROR == 0 {
                unsafe { a20_sysv_msg_txn_unlock(context, flags) };
                return -E2BIG as i64;
            }
            unsafe {
                (*node).size |= NODE_CLAIMED;
                claim_add(queue);
                a20_sysv_msg_txn_unlock(context, flags);
            }
            let out_len = size.min(msgsz);
            let copied = unsafe { a20_sysv_msg_message_copy_out(context, node, msgp, out_len) };
            let flags = unsafe { a20_sysv_msg_txn_lock(context) };
            let removing = queue_state(queue) == USED_REMOVING;
            if !removing && copied < 0 {
                unsafe {
                    (*node).size &= !NODE_CLAIMED;
                    claim_drop(context, queue);
                    a20_sysv_msg_wake(context, queue, false, true);
                    a20_sysv_msg_txn_unlock(context, flags);
                }
                return copied as i64;
            }
            let unlinked = unsafe { unlink_node(queue, node) };
            if removing {
                unsafe { claim_drop(context, queue) };
                unsafe { a20_sysv_msg_txn_unlock(context, flags) };
                unsafe { a20_sysv_msg_message_free(context, node) };
                return if copied < 0 { copied as i64 } else { -EIDRM as i64 };
            }
            if !unlinked {
                unsafe { claim_drop(context, queue) };
                unsafe { a20_sysv_msg_txn_unlock(context, flags) };
                unsafe { a20_sysv_msg_message_free(context, node) };
                return -EIDRM as i64;
            }
            unsafe {
                (*queue).cbytes -= size;
                (*queue).qnum -= 1;
                (*queue).rtime = a20_sysv_msg_txn_now(context);
                (*queue).lrpid = a20_sysv_msg_txn_pid(context);
                claim_drop(context, queue);
                a20_sysv_msg_wake(context, queue, true, true);
                a20_sysv_msg_txn_unlock(context, flags);
                a20_sysv_msg_message_free(context, node);
            }
            return out_len as i64;
        }
        if msgflg & IPC_NOWAIT != 0 {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            return -ENOMSG as i64;
        }

        unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        let waiter = unsafe { a20_sysv_msg_wait_prepare(context) };
        if waiter.is_null() {
            return -EAGAIN as i64;
        }
        let flags = unsafe { a20_sysv_msg_txn_lock(context) };
        let queue = unsafe { active_queue(queues, count, context, msqid) };
        if queue.is_null() {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            unsafe { cancel_wait(waiter) };
            return -EIDRM as i64;
        }
        if !find_match(queue, msgtyp).is_null() {
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
            unsafe { cancel_wait(waiter) };
            continue;
        }
        let linked = unsafe { a20_sysv_msg_wait_link(context, waiter, queue, false) };
        if linked {
            unsafe { claim_add(queue) };
        }
        unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        let interrupted = unsafe { a20_sysv_msg_wait_finish(waiter, linked, true) };
        let flags = unsafe { a20_sysv_msg_txn_lock(context) };
        if linked {
            unsafe { claim_drop(context, queue) };
        }
        let still_active = !unsafe { active_queue(queues, count, context, msqid) }.is_null();
        unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        if !still_active {
            return -EIDRM as i64;
        }
        if interrupted {
            return -EINTR as i64;
        }
        after_wait = true;
    }
}

/// Remove one queue while the C global lock is held. Unclaimed nodes are freed
/// immediately; claimed nodes and the queue storage remain until their owners
/// drop the final claim. Linked waiters are claims too, so queue_reset cannot
/// invalidate a wait queue beneath a sleeper.
#[no_mangle]
pub unsafe extern "C" fn a20_sysv_msg_remove_locked(
    context: *mut c_void,
    queue: *mut SysvMsgQueue,
) {
    if queue.is_null() || queue_state(queue) != USED_ACTIVE {
        return;
    }
    unsafe {
        (*queue).used = ((*queue).used & !USED_STATE) | USED_REMOVING;
        a20_sysv_msg_wake(context, queue, false, true);
        a20_sysv_msg_wake(context, queue, true, true);
    }

    let mut retained_head: *mut SysvMsgNode = core::ptr::null_mut();
    let mut retained_tail: *mut SysvMsgNode = core::ptr::null_mut();
    let mut node = unsafe { (*queue).head };
    while !node.is_null() {
        let next = unsafe { (*node).next };
        if unsafe { (*node).size & NODE_CLAIMED != 0 } {
            unsafe { (*node).next = core::ptr::null_mut() };
            if retained_tail.is_null() {
                retained_head = node;
            } else {
                unsafe { (*retained_tail).next = node };
            }
            retained_tail = node;
        } else {
            unsafe { a20_sysv_msg_message_free(context, node) };
        }
        node = next;
    }
    unsafe {
        (*queue).head = retained_head;
        (*queue).tail = retained_tail;
    }
    if queue_claims(queue) == 0 {
        unsafe { a20_sysv_msg_queue_reset(context, queue) };
    }
}

#[cfg(test)]
mod tests {
    extern crate std;
    use super::*;
    use std::boxed::Box;
    use std::vec::Vec;

    const EFAULT: i32 = 14;

    #[repr(C)]
    struct TestNode {
        node: SysvMsgNode,
        data: [u8; 64],
    }

    #[repr(C)]
    struct UserMessage {
        msg_type: i64,
        data: [u8; 64],
    }

    impl UserMessage {
        fn new(msg_type: i64, bytes: &[u8]) -> Self {
            let mut message = Self { msg_type, data: [0; 64] };
            message.data[..bytes.len()].copy_from_slice(bytes);
            message
        }
    }

    enum FinishAction {
        MakeSpace,
        QueueMessage(i64),
        Remove,
    }

    struct MockMsg {
        queue: *mut SysvMsgQueue,
        locked: bool,
        namespace: u64,
        pid: i32,
        uid: u32,
        gid: u32,
        now: u64,
        live_nodes: usize,
        prepares: usize,
        links: usize,
        resets: usize,
        wakes: usize,
        alloc_error: i32,
        read_error: bool,
        copy_error: bool,
        fail_prepare: bool,
        finish_interrupted: bool,
        remove_on_copy: bool,
        finish_action: Option<FinishAction>,
    }

    impl MockMsg {
        fn new(queue: &mut SysvMsgQueue) -> Self {
            Self {
                queue,
                locked: false,
                namespace: 7,
                pid: 55,
                uid: 1000,
                gid: 100,
                now: 100,
                live_nodes: 0,
                prepares: 0,
                links: 0,
                resets: 0,
                wakes: 0,
                alloc_error: 0,
                read_error: false,
                copy_error: false,
                fail_prepare: false,
                finish_interrupted: false,
                remove_on_copy: false,
                finish_action: None,
            }
        }
    }

    struct MockWaiter {
        context: *mut c_void,
    }

    fn empty_queue() -> SysvMsgQueue {
        SysvMsgQueue {
            used: USED_ACTIVE,
            namespace_ino: 7,
            key: 1,
            perm: SysvIpcPerm {
                key: 1,
                uid: 0,
                gid: 0,
                cuid: 0,
                cgid: 0,
                mode: 0o600,
                seq: 0,
                pad: 0,
                unused1: 0,
                unused2: 0,
            },
            stime: 0,
            rtime: 0,
            ctime: 100,
            cbytes: 0,
            qnum: 0,
            qbytes: 128,
            lspid: 0,
            lrpid: 0,
            head: core::ptr::null_mut(),
            tail: core::ptr::null_mut(),
            _waiters: [0; if usize::BITS == 64 { 24 } else { 32 }],
        }
    }

    #[test]
    fn get_ctl_metadata_and_remove_own_queue_lifecycle() {
        let mut queues: [SysvMsgQueue; SYSV_MSG_MAX] = core::array::from_fn(|_| empty_queue());
        for queue in &mut queues {
            queue.used = 0;
            queue.namespace_ino = 0;
        }
        let mut mock = MockMsg::new(&mut queues[0]);
        let context = &mut mock as *mut MockMsg as *mut c_void;
        let ptr = queues.as_mut_ptr();
        unsafe {
            assert_eq!(a20_sysv_msg_get_txn(ptr, SYSV_MSG_MAX, context, 0x1234, IPC_CREAT | 0o620), 0);
            assert_eq!(a20_sysv_msg_get_txn(ptr, SYSV_MSG_MAX, context, 0x1234, 0), 0);
            assert_eq!(a20_sysv_msg_get_txn(ptr, SYSV_MSG_MAX, context, 0x1234, IPC_CREAT | IPC_EXCL), -17);
            assert_eq!(a20_sysv_msg_get_txn(ptr, SYSV_MSG_MAX, context, 0x5678, 0), -2);
        }
        assert_eq!((queues[0].perm.uid, queues[0].perm.gid, queues[0].perm.mode, queues[0].qbytes), (1000, 100, 0o620, MSG_QBYTES));
        let mut ds = SysvMsqidDs::default();
        unsafe { assert_eq!(a20_sysv_msg_control_txn(ptr, SYSV_MSG_MAX, context, 0, IPC_STAT, (&mut ds as *mut SysvMsqidDs).cast()), 0) };
        assert_eq!((ds.perm.key, ds.perm.uid, ds.perm.mode, ds.qbytes), (0x1234, 1000, 0o620, MSG_QBYTES as u64));
        ds.perm.uid = 88;
        ds.perm.mode = 0o640;
        unsafe { assert_eq!(a20_sysv_msg_control_txn(ptr, SYSV_MSG_MAX, context, 0, IPC_SET, (&ds as *const SysvMsqidDs).cast_mut().cast()), 0) };
        assert_eq!((queues[0].perm.uid, queues[0].perm.mode), (88, 0o640));
        unsafe { assert_eq!(a20_sysv_msg_control_txn(ptr, SYSV_MSG_MAX, context, 0, IPC_RMID, core::ptr::null_mut()), 0) };
        assert_eq!(mock.resets, 1);
        assert_eq!(queues[0].used, 0);
        assert!(!mock.locked);
    }

    fn queues() -> Vec<SysvMsgQueue> {
        (0..SYSV_MSG_MAX).map(|_| empty_queue()).collect()
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_txn_lock(context: *mut c_void) -> u64 {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(!mock.locked);
        mock.locked = true;
        0x5a
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_txn_unlock(context: *mut c_void, flags: u64) {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(mock.locked);
        assert_eq!(flags, 0x5a);
        mock.locked = false;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_txn_namespace(context: *mut c_void) -> u64 {
        unsafe { (*(context as *mut MockMsg)).namespace }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_txn_pid(context: *mut c_void) -> i32 {
        unsafe { (*(context as *mut MockMsg)).pid }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_txn_uid(context: *mut c_void) -> u32 {
        unsafe { (*(context as *mut MockMsg)).uid }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_txn_gid(context: *mut c_void) -> u32 {
        unsafe { (*(context as *mut MockMsg)).gid }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_txn_now(context: *mut c_void) -> u64 {
        unsafe { (*(context as *mut MockMsg)).now }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_queue_init(queue: *mut c_void) {
        unsafe {
            let q = queue as *mut SysvMsgQueue;
            (*q).used = 0;
            (*q).namespace_ino = 0;
            (*q).head = core::ptr::null_mut();
            (*q).tail = core::ptr::null_mut();
        }
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_copy_from_user(context: *mut c_void, dst: *mut c_void, src: *const c_void, len: usize) -> i32 {
        assert!(!unsafe { (*(context as *mut MockMsg)).locked });
        unsafe { core::ptr::copy_nonoverlapping(src as *const u8, dst as *mut u8, len) };
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_copy_to_user(context: *mut c_void, dst: *mut c_void, src: *const c_void, len: usize) -> i32 {
        assert!(!unsafe { (*(context as *mut MockMsg)).locked });
        unsafe { core::ptr::copy_nonoverlapping(src as *const u8, dst as *mut u8, len) };
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_read_type(
        context: *mut c_void,
        msgp: *const c_void,
        msg_type: *mut i64,
    ) -> i32 {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        if mock.read_error {
            return -EFAULT;
        }
        unsafe { *msg_type = (*(msgp as *const UserMessage)).msg_type };
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_message_alloc(
        context: *mut c_void,
        msgp: *const c_void,
        msgsz: usize,
        status: *mut i32,
    ) -> *mut SysvMsgNode {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        if mock.alloc_error != 0 || msgsz > 64 {
            unsafe { *status = if mock.alloc_error != 0 { mock.alloc_error } else { -EFAULT } };
            return core::ptr::null_mut();
        }
        let user = unsafe { &*(msgp as *const UserMessage) };
        let mut node = Box::new(TestNode {
            node: SysvMsgNode {
                next: core::ptr::null_mut(),
                msg_type: user.msg_type,
                size: msgsz,
            },
            data: [0; 64],
        });
        node.data[..msgsz].copy_from_slice(&user.data[..msgsz]);
        mock.live_nodes += 1;
        Box::into_raw(node) as *mut SysvMsgNode
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_message_free(context: *mut c_void, node: *mut SysvMsgNode) {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(!node.is_null());
        unsafe { drop(Box::from_raw(node as *mut TestNode)) };
        mock.live_nodes -= 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_message_copy_out(
        context: *mut c_void,
        node: *mut SysvMsgNode,
        msgp: *mut c_void,
        len: usize,
    ) -> i32 {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(!mock.locked);
        if mock.remove_on_copy {
            mock.remove_on_copy = false;
            let flags = unsafe { a20_sysv_msg_txn_lock(context) };
            unsafe { a20_sysv_msg_remove_locked(context, mock.queue) };
            unsafe { a20_sysv_msg_txn_unlock(context, flags) };
        }
        if mock.copy_error {
            return -EFAULT;
        }
        let node = unsafe { &*(node as *mut TestNode) };
        let user = unsafe { &mut *(msgp as *mut UserMessage) };
        user.msg_type = node.node.msg_type;
        user.data[..len].copy_from_slice(&node.data[..len]);
        0
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_wait_prepare(context: *mut c_void) -> *mut c_void {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(!mock.locked);
        mock.prepares += 1;
        if mock.fail_prepare {
            return core::ptr::null_mut();
        }
        Box::into_raw(Box::new(MockWaiter { context })).cast()
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_wait_link(
        context: *mut c_void,
        _waiter: *mut c_void,
        _queue: *mut SysvMsgQueue,
        _send: bool,
    ) -> bool {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(mock.locked);
        mock.links += 1;
        true
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_wait_finish(
        waiter: *mut c_void,
        linked: bool,
        commit: bool,
    ) -> bool {
        let waiter = unsafe { Box::from_raw(waiter as *mut MockWaiter) };
        let context = waiter.context;
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(!mock.locked);
        if linked && commit {
            match mock.finish_action.take() {
                Some(FinishAction::MakeSpace) => {
                    let queue = unsafe { &mut *mock.queue };
                    let old = queue.head;
                    if !old.is_null() {
                        let size = node_size(old);
                        queue.head = unsafe { (*old).next };
                        if queue.head.is_null() {
                            queue.tail = core::ptr::null_mut();
                        }
                        queue.cbytes -= size;
                        queue.qnum -= 1;
                        unsafe { a20_sysv_msg_message_free(context, old) };
                    }
                }
                Some(FinishAction::QueueMessage(msg_type)) => {
                    let user = UserMessage::new(msg_type, &[9]);
                    let mut status = 0;
                    let node = unsafe {
                        a20_sysv_msg_message_alloc(context, (&user as *const UserMessage).cast(), 1, &mut status)
                    };
                    let queue = unsafe { &mut *mock.queue };
                    if queue.tail.is_null() {
                        queue.head = node;
                        queue.tail = node;
                    } else {
                        unsafe { (*queue.tail).next = node };
                        queue.tail = node;
                    }
                    queue.cbytes += 1;
                    queue.qnum += 1;
                }
                Some(FinishAction::Remove) => {
                    let flags = unsafe { a20_sysv_msg_txn_lock(context) };
                    unsafe { a20_sysv_msg_remove_locked(context, mock.queue) };
                    unsafe { a20_sysv_msg_txn_unlock(context, flags) };
                }
                None => {}
            }
        }
        mock.finish_interrupted
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_wake(
        context: *mut c_void,
        _queue: *mut SysvMsgQueue,
        _send: bool,
        _all: bool,
    ) {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(mock.locked);
        mock.wakes += 1;
    }

    #[no_mangle]
    unsafe extern "C" fn a20_sysv_msg_queue_reset(context: *mut c_void, queue: *mut SysvMsgQueue) {
        let mock = unsafe { &mut *(context as *mut MockMsg) };
        assert!(mock.locked);
        unsafe { *queue = empty_queue() };
        unsafe {
            (*queue).used = 0;
            (*queue).namespace_ino = 0;
            (*queue).qbytes = 0;
        }
        mock.resets += 1;
    }

    unsafe fn send(
        queues: *mut SysvMsgQueue,
        context: *mut c_void,
        message: &UserMessage,
        len: usize,
        flags: i32,
    ) -> i32 {
        unsafe {
            a20_sysv_msg_send_txn(
                queues,
                SYSV_MSG_MAX,
                context,
                0,
                (message as *const UserMessage).cast(),
                len,
                flags,
            )
        }
    }

    unsafe fn receive(
        queues: *mut SysvMsgQueue,
        context: *mut c_void,
        message: &mut UserMessage,
        len: usize,
        msgtyp: i64,
        flags: i32,
    ) -> i64 {
        unsafe {
            a20_sysv_msg_receive_txn(
                queues,
                SYSV_MSG_MAX,
                context,
                0,
                (message as *mut UserMessage).cast(),
                len,
                msgtyp,
                flags,
            )
        }
    }

    #[test]
    fn type_selection_and_accounting() {
        let mut queues = queues();
        let mut mock = MockMsg::new(&mut queues[0]);
        let context = &mut mock as *mut MockMsg as *mut c_void;
        for (msg_type, byte) in [(2, b'a'), (1, b'b'), (2, b'c'), (3, b'd')] {
            let message = UserMessage::new(msg_type, &[byte]);
            unsafe { assert_eq!(send(queues.as_mut_ptr(), context, &message, 1, 0), 0) };
        }
        assert_eq!((queues[0].qnum, queues[0].cbytes, mock.live_nodes), (4, 4, 4));
        let mut received = UserMessage::new(0, &[]);
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 2, 0), 1);
        }
        assert_eq!((received.msg_type, received.data[0]), (2, b'a'));
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, -2, 0), 1);
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 0, 0), 1);
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 3, 0), 1);
        }
        assert_eq!((queues[0].qnum, queues[0].cbytes, mock.live_nodes), (0, 0, 0));
        assert_eq!(queues[0].lspid, 55);
        assert_eq!(queues[0].lrpid, 55);
        assert!(queues[0].stime >= 100 && queues[0].rtime >= 100);
        assert!(!mock.locked);
    }

    #[test]
    fn small_buffer_and_copy_failure_roll_back() {
        let mut queues = queues();
        let mut mock = MockMsg::new(&mut queues[0]);
        let context = &mut mock as *mut MockMsg as *mut c_void;
        let message = UserMessage::new(5, b"test");
        unsafe { assert_eq!(send(queues.as_mut_ptr(), context, &message, 4, 0), 0) };
        let mut received = UserMessage::new(0, &[]);
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 2, 0, 0), -E2BIG as i64);
        }
        assert_eq!((queues[0].qnum, queues[0].cbytes, queues[0].used), (1, 4, 1));
        mock.copy_error = true;
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 4, 0, 0), -EFAULT as i64);
        }
        assert_eq!((queues[0].qnum, queues[0].cbytes, queues[0].used), (1, 4, 1));
        assert_eq!(unsafe { (*queues[0].head).size }, 4);
        mock.copy_error = false;
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 2, 0, MSG_NOERROR), 2);
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 2, 0, IPC_NOWAIT), -ENOMSG as i64);
        }
        assert_eq!((received.data[0], received.data[1], mock.live_nodes), (b't', b'e', 0));
        let invalid = UserMessage::new(0, &[1]);
        unsafe {
            assert_eq!(send(queues.as_mut_ptr(), context, &invalid, 1, 0), -EINVAL);
            assert_eq!(send(queues.as_mut_ptr(), context, &message, MSG_MAX + 1, 0), -EINVAL);
        }
    }

    #[test]
    fn blocking_recheck_interrupt_and_prepare_failures() {
        let mut queues = queues();
        queues[0].qbytes = 1;
        let mut mock = MockMsg::new(&mut queues[0]);
        let context = &mut mock as *mut MockMsg as *mut c_void;
        let first = UserMessage::new(1, &[1]);
        let second = UserMessage::new(2, &[2]);
        unsafe { assert_eq!(send(queues.as_mut_ptr(), context, &first, 1, 0), 0) };
        mock.finish_action = Some(FinishAction::MakeSpace);
        unsafe { assert_eq!(send(queues.as_mut_ptr(), context, &second, 1, 0), 0) };
        assert_eq!((mock.prepares, mock.links, mock.live_nodes), (1, 1, 1));
        let mut received = UserMessage::new(0, &[]);
        mock.finish_action = Some(FinishAction::QueueMessage(9));
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 2, 0), 1);
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 9, 0), 1);
        }
        mock.finish_interrupted = true;
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 0, 0), -EINTR as i64);
            assert_eq!(send(queues.as_mut_ptr(), context, &first, 1, 0), 0);
            assert_eq!(send(queues.as_mut_ptr(), context, &second, 1, 0), -EINTR);
        }
        assert_eq!(mock.live_nodes, 1);
        mock.finish_interrupted = false;
        mock.fail_prepare = true;
        unsafe {
            assert_eq!(send(queues.as_mut_ptr(), context, &second, 1, 0), -EAGAIN);
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 1, 0), 1);
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 0, 0), -EAGAIN as i64);
        }
        assert_eq!(mock.live_nodes, 0);
        assert!(!mock.locked);
    }

    #[test]
    fn remove_during_copy_and_wait_defers_reclaim() {
        let mut queues = queues();
        let mut mock = MockMsg::new(&mut queues[0]);
        let context = &mut mock as *mut MockMsg as *mut c_void;
        let first = UserMessage::new(1, &[1]);
        let second = UserMessage::new(2, &[2]);
        unsafe {
            assert_eq!(send(queues.as_mut_ptr(), context, &first, 1, 0), 0);
            assert_eq!(send(queues.as_mut_ptr(), context, &second, 1, 0), 0);
        }
        mock.remove_on_copy = true;
        let mut received = UserMessage::new(0, &[]);
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 1, 0), -EIDRM as i64);
        }
        assert_eq!((queues[0].used, mock.live_nodes, mock.resets), (0, 0, 1));

        queues[0] = empty_queue();
        mock.finish_action = Some(FinishAction::Remove);
        unsafe {
            assert_eq!(receive(queues.as_mut_ptr(), context, &mut received, 1, 0, 0), -EIDRM as i64);
        }
        assert_eq!((queues[0].used, mock.live_nodes, mock.resets), (0, 0, 2));
        assert!(!mock.locked);
    }
}
