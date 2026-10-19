#![no_std]

//! Rust-owned Native channel send transaction. C bridges only expose the
//! immutable channel contract and perform lock/park/allocation mechanisms.
#[cfg(test)]
extern crate std;

use core::ffi::c_void;
use core::mem::{size_of, MaybeUninit};
use core::ptr;

const ERR_INVALID: i64 = 12;
const ERR_WOULD_BLOCK: i64 = 18;
#[cfg(test)]
const ERR_INTERRUPTED: i64 = 3;
const NONBLOCK: u32 = 1;

type CloseHook = unsafe extern "C" fn(*mut c_void);

#[repr(C)]
struct CloseLocks {
    ep: *mut c_void,
    peer: *mut c_void,
    peer_locked: i32,
}

#[repr(C)]
struct CloseView {
    peer: *mut c_void,
    peer_link: *mut *mut c_void,
    peer_backlink: *mut *mut c_void,
    peer_closed: *mut i32,
    hook: *mut Option<CloseHook>,
    ctx_ref: *mut Option<CloseHook>,
    ctx_put: *mut Option<CloseHook>,
    hook_ctx: *mut *mut c_void,
    message_head: *mut *mut MessageHeader,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MessageHeader {
    data_len: u32,
    handle_count: u32,
    next: *mut MessageHeader,
}

unsafe extern "C" {
    fn a20_channel_close_lock(ep: *mut c_void, locks: *mut CloseLocks);
    fn a20_channel_close_unlock_peer(locks: *mut CloseLocks);
    fn a20_channel_close_unlock(locks: *mut CloseLocks);
    fn a20_channel_close_view(locks: *mut CloseLocks, view: *mut CloseView);
    fn a20_channel_close_stats_dec();
    fn a20_channel_close_wake(ep: *mut c_void);
    fn a20_channel_close_event(ep: *mut c_void, event: u32);
    fn a20_channel_close_destroy_watches(ep: *mut c_void);
    fn a20_channel_close_free_msg(msg: *mut MessageHeader);
    fn a20_channel_close_free_ep(ep: *mut c_void);
}

struct CloseGuard(*mut CloseLocks);
impl Drop for CloseGuard {
    fn drop(&mut self) { unsafe { a20_channel_close_unlock(self.0) } }
}

const EVENT_PEER_CLOSED: u32 = 9;
const EVENT_CLOSED: u32 = 3;

fn capture_peer_hook(view: &CloseView) -> (Option<CloseHook>, Option<CloseHook>, *mut c_void) {
    unsafe {
        let hook = if view.hook.is_null() { None } else { *view.hook };
        let put = if view.ctx_put.is_null() { None } else { *view.ctx_put };
        let ctx_ref = if view.ctx_ref.is_null() { None } else { *view.ctx_ref };
        let context = if view.hook_ctx.is_null() { core::ptr::null_mut() } else { *view.hook_ctx };
        if hook.is_some() {
            if let Some(reference) = ctx_ref { reference(context); }
        }
        (hook, put, context)
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_channel_peer_shutdown(ep: *mut c_void) {
    if ep.is_null() { return; }
    let mut locks = MaybeUninit::<CloseLocks>::uninit();
    unsafe { a20_channel_close_lock(ep, locks.as_mut_ptr()) };
    let locks = unsafe { locks.assume_init_mut() };
    let guard = CloseGuard(locks);
    let mut view = MaybeUninit::<CloseView>::uninit();
    unsafe { a20_channel_close_view(locks, view.as_mut_ptr()) };
    let view = unsafe { view.assume_init() };
    if view.peer.is_null() { drop(guard); return; }

    unsafe { *view.peer_closed = 1; }
    let (hook, put, context) = capture_peer_hook(&view);
    unsafe { a20_channel_close_unlock_peer(locks); }
    unsafe {
        a20_channel_close_wake(view.peer);
        a20_channel_close_event(view.peer, EVENT_PEER_CLOSED);
    }
    drop(guard);
    if let Some(hook) = hook { unsafe { hook(context) }; }
    if hook.is_some() { if let Some(put) = put { unsafe { put(context) }; } }
}

#[no_mangle]
/// Finalize an endpoint after the C entrypoint has consumed its last reference.
/// The refcount must already be zero; this function must never decrement it.
pub unsafe extern "C" fn a20_rust_channel_ep_finalize(ep: *mut c_void) {
    if ep.is_null() { return; }
    unsafe { a20_channel_close_stats_dec(); }

    let mut locks = MaybeUninit::<CloseLocks>::uninit();
    unsafe { a20_channel_close_lock(ep, locks.as_mut_ptr()) };
    let locks = unsafe { locks.assume_init_mut() };
    let guard = CloseGuard(locks);
    let mut view = MaybeUninit::<CloseView>::uninit();
    unsafe { a20_channel_close_view(locks, view.as_mut_ptr()) };
    let view = unsafe { view.assume_init() };
    let (hook, put, context) = if view.peer.is_null() {
        (None, None, core::ptr::null_mut())
    } else {
        unsafe { *view.peer_closed = 1; }
        let saved = capture_peer_hook(&view);
        unsafe {
            if *view.peer_backlink == ep { *view.peer_backlink = core::ptr::null_mut(); }
            *view.peer_link = core::ptr::null_mut();
            a20_channel_close_unlock_peer(locks);
            a20_channel_close_wake(view.peer);
            a20_channel_close_event(view.peer, EVENT_PEER_CLOSED);
        }
        saved
    };

    unsafe {
        a20_channel_close_event(ep, EVENT_CLOSED);
        a20_channel_close_destroy_watches(ep);
    }
    drop(guard);
    if let Some(hook) = hook { unsafe { hook(context) }; }
    if hook.is_some() { if let Some(put) = put { unsafe { put(context) }; } }

    let mut message = unsafe { *view.message_head };
    while !message.is_null() {
        let next = unsafe { (*message).next };
        unsafe { a20_channel_close_free_msg(message); }
        message = next;
    }
    unsafe { a20_channel_close_free_ep(ep); }
}
const MAX_DATA: u32 = 65_536;
const MAX_HANDLES: u32 = 8;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct HandleInfo {
    object: *mut c_void,
    ty: u16,
    pad: u16,
    transfer_rights: u64,
    expiry_tick: u64,
    remaining_ops: u32,
    temporal_flags: u32,
    security_label: u8,
    pad2: [u8; 7],
}

#[repr(C)]
struct QueueView {
    head: *mut *mut c_void,
    tail: *mut *mut c_void,
    count: *mut u32,
    capacity: *const u32,
    total_data: *mut u32,
    peer_closed: *const i32,
}

const _: () = {
    use core::mem::{align_of, offset_of, size_of};
    assert!(size_of::<HandleInfo>() == if usize::BITS == 64 { 48 } else { 40 });
    #[cfg(any(target_arch = "arm", target_arch = "riscv32"))]
    assert!(align_of::<HandleInfo>() == 8);
    #[cfg(not(any(target_arch = "arm", target_arch = "riscv32")))]
    assert!(align_of::<HandleInfo>() == if usize::BITS == 64 { 8 } else { 4 });
    assert!(offset_of!(HandleInfo, object) == 0);
    assert!(offset_of!(HandleInfo, ty) == size_of::<usize>());
    assert!(offset_of!(HandleInfo, pad) == size_of::<usize>() + 2);
    assert!(offset_of!(HandleInfo, transfer_rights) == if usize::BITS == 64 { 16 } else { 8 });
    assert!(offset_of!(HandleInfo, expiry_tick) == if usize::BITS == 64 { 24 } else { 16 });
    assert!(offset_of!(HandleInfo, remaining_ops) == if usize::BITS == 64 { 32 } else { 24 });
    assert!(offset_of!(HandleInfo, temporal_flags) == if usize::BITS == 64 { 36 } else { 28 });
    assert!(offset_of!(HandleInfo, security_label) == if usize::BITS == 64 { 40 } else { 32 });
    assert!(offset_of!(HandleInfo, pad2) == if usize::BITS == 64 { 41 } else { 33 });
    assert!(size_of::<MessageHeader>() == 8 + size_of::<usize>());
    assert!(offset_of!(MessageHeader, next) == 8);
    assert!(size_of::<QueueView>() == 6 * size_of::<usize>());
};

unsafe extern "C" {
    fn a20_channel_send_limits(ep: *mut c_void, max_data: *mut u32,
                               max_handles: *mut u32, types: *mut u32,
                               typed: *mut i32);
    fn a20_channel_send_hold(ep: *mut c_void);
    fn a20_channel_send_alloc(data: *const c_void, data_len: u32,
                              handles: *const HandleInfo, handle_count: u32)
                              -> *mut c_void;
    fn a20_channel_send_lock_peer(ep: *mut c_void) -> *mut c_void;
    fn a20_channel_send_unlock_peer(peer: *mut c_void);
    fn a20_channel_queue_view(ep: *mut c_void, view: *mut QueueView);
    fn a20_channel_send_notify(peer: *mut c_void, defer_wake: i32);
    fn a20_channel_send_wait_space(ep: *mut c_void) -> i64;
    fn a20_channel_send_free(msg: *mut c_void, release_refs: i32);
    fn a20_channel_trace(op: u32, len: u32, p1: *mut c_void,
                         p2: *mut c_void, meta: u32);
    fn a20_channel_ep_release(ep: *mut c_void);
    fn a20_object_ref(object: *mut c_void, ty: u16);
    fn a20_object_release(object: *mut c_void, ty: u16);
}

trait SendOps {
    unsafe fn limits(&mut self, ep: *mut c_void, max_data: &mut u32,
                     max_handles: &mut u32, types: &mut u32, typed: &mut i32);
    unsafe fn hold(&mut self, ep: *mut c_void);
    unsafe fn put_ep(&mut self, ep: *mut c_void);
    unsafe fn ref_object(&mut self, object: *mut c_void, ty: u16);
    unsafe fn put_object(&mut self, object: *mut c_void, ty: u16);
    unsafe fn alloc(&mut self, data: *const c_void, len: u32,
                    handles: *const HandleInfo, count: u32) -> *mut c_void;
    unsafe fn lock_peer(&mut self, ep: *mut c_void) -> *mut c_void;
    unsafe fn unlock_peer(&mut self, peer: *mut c_void);
    unsafe fn queue_view(&mut self, peer: *mut c_void, view: *mut QueueView);
    unsafe fn notify(&mut self, peer: *mut c_void, defer_wake: i32);
    unsafe fn wait_space(&mut self, ep: *mut c_void) -> i64;
    unsafe fn free(&mut self, msg: *mut c_void);
    unsafe fn trace(&mut self, op: u32, len: u32, msg: *mut c_void,
                    meta: u32);
}

struct KernelOps;
impl SendOps for KernelOps {
    unsafe fn limits(&mut self, ep: *mut c_void, max_data: &mut u32,
                     max_handles: &mut u32, types: &mut u32, typed: &mut i32) {
        unsafe { a20_channel_send_limits(ep, max_data, max_handles, types, typed); }
    }
    unsafe fn hold(&mut self, ep: *mut c_void) { unsafe { a20_channel_send_hold(ep); } }
    unsafe fn put_ep(&mut self, ep: *mut c_void) { unsafe { a20_channel_ep_release(ep); } }
    unsafe fn ref_object(&mut self, object: *mut c_void, ty: u16) {
        unsafe { a20_object_ref(object, ty); }
    }
    unsafe fn put_object(&mut self, object: *mut c_void, ty: u16) {
        unsafe { a20_object_release(object, ty); }
    }
    unsafe fn alloc(&mut self, data: *const c_void, len: u32,
                    handles: *const HandleInfo, count: u32) -> *mut c_void {
        unsafe { a20_channel_send_alloc(data, len, handles, count) }
    }
    unsafe fn lock_peer(&mut self, ep: *mut c_void) -> *mut c_void {
        unsafe { a20_channel_send_lock_peer(ep) }
    }
    unsafe fn unlock_peer(&mut self, peer: *mut c_void) {
        unsafe { a20_channel_send_unlock_peer(peer); }
    }
    unsafe fn queue_view(&mut self, peer: *mut c_void, view: *mut QueueView) {
        unsafe { a20_channel_queue_view(peer, view); }
    }
    unsafe fn notify(&mut self, peer: *mut c_void, defer_wake: i32) {
        unsafe { a20_channel_send_notify(peer, defer_wake); }
    }
    unsafe fn wait_space(&mut self, ep: *mut c_void) -> i64 {
        unsafe { a20_channel_send_wait_space(ep) }
    }
    unsafe fn free(&mut self, msg: *mut c_void) { unsafe { a20_channel_send_free(msg, 0); } }
    unsafe fn trace(&mut self, op: u32, len: u32, msg: *mut c_void, meta: u32) {
        unsafe { a20_channel_trace(op, len, msg, ptr::null_mut(), meta); }
    }
}

/// Perform one FIFO commit while holding a pinned peer lock. The C side
/// supplies only the lock/reference and individual field primitives; queue
/// capacity, link order, count, and byte accounting are owned here.
unsafe fn queue_once<O: SendOps>(ops: &mut O, ep: *mut c_void,
                                 msg: *mut c_void, defer_wake: i32) -> i32 {
    let peer = unsafe { ops.lock_peer(ep) };
    if peer.is_null() { return -20; } // CANCELED
    let mut uninit = MaybeUninit::uninit();
    unsafe { ops.queue_view(peer, uninit.as_mut_ptr()); }
    let view = unsafe { uninit.assume_init() };
    if unsafe { view.peer_closed.read() != 0 } {
        unsafe { ops.unlock_peer(peer); }
        return -20;
    }
    let count = unsafe { view.count.read() };
    if count >= unsafe { view.capacity.read() } {
        unsafe { ops.unlock_peer(peer); }
        return 0;
    }

    let tail = unsafe { view.tail.read() };
    let header = msg.cast::<MessageHeader>();
    unsafe { (*header).next = ptr::null_mut(); }
    if tail.is_null() {
        unsafe { view.head.write(msg); }
    } else {
        unsafe { (*tail.cast::<MessageHeader>()).next = msg.cast::<MessageHeader>(); }
    }
    unsafe {
        view.tail.write(msg);
        view.count.write(count.wrapping_add(1));
        view.total_data.write(view.total_data.read().wrapping_add((*header).data_len));
        ops.notify(peer, defer_wake);
        ops.unlock_peer(peer);
    }
    1
}

unsafe fn release_handles<O: SendOps>(ops: &mut O, handles: *const HandleInfo,
                                       count: u32) {
    if handles.is_null() { return; }
    for i in 0..count as usize {
        let info = unsafe { &*handles.add(i) };
        unsafe { ops.put_object(info.object, info.ty); }
    }
}

unsafe fn send_with<O: SendOps>(ops: &mut O, ep: *mut c_void,
    data: *const c_void, data_len: u32, handles: *const HandleInfo,
    handle_count: u32, flags: u32, defer_wake: i32) -> i64 {
    if ep.is_null() { return -5; } // BAD_HANDLE
    if data_len > MAX_DATA || handle_count > MAX_HANDLES || flags & !NONBLOCK != 0 {
        return -ERR_INVALID;
    }

    unsafe { ops.hold(ep); }
    let mut result;
    let (mut max_data, mut max_handles, mut send_types, mut typed) =
        (0u32, 0u32, 0u32, 0i32);
    unsafe { ops.limits(ep, &mut max_data, &mut max_handles,
                        &mut send_types, &mut typed); }
    if (max_data != 0 && data_len > max_data) ||
       (max_handles != 0 && handle_count > max_handles) {
        result = -ERR_INVALID;
        unsafe { ops.put_ep(ep); }
        return result;
    }
    if typed != 0 && handle_count != 0 {
        for i in 0..handle_count as usize {
            let ty = unsafe { (*handles.add(i)).ty } as u32;
            if ty >= 32 || send_types & (1u32 << ty) == 0 {
                unsafe { ops.put_ep(ep); }
                return -23; // TYPE_MISMATCH
            }
        }
    }

    for i in 0..handle_count as usize {
        let info = unsafe { &*handles.add(i) };
        unsafe { ops.ref_object(info.object, info.ty); }
    }
    let msg = unsafe { ops.alloc(data, data_len, handles, handle_count) };
    if msg.is_null() {
        unsafe {
            release_handles(ops, handles, handle_count);
            ops.put_ep(ep);
        }
        return -6; // NO_MEMORY
    }
    unsafe { ops.trace(1, data_len, msg, handle_count); }

    loop {
        let queued = unsafe { queue_once(ops, ep, msg, defer_wake) };
        if queued > 0 {
            unsafe { ops.trace(3, data_len, msg, queued as u32); }
            result = 0;
            break;
        }
        if queued < 0 {
            unsafe { ops.trace(4, data_len, msg, (-queued) as u32); }
            result = queued as i64;
            break;
        }
        if flags & NONBLOCK != 0 {
            result = -ERR_WOULD_BLOCK;
            break;
        }
        result = unsafe { ops.wait_space(ep) };
        if result != 0 { break; }
    }

    if result != 0 {
        unsafe {
            ops.free(msg);
            release_handles(ops, handles, handle_count);
        }
    }
    unsafe { ops.put_ep(ep); }
    result
}

unsafe extern "C" {
    fn a20_channel_recv_queue_view(ep: *mut c_void, view: *mut QueueView);
    fn a20_channel_recv_lock(ep: *mut c_void);
    fn a20_channel_recv_hold(ep: *mut c_void);
    fn a20_channel_recv_wait(ep: *mut c_void) -> i64;
    fn a20_channel_recv_free_msg_refs(msg: *mut c_void);
    fn a20_channel_recv_trace_begin(len: u32, msg: *mut c_void, count: u32);
    fn a20_channel_recv_contract(ep: *mut c_void, types: *mut u32, typed: *mut i32);
    fn a20_channel_recv_copy(msg: *mut c_void, data: *mut c_void,
                             handles: *mut HandleInfo);
    fn a20_channel_recv_wake_sender(ep: *mut c_void);
    fn a20_channel_recv_unlock(ep: *mut c_void);
    fn a20_channel_recv_release_ep(ep: *mut c_void);
    fn a20_channel_recv_free_msg(msg: *mut c_void);
    fn a20_channel_recv_trace(len: u32, msg: *mut c_void, data: *mut c_void,
                              handles: u32);
}

trait RecvOps {
    unsafe fn queue_view(&mut self, ep: *mut c_void, view: *mut QueueView);
    unsafe fn lock(&mut self, ep: *mut c_void);
    unsafe fn hold_ep(&mut self, ep: *mut c_void);
    unsafe fn wait(&mut self, ep: *mut c_void) -> i64;
    unsafe fn free_msg_refs(&mut self, msg: *mut c_void);
    unsafe fn trace_begin(&mut self, len: u32, msg: *mut c_void, count: u32);
    unsafe fn contract(&mut self, ep: *mut c_void, types: &mut u32, typed: &mut i32);
    unsafe fn copy_out(&mut self, msg: *mut c_void, data: *mut c_void,
                       handles: *mut HandleInfo);
    unsafe fn wake_sender(&mut self, ep: *mut c_void);
    unsafe fn unlock(&mut self, ep: *mut c_void);
    unsafe fn put_ep(&mut self, ep: *mut c_void);
    unsafe fn free_msg(&mut self, msg: *mut c_void);
    unsafe fn trace(&mut self, len: u32, msg: *mut c_void, data: *mut c_void,
                    handles: u32);
}

struct KernelRecvOps;
impl RecvOps for KernelRecvOps {
    unsafe fn queue_view(&mut self, ep: *mut c_void, view: *mut QueueView) {
        unsafe { a20_channel_recv_queue_view(ep, view); }
    }
    unsafe fn lock(&mut self, ep: *mut c_void) { unsafe { a20_channel_recv_lock(ep); } }
    unsafe fn hold_ep(&mut self, ep: *mut c_void) { unsafe { a20_channel_recv_hold(ep); } }
    unsafe fn wait(&mut self, ep: *mut c_void) -> i64 { unsafe { a20_channel_recv_wait(ep) } }
    unsafe fn free_msg_refs(&mut self, msg: *mut c_void) { unsafe { a20_channel_recv_free_msg_refs(msg); } }
    unsafe fn trace_begin(&mut self, len: u32, msg: *mut c_void, count: u32) {
        unsafe { a20_channel_recv_trace_begin(len, msg, count); }
    }
    unsafe fn contract(&mut self, ep: *mut c_void, types: &mut u32, typed: &mut i32) {
        unsafe { a20_channel_recv_contract(ep, types, typed); }
    }
    unsafe fn copy_out(&mut self, msg: *mut c_void, data: *mut c_void, handles: *mut HandleInfo) {
        unsafe { a20_channel_recv_copy(msg, data, handles); }
    }
    unsafe fn wake_sender(&mut self, ep: *mut c_void) { unsafe { a20_channel_recv_wake_sender(ep); } }
    unsafe fn unlock(&mut self, ep: *mut c_void) { unsafe { a20_channel_recv_unlock(ep); } }
    unsafe fn put_ep(&mut self, ep: *mut c_void) { unsafe { a20_channel_recv_release_ep(ep); } }
    unsafe fn free_msg(&mut self, msg: *mut c_void) { unsafe { a20_channel_recv_free_msg(msg); } }
    unsafe fn trace(&mut self, len: u32, msg: *mut c_void, data: *mut c_void, hc: u32) {
        unsafe { a20_channel_recv_trace(len, msg, data, hc); }
    }
}

/// A receive transaction pins its endpoint and may hold its queue lock. The
/// stack guard cleans both together on every local error; successful begin or
/// probe transfers that pair to the finish/abort ABI entry point.
struct RecvLease<'a, O: RecvOps> {
    ops: &'a mut O,
    ep: *mut c_void,
    held: bool,
    locked: bool,
}

impl<'a, O: RecvOps> RecvLease<'a, O> {
    unsafe fn hold(ops: &'a mut O, ep: *mut c_void) -> Self {
        unsafe { ops.hold_ep(ep); }
        Self { ops, ep, held: true, locked: false }
    }

    fn locked(ops: &'a mut O, ep: *mut c_void) -> Self {
        Self { ops, ep, held: true, locked: true }
    }

    unsafe fn lock(&mut self) {
        unsafe { self.ops.lock(self.ep); }
        self.locked = true;
    }

    unsafe fn unlock(&mut self) {
        if self.locked {
            unsafe { self.ops.unlock(self.ep); }
            self.locked = false;
        }
    }

    unsafe fn release(&mut self) {
        if self.held {
            unsafe { self.ops.put_ep(self.ep); }
            self.held = false;
        }
    }

    fn transfer(mut self) {
        self.locked = false;
        self.held = false;
    }
}

impl<O: RecvOps> Drop for RecvLease<'_, O> {
    fn drop(&mut self) {
        unsafe {
            self.unlock();
            self.release();
        }
    }
}

unsafe fn recv_probe_core_with<O: RecvOps>(lease: &mut RecvLease<'_, O>,
    out_len: *mut u32, out_handles: *mut u32) -> i64 {
    let ep = lease.ep;
    let mut uninit = MaybeUninit::uninit();
    unsafe { lease.ops.queue_view(ep, uninit.as_mut_ptr()); }
    let view = unsafe { uninit.assume_init() };
    let count = unsafe { view.count.read() };
    if count == 0 { return 1; }
    let msg = unsafe { view.head.read() };
    let header = msg.cast::<MessageHeader>();
    let len = unsafe { (*header).data_len };
    let hc = unsafe { (*header).handle_count };
    let (mut types, mut typed) = (0u32, 0i32);
    if hc != 0 {
        unsafe { lease.ops.contract(ep, &mut types, &mut typed); }
    }
    if hc != 0 && typed != 0 {
        for index in 0..hc {
            let info = unsafe { msg.cast::<u8>().add(size_of::<MessageHeader>() +
                len as usize + index as usize * size_of::<HandleInfo>()) };
            let ty = unsafe { info.add(size_of::<usize>()).cast::<u16>().read_unaligned() } as u32;
            if ty >= 32 || types & (1u32 << ty) == 0 {
                let next = unsafe { (*header).next.cast::<c_void>() };
                unsafe {
                    view.head.write(next);
                    if next.is_null() { view.tail.write(ptr::null_mut()); }
                    view.count.write(count.wrapping_sub(1));
                    view.total_data.write(view.total_data.read().wrapping_sub(len));
                    lease.ops.wake_sender(ep);
                }
                /* Dropping message handle refs can release either endpoint
                 * and recursively take channel locks. Match the C ordering:
                 * dequeue/wake while locked, then unlock, release message
                 * refs, and finally drop our endpoint pin. Disarm the lease
                 * as each ownership edge is consumed so Drop cannot repeat
                 * either unlock or put. */
                unsafe { lease.unlock(); }
                unsafe { lease.ops.free_msg_refs(msg); }
                unsafe { lease.release(); }
                return -23;
            }
        }
    }
    unsafe {
        if !out_len.is_null() { out_len.write(len); }
        if !out_handles.is_null() { out_handles.write(hc); }
        lease.ops.trace_begin(len, msg, count);
    }
    0 // locked message and reference pass to finish/abort
}

unsafe fn recv_probe_locked_with<O: RecvOps>(ops: &mut O, ep: *mut c_void,
    out_len: *mut u32, out_handles: *mut u32) -> i64 {
    let mut lease = RecvLease::locked(ops, ep);
    let result = unsafe { recv_probe_core_with(&mut lease, out_len, out_handles) };
    if result >= 0 { lease.transfer(); }
    result
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_channel_recv_probe_locked(ep: *mut c_void,
    out_len: *mut u32, out_handles: *mut u32) -> i64 {
    unsafe { recv_probe_locked_with(&mut KernelRecvOps, ep, out_len, out_handles) }
}

unsafe fn recv_begin_with<O: RecvOps>(ops: &mut O, ep: *mut c_void,
    flags: u32, out_len: *mut u32, out_handles: *mut u32) -> i64 {
    if ep.is_null() { return -5; }
    if flags & !NONBLOCK != 0 { return -ERR_INVALID; }
    let mut lease = unsafe { RecvLease::hold(ops, ep) };
    loop {
        unsafe { lease.lock(); }
        let probe = unsafe { recv_probe_core_with(&mut lease, out_len, out_handles) };
        if probe == 0 { lease.transfer(); return 0; }
        if probe < 0 { return probe; }
        let mut uninit = MaybeUninit::uninit();
        unsafe { lease.ops.queue_view(ep, uninit.as_mut_ptr()); }
        let view = unsafe { uninit.assume_init() };
        if unsafe { view.peer_closed.read() != 0 } { return -20; }
        unsafe { lease.unlock(); }
        if flags & NONBLOCK != 0 { return -ERR_WOULD_BLOCK; }
        let wait_rc = unsafe { lease.ops.wait(ep) };
        if wait_rc != 0 { return wait_rc; }
    }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_channel_recv_begin(ep: *mut c_void,
    flags: u32, out_len: *mut u32, out_handles: *mut u32) -> i64 {
    unsafe { recv_begin_with(&mut KernelRecvOps, ep, flags, out_len, out_handles) }
}

unsafe fn recv_finish_with<O: RecvOps>(ops: &mut O, ep: *mut c_void,
    data: *mut c_void, data_len: *mut u32, handles: *mut HandleInfo,
    handle_count: *mut u32) -> i64 {
    let mut lease = RecvLease::locked(ops, ep);
    let mut uninit = MaybeUninit::uninit();
    unsafe { lease.ops.queue_view(ep, uninit.as_mut_ptr()); }
    let view = unsafe { uninit.assume_init() };
    let msg = unsafe { view.head.read() };
    if msg.is_null() {
        return -ERR_WOULD_BLOCK;
    }
    let header = msg.cast::<MessageHeader>();
    let (out_len, hc) = unsafe { ((*header).data_len, (*header).handle_count) };
    if out_len > 0 && (data.is_null() || data_len.is_null() ||
                       unsafe { *data_len } < out_len) {
        return -13; // NO_SPACE
    }
    if hc > 0 && (handles.is_null() || handle_count.is_null() ||
                  unsafe { *handle_count } < hc) {
        return -13;
    }
    let (mut recv_types, mut typed) = (0u32, 0i32);
    if hc != 0 {
        unsafe { lease.ops.contract(ep, &mut recv_types, &mut typed); }
    }
    if hc != 0 && typed != 0 {
        for index in 0..hc {
            let info = unsafe { msg.cast::<u8>().add(size_of::<MessageHeader>() +
                out_len as usize + index as usize * size_of::<HandleInfo>()) };
            let ty = unsafe { info.add(size_of::<usize>()).cast::<u16>().read_unaligned() } as u32;
            if ty >= 32 || recv_types & (1u32 << ty) == 0 {
                return -23; // TYPE_MISMATCH
            }
        }
    }

    let next = unsafe { (*header).next.cast::<c_void>() };
    unsafe {
        view.head.write(next);
        if next.is_null() { view.tail.write(ptr::null_mut()); }
        view.count.write(view.count.read().wrapping_sub(1));
        view.total_data.write(view.total_data.read().wrapping_sub(out_len));
        lease.ops.wake_sender(ep);
        lease.unlock();
        lease.ops.copy_out(msg, data, handles);
        if !data_len.is_null() { data_len.write(out_len); }
        if !handle_count.is_null() { handle_count.write(hc); }
        lease.ops.trace(out_len, msg, data, hc);
        lease.ops.free_msg(msg); // transferred object refs now belong to receiver
        lease.release();
    }
    0
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_channel_recv_finish(ep: *mut c_void,
    data: *mut c_void, data_len: *mut u32, handles: *mut HandleInfo,
    handle_count: *mut u32) -> i64 {
    unsafe { recv_finish_with(&mut KernelRecvOps, ep, data, data_len, handles, handle_count) }
}

#[no_mangle]
pub unsafe extern "C" fn a20_rust_channel_recv_abort(ep: *mut c_void) {
    drop(RecvLease::locked(&mut KernelRecvOps, ep));
}

/// Send one message, retaining one backing reference per transferred handle
/// until the queued message is received or discarded.
///
/// # Safety
/// `ep` must remain a valid channel endpoint for the call. `data` and
/// `handles` are kernel pointers to the indicated lengths.
#[no_mangle]
pub unsafe extern "C" fn a20_rust_channel_send_dwc(
    ep: *mut c_void,
    data: *const c_void,
    data_len: u32,
    handles: *const HandleInfo,
    handle_count: u32,
    _sender_ht: *mut c_void,
    flags: u32,
    defer_wake: i32,
) -> i64 {
    let _ = _sender_ht;
    unsafe { send_with(&mut KernelOps, ep, data, data_len, handles,
                       handle_count, flags, defer_wake) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::boxed::Box;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::Mutex;
    use std::vec::Vec;

    static CLOSE_LOG: Mutex<Vec<(u8, usize)>> = Mutex::new(Vec::new());
    static HOOK_REFS: AtomicUsize = AtomicUsize::new(0);
    static HOOK_CALLS: AtomicUsize = AtomicUsize::new(0);
    static HOOK_PUTS: AtomicUsize = AtomicUsize::new(0);

    #[repr(C)]
    struct CloseMockEndpoint {
        id: usize,
        peer: *mut CloseMockEndpoint,
        peer_closed: i32,
        hook: Option<CloseHook>,
        ctx_ref: Option<CloseHook>,
        ctx_put: Option<CloseHook>,
        hook_ctx: *mut c_void,
        message_head: *mut MessageHeader,
        refs: i32,
    }

    unsafe extern "C" fn close_test_ref(_ctx: *mut c_void) {
        HOOK_REFS.fetch_add(1, Ordering::SeqCst);
        CLOSE_LOG.lock().unwrap().push((9, 0));
    }
    unsafe extern "C" fn close_test_hook(_ctx: *mut c_void) {
        HOOK_CALLS.fetch_add(1, Ordering::SeqCst);
        CLOSE_LOG.lock().unwrap().push((10, 0));
    }
    unsafe extern "C" fn close_test_put(_ctx: *mut c_void) {
        HOOK_PUTS.fetch_add(1, Ordering::SeqCst);
        CLOSE_LOG.lock().unwrap().push((11, 0));
    }

    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_lock(ep: *mut c_void, locks: *mut CloseLocks) {
        let ep = ep.cast::<CloseMockEndpoint>();
        let peer = unsafe { (*ep).peer };
        unsafe { locks.write(CloseLocks { ep: ep.cast(), peer: peer.cast(), peer_locked: i32::from(!peer.is_null()) }); }
        CLOSE_LOG.lock().unwrap().push((1, unsafe { (*ep).id }));
    }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_unlock_peer(locks: *mut CloseLocks) {
        let locks = unsafe { &mut *locks };
        if locks.peer_locked != 0 {
            locks.peer_locked = 0;
            CLOSE_LOG.lock().unwrap().push((2, 0));
        }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_unlock(locks: *mut CloseLocks) {
        unsafe { a20_channel_close_unlock_peer(locks); }
        CLOSE_LOG.lock().unwrap().push((3, 0));
    }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_view(locks: *mut CloseLocks, out: *mut CloseView) {
        let locks = unsafe { &*locks };
        let ep = locks.ep.cast::<CloseMockEndpoint>();
        let peer = locks.peer.cast::<CloseMockEndpoint>();
        unsafe { out.write(CloseView {
            peer: peer.cast(),
            peer_link: core::ptr::addr_of_mut!((*ep).peer).cast(),
            peer_backlink: if peer.is_null() { ptr::null_mut() } else { core::ptr::addr_of_mut!((*peer).peer).cast() },
            peer_closed: if peer.is_null() { ptr::null_mut() } else { core::ptr::addr_of_mut!((*peer).peer_closed) },
            hook: if peer.is_null() { ptr::null_mut() } else { core::ptr::addr_of_mut!((*peer).hook) },
            ctx_ref: if peer.is_null() { ptr::null_mut() } else { core::ptr::addr_of_mut!((*peer).ctx_ref) },
            ctx_put: if peer.is_null() { ptr::null_mut() } else { core::ptr::addr_of_mut!((*peer).ctx_put) },
            hook_ctx: if peer.is_null() { ptr::null_mut() } else { core::ptr::addr_of_mut!((*peer).hook_ctx) },
            message_head: core::ptr::addr_of_mut!((*ep).message_head),
        }); }
    }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_stats_dec() { CLOSE_LOG.lock().unwrap().push((4, 0)); }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_wake(ep: *mut c_void) {
        CLOSE_LOG.lock().unwrap().push((5, unsafe { (*ep.cast::<CloseMockEndpoint>()).id }));
    }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_event(ep: *mut c_void, event: u32) {
        CLOSE_LOG.lock().unwrap().push((event as u8, unsafe { (*ep.cast::<CloseMockEndpoint>()).id }));
    }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_destroy_watches(ep: *mut c_void) {
        CLOSE_LOG.lock().unwrap().push((6, unsafe { (*ep.cast::<CloseMockEndpoint>()).id }));
    }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_free_msg(msg: *mut MessageHeader) {
        CLOSE_LOG.lock().unwrap().push((7, 0));
        drop(unsafe { Box::from_raw(msg) });
    }
    #[no_mangle]
    unsafe extern "C" fn a20_channel_close_free_ep(ep: *mut c_void) {
        assert_eq!(unsafe { (*ep.cast::<CloseMockEndpoint>()).refs }, 0,
                   "Rust finalizer must receive an endpoint whose last ref was consumed by C");
        CLOSE_LOG.lock().unwrap().push((8, unsafe { (*ep.cast::<CloseMockEndpoint>()).id }));
        drop(unsafe { Box::from_raw(ep.cast::<CloseMockEndpoint>()) });
    }

    struct Mock {
        alloc_ok: bool,
        max_data: u32,
        max_handles: u32,
        send_types: u32,
        typed: i32,
        queue_count: u32,
        queue_cap: u32,
        head: *mut c_void,
        tail: *mut c_void,
        total: u32,
        closed: i32,
        notifications: u32,
        msg_header: MessageHeader,
        old_tail_header: MessageHeader,
        refs: i32,
        puts: i32,
        frees: i32,
        waits: i32,
        endpoint_refs: i32,
        wait_result: i64,
    }

    impl SendOps for Mock {
        unsafe fn limits(&mut self, _ep: *mut c_void, max_data: &mut u32,
                         max_handles: &mut u32, types: &mut u32, typed: &mut i32) {
            *max_data = self.max_data; *max_handles = self.max_handles;
            *types = self.send_types; *typed = self.typed;
        }
        unsafe fn hold(&mut self, _ep: *mut c_void) { self.endpoint_refs += 1; }
        unsafe fn put_ep(&mut self, _ep: *mut c_void) { self.endpoint_refs -= 1; }
        unsafe fn ref_object(&mut self, _object: *mut c_void, _ty: u16) { self.refs += 1; }
        unsafe fn put_object(&mut self, _object: *mut c_void, _ty: u16) { self.puts += 1; }
        unsafe fn alloc(&mut self, _data: *const c_void, len: u32,
                        _handles: *const HandleInfo, count: u32) -> *mut c_void {
            if self.alloc_ok {
                self.msg_header = MessageHeader { data_len: len, handle_count: count,
                                                 next: ptr::null_mut() };
                ptr::addr_of_mut!(self.msg_header).cast()
            } else { ptr::null_mut() }
        }
        unsafe fn lock_peer(&mut self, _ep: *mut c_void) -> *mut c_void { 8usize as *mut c_void }
        unsafe fn unlock_peer(&mut self, _peer: *mut c_void) {}
        unsafe fn queue_view(&mut self, _peer: *mut c_void, out: *mut QueueView) {
            unsafe { out.write(QueueView {
                head: ptr::addr_of_mut!(self.head), tail: ptr::addr_of_mut!(self.tail),
                count: ptr::addr_of_mut!(self.queue_count), capacity: ptr::addr_of!(self.queue_cap),
                total_data: ptr::addr_of_mut!(self.total), peer_closed: ptr::addr_of!(self.closed),
            }); }
        }
        unsafe fn notify(&mut self, _peer: *mut c_void, _defer: i32) { self.notifications += 1; }
        unsafe fn wait_space(&mut self, _ep: *mut c_void) -> i64 {
            self.waits += 1;
            if self.wait_result == 0 && self.queue_count != 0 { self.queue_count -= 1; }
            self.wait_result
        }
        unsafe fn free(&mut self, _msg: *mut c_void) { self.frees += 1; }
        unsafe fn trace(&mut self, _op: u32, _len: u32, _msg: *mut c_void, _meta: u32) {}
    }

    fn mock() -> Mock {
        Mock { alloc_ok: true, max_data: 0, max_handles: 0, send_types: 0,
               typed: 0, queue_count: 0, queue_cap: 1, head: ptr::null_mut(),
               tail: ptr::null_mut(), total: 0, closed: 0, notifications: 0,
               msg_header: MessageHeader { data_len: 0, handle_count: 0, next: ptr::null_mut() },
               old_tail_header: MessageHeader { data_len: 0, handle_count: 0, next: ptr::null_mut() },
               refs: 0, puts: 0, frees: 0,
               waits: 0, endpoint_refs: 0, wait_result: 0 }
    }

    #[test]
    fn final_release_unlinks_peer_and_releases_hook_queue_and_endpoint_once() {
        CLOSE_LOG.lock().unwrap().clear();
        HOOK_REFS.store(0, Ordering::SeqCst);
        HOOK_CALLS.store(0, Ordering::SeqCst);
        HOOK_PUTS.store(0, Ordering::SeqCst);
        let ep = Box::new(CloseMockEndpoint {
            id: 1, peer: ptr::null_mut(), peer_closed: 0, hook: None,
            ctx_ref: None, ctx_put: None, hook_ctx: ptr::null_mut(),
            message_head: Box::into_raw(Box::new(MessageHeader { data_len: 9, handle_count: 0, next: ptr::null_mut() })), refs: 0,
        });
        let mut peer = Box::new(CloseMockEndpoint {
            id: 2, peer: ptr::null_mut(), peer_closed: 0, hook: Some(close_test_hook),
            ctx_ref: Some(close_test_ref), ctx_put: Some(close_test_put), hook_ctx: 1usize as *mut c_void,
            message_head: ptr::null_mut(), refs: 1,
        });
        let ep_ptr = Box::into_raw(ep);
        let peer_ptr = &mut *peer as *mut CloseMockEndpoint;
        unsafe { (*ep_ptr).peer = peer_ptr; }
        peer.peer = ep_ptr;
        unsafe { a20_rust_channel_ep_finalize(ep_ptr.cast()); }

        assert_eq!(peer.peer_closed, 1);
        assert!(peer.peer.is_null());
        assert_eq!(HOOK_REFS.load(Ordering::SeqCst), 1);
        assert_eq!(HOOK_CALLS.load(Ordering::SeqCst), 1);
        assert_eq!(HOOK_PUTS.load(Ordering::SeqCst), 1);
        let log = CLOSE_LOG.lock().unwrap();
        assert_eq!(log.iter().filter(|(event, _)| *event == 7).count(), 1);
        assert_eq!(log.iter().filter(|(event, id)| *event == 8 && *id == 1).count(), 1);
        assert_eq!(log.iter().filter(|(event, id)| *event == EVENT_PEER_CLOSED as u8 && *id == 2).count(), 1);
        assert_eq!(log.iter().filter(|(event, id)| *event == EVENT_CLOSED as u8 && *id == 1).count(), 1);
        assert_eq!(log.iter().filter(|(event, id)| *event == 6 && *id == 1).count(), 1);
        let index_of = |entry: (u8, usize)| log.iter().position(|item| *item == entry).unwrap();
        assert!(index_of((3, 0)) < index_of((10, 0)));
        assert!(index_of((10, 0)) < index_of((11, 0)));
        drop(log);
        drop(peer);
    }

    #[repr(align(8))]
    struct MessageStorage([u8; 128]);

    struct RecvMock {
        storage: MessageStorage,
        head: *mut c_void,
        tail: *mut c_void,
        count: u32,
        capacity: u32,
        total: u32,
        closed: i32,
        types: u32,
        typed: i32,
        unlocks: u32,
        puts: u32,
        frees: u32,
        wakes: u32,
        copies: u32,
        locked: bool,
        refs: i32,
        order: u32,
        unlock_order: u32,
        free_refs_order: u32,
        put_order: u32,
    }

    impl RecvMock {
        fn new() -> Self {
            Self {
                storage: MessageStorage([0; 128]), head: ptr::null_mut(),
                tail: ptr::null_mut(), count: 1, capacity: 1, total: 0,
                closed: 0, types: 1 << 7, typed: 1, unlocks: 0, puts: 0,
                frees: 0, wakes: 0, copies: 0, locked: true, refs: 1,
                order: 0, unlock_order: 0, free_refs_order: 0, put_order: 0,
            }
        }
        fn init_msg(&mut self, len: u32, handles: u32) {
            self.head = self.storage.0.as_mut_ptr().cast();
            self.tail = self.head;
            self.total = len;
            unsafe {
                self.head.cast::<MessageHeader>().write(MessageHeader {
                    data_len: len, handle_count: handles, next: ptr::null_mut(),
                });
                for i in 0..handles as usize {
                    self.head.cast::<u8>().add(size_of::<MessageHeader>() + len as usize +
                        i * size_of::<HandleInfo>() + size_of::<usize>())
                        .cast::<u16>().write_unaligned(7);
                }
            }
        }
    }

    impl RecvOps for RecvMock {
        unsafe fn queue_view(&mut self, _ep: *mut c_void, out: *mut QueueView) {
            unsafe { out.write(QueueView {
                head: ptr::addr_of_mut!(self.head), tail: ptr::addr_of_mut!(self.tail),
                count: ptr::addr_of_mut!(self.count), capacity: ptr::addr_of!(self.capacity),
                total_data: ptr::addr_of_mut!(self.total), peer_closed: ptr::addr_of!(self.closed),
            }); }
        }
        unsafe fn lock(&mut self, _ep: *mut c_void) { assert!(!self.locked); self.locked = true; }
        unsafe fn hold_ep(&mut self, _ep: *mut c_void) { self.refs += 1; }
        unsafe fn wait(&mut self, _ep: *mut c_void) -> i64 { 0 }
        unsafe fn free_msg_refs(&mut self, _msg: *mut c_void) {
            assert!(!self.locked, "message refs released while endpoint lock held");
            self.order += 1;
            self.free_refs_order = self.order;
            self.frees += 1;
        }
        unsafe fn trace_begin(&mut self, _len: u32, _msg: *mut c_void, _count: u32) {}
        unsafe fn contract(&mut self, _ep: *mut c_void, types: &mut u32, typed: &mut i32) {
            *types = self.types; *typed = self.typed;
        }
        unsafe fn copy_out(&mut self, _msg: *mut c_void, _data: *mut c_void,
                           _handles: *mut HandleInfo) { self.copies += 1; }
        unsafe fn wake_sender(&mut self, _ep: *mut c_void) { self.wakes += 1; }
        unsafe fn unlock(&mut self, _ep: *mut c_void) {
            assert!(self.locked); self.locked = false; self.unlocks += 1;
            self.order += 1;
            self.unlock_order = self.order;
        }
        unsafe fn put_ep(&mut self, _ep: *mut c_void) {
            assert!(!self.locked, "endpoint pin released while lock held");
            self.refs -= 1; self.puts += 1;
            self.order += 1;
            self.put_order = self.order;
        }
        unsafe fn free_msg(&mut self, _msg: *mut c_void) { self.frees += 1; }
        unsafe fn trace(&mut self, _len: u32, _msg: *mut c_void, _data: *mut c_void,
                        _handles: u32) {}
    }

    #[test]
    fn queue_full_retries_without_realloc_and_transfers_refs_on_success() {
        let mut ops = mock(); ops.queue_count = 1;
        let msg_ptr = ptr::addr_of_mut!(ops.msg_header).cast::<c_void>();
        let handles = [HandleInfo { object: 1usize as *mut c_void, ty: 7, pad: 0,
            transfer_rights: 0, expiry_tick: 0, remaining_ops: 0,
            temporal_flags: 0, security_label: 0, pad2: [0; 7] }];
        let result = unsafe { send_with(&mut ops, 1usize as *mut c_void,
            ptr::null(), 0, handles.as_ptr(), 1, 0, 0) };
        assert_eq!(result, 0);
        assert_eq!((ops.refs, ops.puts, ops.frees, ops.waits, ops.endpoint_refs), (1, 0, 0, 1, 0));
        assert_eq!((ops.head, ops.tail, ops.queue_count, ops.notifications),
                   (msg_ptr, msg_ptr, 1, 1));

        let mut append = mock();
        let old_tail = ptr::addr_of_mut!(append.old_tail_header).cast::<c_void>();
        let new_msg = ptr::addr_of_mut!(append.msg_header).cast::<c_void>();
        append.head = old_tail;
        append.tail = old_tail;
        append.queue_count = 1;
        append.queue_cap = 2;
        append.msg_header.data_len = 13;
        assert_eq!(unsafe { queue_once(&mut append, 1usize as *mut c_void,
                                      new_msg, 0) }, 1);
        assert_eq!((append.head, append.tail, append.old_tail_header.next.cast::<c_void>(),
                    append.queue_count, append.total),
                   (old_tail, new_msg, new_msg, 2, 13));
    }

    #[test]
    fn allocation_and_nonblocking_failures_release_each_reference_once() {
        let handles = [HandleInfo { object: 1usize as *mut c_void, ty: 7, pad: 0,
            transfer_rights: 0, expiry_tick: 0, remaining_ops: 0,
            temporal_flags: 0, security_label: 0, pad2: [0; 7] }];
        let mut oom = mock(); oom.alloc_ok = false;
        let r = unsafe { send_with(&mut oom, 1usize as *mut c_void,
            ptr::null(), 0, handles.as_ptr(), 1, 0, 0) };
        assert_eq!(r, -6);
        assert_eq!((oom.refs, oom.puts, oom.frees, oom.endpoint_refs), (1, 1, 0, 0));

        let mut full = mock(); full.queue_count = full.queue_cap;
        let r = unsafe { send_with(&mut full, 1usize as *mut c_void,
            ptr::null(), 0, handles.as_ptr(), 1, NONBLOCK, 0) };
        assert_eq!(r, -ERR_WOULD_BLOCK);
        assert_eq!((full.refs, full.puts, full.frees, full.waits, full.endpoint_refs), (1, 1, 1, 0, 0));
    }

    #[test]
    fn type_contract_and_interrupted_wait_leave_no_reference_behind() {
        let handles = [HandleInfo { object: 1usize as *mut c_void, ty: 7, pad: 0,
            transfer_rights: 0, expiry_tick: 0, remaining_ops: 0,
            temporal_flags: 0, security_label: 0, pad2: [0; 7] }];
        let mut mismatch = mock(); mismatch.typed = 1; mismatch.send_types = 1 << 6;
        let r = unsafe { send_with(&mut mismatch, 1usize as *mut c_void,
            ptr::null(), 0, handles.as_ptr(), 1, 0, 0) };
        assert_eq!(r, -23);
        assert_eq!((mismatch.refs, mismatch.puts, mismatch.frees, mismatch.endpoint_refs), (0, 0, 0, 0));

        let mut interrupted = mock(); interrupted.queue_count = interrupted.queue_cap;
        interrupted.wait_result = -ERR_INTERRUPTED;
        let r = unsafe { send_with(&mut interrupted, 1usize as *mut c_void,
            ptr::null(), 0, handles.as_ptr(), 1, 0, 0) };
        assert_eq!(r, -ERR_INTERRUPTED);
        assert_eq!((interrupted.refs, interrupted.puts, interrupted.frees,
                    interrupted.waits, interrupted.endpoint_refs), (1, 1, 1, 1, 0));
    }

    #[test]
    fn receive_commit_and_no_space_preserve_queue_ownership() {
        let mut ops = RecvMock::new();
        ops.init_msg(4, 0);
        let ep = 8usize as *mut c_void;
        let mut data = [0u8; 8];
        let mut len = 8;
        let mut hcount = 0;
        let r = unsafe { recv_finish_with(&mut ops, ep, data.as_mut_ptr().cast(),
                    &mut len, ptr::null_mut(), &mut hcount) };
        assert_eq!(r, 0);
        assert_eq!((ops.head, ops.tail, ops.count, ops.total),
                   (ptr::null_mut(), ptr::null_mut(), 0, 0));
        assert_eq!((ops.unlocks, ops.puts, ops.frees, ops.wakes, ops.copies), (1, 1, 1, 1, 1));
        assert_eq!((len, hcount), (4, 0));

        let mut no_space = RecvMock::new();
        no_space.init_msg(4, 0);
        let mut short_len = 1;
        let r = unsafe { recv_finish_with(&mut no_space, ep, data.as_mut_ptr().cast(),
                    &mut short_len, ptr::null_mut(), &mut hcount) };
        assert_eq!(r, -13);
        assert_eq!((no_space.head.is_null(), no_space.count, no_space.total,
                    no_space.frees, no_space.wakes), (false, 1, 4, 0, 0));
    }

    #[test]
    fn receive_begin_retains_locked_message_and_discards_forbidden_head() {
        let ep = 8usize as *mut c_void;
        let mut ready = RecvMock::new();
        ready.init_msg(4, 1);
        ready.refs = 0;
        ready.locked = false;
        let (mut len, mut hc) = (0, 0);
        assert_eq!(unsafe { recv_begin_with(&mut ready, ep, 0, &mut len, &mut hc) }, 0);
        assert_eq!((ready.locked, ready.refs, ready.count, len, hc), (true, 1, 1, 4, 1));
        assert_eq!(unsafe { recv_finish_with(&mut ready, ep, ptr::null_mut(), ptr::null_mut(),
                                            ptr::null_mut(), ptr::null_mut()) }, -13);
        assert_eq!((ready.locked, ready.refs, ready.count, ready.frees), (false, 0, 1, 0));

        let mut forbidden = RecvMock::new();
        forbidden.init_msg(0, 1);
        forbidden.refs = 0;
        forbidden.locked = false;
        forbidden.types = 0;
        assert_eq!(unsafe { recv_begin_with(&mut forbidden, ep, 0, &mut len, &mut hc) }, -23);
        assert_eq!((forbidden.locked, forbidden.refs, forbidden.count,
                    forbidden.frees, forbidden.wakes), (false, 0, 0, 1, 1));
        assert_eq!((forbidden.unlock_order, forbidden.free_refs_order,
                    forbidden.put_order), (1, 2, 3));
    }
}
