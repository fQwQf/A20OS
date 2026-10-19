//! UXFS RPC engine: request admission, deadline/retry sequencing, stale-reply
//! filtering and shutdown admission (Rust-owned split for `uxfs.c`).
//!
//! The C adapter keeps the channel endpoint, `req_lock`, the sleeping/timer
//! bridge, the message buffer allocation and the actual `a20_channel_send` /
//! `a20_channel_recv_begin`/`finish` calls.  This module owns the RPC state
//! machine and its ordering: lock acquisition with a monotonic deadline,
//! endpoint/shutdown admission (INIT/SHUTDOWN/CLOSE survive shutdown), the
//! send retry on a full peer queue, the receive loop with nonblocking polls and
//! deadline expiry, stale/out-of-order reply rejection by `(magic, req_id,
//! opcode)`, response/payload bounds checking and the errno mapping.
//!
//! Every operation reaches the outside only through the callback table, so the
//! host test drives the exact production loop with a scripted channel.

#![cfg_attr(not(test), no_std)]

use core::ffi::{c_int, c_void};

pub const UFS_REQ_MAGIC: u32 = 0x5546_5352;
pub const UFS_RESP_MAGIC: u32 = 0x5546_5350;
pub const UFS_OP_INIT: u32 = 1;
pub const UFS_OP_LOOKUP: u32 = 2;
pub const UFS_OP_CLOSE: u32 = 16;
pub const UFS_OP_SHUTDOWN: u32 = 18;

pub const EIO: c_int = 5;
pub const ESHUTDOWN: c_int = 108;
pub const ETIMEDOUT: c_int = 110;
pub const EINVAL: c_int = 22;

/// `ufs_req_hdr_t`, asserted against the C header by `uxfs.c`.
#[repr(C)]
pub struct UxfsReqHdr {
    pub magic: u32,
    pub version: u32,
    pub opcode: u32,
    pub req_id: u32,
    pub ino: u64,
    pub arg0: u64,
    pub arg1: u64,
    pub name_len: u32,
    pub payload_len: u32,
}

/// `ufs_resp_hdr_t`.
#[repr(C)]
pub struct UxfsRespHdr {
    pub magic: u32,
    pub opcode: u32,
    pub req_id: u32,
    pub status: i32,
    pub out0: u64,
    pub out1: u64,
    pub out2: u64,
    pub payload_len: u32,
    pub _pad: u32,
}

/// Send result codes returned by the C bridge.
pub const SEND_OK: c_int = 0;
pub const SEND_WOULD_BLOCK: c_int = 1;
pub const SEND_ERROR: c_int = 2;
/// Receive result codes: >= 0 is the received length.
pub const RECV_WOULD_BLOCK: c_int = -1;
pub const RECV_ERROR: c_int = -2;
pub const RECV_INVALID: c_int = -3;

pub type NowFn = extern "C" fn() -> u64;
pub type SleepTickFn = extern "C" fn();
pub type TryLockFn = extern "C" fn(*mut c_void) -> c_int;
pub type UnlockFn = extern "C" fn(*mut c_void);
/// `(ctx, msg, len, deadline_ns, nonblock) -> SEND_*`.
pub type SendFn = extern "C" fn(*mut c_void, *const u8, u32, u64, c_int) -> c_int;
/// `(ctx, buf, cap, deadline_ns, nonblock, out_len) -> length or RECV_*`.
pub type RecvFn = extern "C" fn(*mut c_void, *mut u8, u32, u64, c_int, *mut u32) -> c_int;
pub type HasEpFn = extern "C" fn(*mut c_void) -> c_int;
pub type ShutdownStartedFn = extern "C" fn(*mut c_void) -> c_int;

/// Narrow mechanical bridge table.  All entries are required.
#[repr(C)]
pub struct UxfsRpcOps {
    pub now_ns: Option<NowFn>,
    pub sleep_tick: Option<SleepTickFn>,
    pub try_lock: Option<TryLockFn>,
    pub unlock: Option<UnlockFn>,
    pub send: Option<SendFn>,
    pub recv: Option<RecvFn>,
    pub has_ep: Option<HasEpFn>,
    pub shutdown_started: Option<ShutdownStartedFn>,
}

struct Ops<'a> {
    now_ns: &'a NowFn,
    sleep_tick: &'a SleepTickFn,
    try_lock: &'a TryLockFn,
    unlock: &'a UnlockFn,
    send: &'a SendFn,
    recv: &'a RecvFn,
    has_ep: &'a HasEpFn,
    shutdown_started: &'a ShutdownStartedFn,
}

impl<'a> Ops<'a> {
    unsafe fn load(t: *const UxfsRpcOps) -> Option<Ops<'a>> {
        if t.is_null() {
            return None;
        }
        // SAFETY: caller passes a live table for the duration of the call.
        let t = unsafe { &*t };
        Some(Ops {
            now_ns: t.now_ns.as_ref()?,
            sleep_tick: t.sleep_tick.as_ref()?,
            try_lock: t.try_lock.as_ref()?,
            unlock: t.unlock.as_ref()?,
            send: t.send.as_ref()?,
            recv: t.recv.as_ref()?,
            has_ep: t.has_ep.as_ref()?,
            shutdown_started: t.shutdown_started.as_ref()?,
        })
    }
}

#[inline]
fn admitted(opcode: u32, shutdown_started: bool) -> bool {
    !shutdown_started || opcode == UFS_OP_INIT || opcode == UFS_OP_SHUTDOWN || opcode == UFS_OP_CLOSE
}

#[inline]
fn reply_matches(cand: &UxfsRespHdr, req: &UxfsReqHdr) -> bool {
    cand.magic == UFS_RESP_MAGIC && cand.req_id == req.req_id && cand.opcode == req.opcode
}

/// Run one RPC under the mount's request lock.
///
/// `msg` is the already-encoded request frame; `rx`/`rx_cap` is the caller's
/// receive buffer.  On success the response header is copied to `resp` and the
/// response payload to `resp_payload` (at most `resp_cap`), returning
/// `resp.status`.  Negative returns use the Linux errno values above.
#[no_mangle]
pub unsafe extern "C" fn a20_uxfs_rpc_run(
    ctx: *mut c_void,
    table: *const UxfsRpcOps,
    req: *const UxfsReqHdr,
    msg: *const u8,
    msg_len: u32,
    rx: *mut u8,
    rx_cap: u32,
    resp: *mut UxfsRespHdr,
    resp_payload: *mut u8,
    resp_cap: u32,
    resp_payload_len: *mut u32,
    deadline_ns: u64,
    request_sent: *mut c_int,
) -> c_int {
    if req.is_null() || msg.is_null() || resp.is_null() || rx.is_null() {
        return -EINVAL;
    }
    let ops = match unsafe { Ops::load(table) } {
        Some(o) => o,
        None => return -EINVAL,
    };
    // SAFETY: req/resp point at live kernel stack objects for this call.
    let req = unsafe { &*req };
    if !request_sent.is_null() {
        unsafe { *request_sent = 0 };
    }

    // 1. Serialise this mount's send/wait pair with a deadline.
    loop {
        if (ops.try_lock)(ctx) != 0 {
            break;
        }
        if deadline_ns != 0 && (ops.now_ns)() >= deadline_ns {
            return -ETIMEDOUT;
        }
        (ops.sleep_tick)();
    }

    let result = unsafe {
        rpc_locked(ctx, &ops, req, msg, msg_len, rx, rx_cap, resp,
                   resp_payload, resp_cap, resp_payload_len, deadline_ns,
                   request_sent)
    };
    (ops.unlock)(ctx);
    result
}

unsafe fn rpc_locked(
    ctx: *mut c_void,
    ops: &Ops<'_>,
    req: &UxfsReqHdr,
    msg: *const u8,
    msg_len: u32,
    rx: *mut u8,
    rx_cap: u32,
    resp: *mut UxfsRespHdr,
    resp_payload: *mut u8,
    resp_cap: u32,
    resp_payload_len: *mut u32,
    deadline_ns: u64,
    request_sent: *mut c_int,
) -> c_int {
    let has_ep = (ops.has_ep)(ctx) != 0;
    if !has_ep {
        return -ESHUTDOWN;
    }
    let shutdown = (ops.shutdown_started)(ctx) != 0;
    if !admitted(req.opcode, shutdown) {
        return -ESHUTDOWN;
    }

    // 2. Send, retrying a full peer queue until the deadline.  A bounded RPC
    //    uses nonblocking sends so the receive deadline starts even when the
    //    queue never drains.
    let nonblock = if deadline_ns != 0 { 1 } else { 0 };
    let send_rc = loop {
        let r = (ops.send)(ctx, msg, msg_len, deadline_ns, nonblock);
        if r == SEND_OK {
            break SEND_OK;
        }
        if r == SEND_WOULD_BLOCK && deadline_ns != 0 {
            if (ops.now_ns)() >= deadline_ns {
                break SEND_ERROR;
            }
            (ops.sleep_tick)();
            continue;
        }
        break SEND_ERROR;
    };
    if send_rc != SEND_OK {
        if deadline_ns != 0 && (ops.now_ns)() >= deadline_ns {
            return -ETIMEDOUT;
        }
        return -EIO;
    }
    if !request_sent.is_null() {
        unsafe { *request_sent = 1 };
    }

    // 3. Receive, dropping stale/out-of-order replies until the matching one
    //    arrives or the deadline expires.
    loop {
        let mut in_len = rx_cap;
        let recv_nonblock = if deadline_ns != 0 {
            if (ops.now_ns)() >= deadline_ns {
                return -ETIMEDOUT;
            }
            1
        } else {
            0
        };
        let r = (ops.recv)(ctx, rx, rx_cap, deadline_ns, recv_nonblock, &mut in_len);
        if r == RECV_WOULD_BLOCK {
            if deadline_ns != 0 {
                (ops.sleep_tick)();
                continue;
            }
            return -EIO;
        }
        if r == RECV_INVALID {
            return -EIO;
        }
        if r < 0 {
            if deadline_ns != 0 && (ops.now_ns)() >= deadline_ns {
                return -ETIMEDOUT;
            }
            return -EIO;
        }
        if (in_len as usize) < core::mem::size_of::<UxfsRespHdr>() {
            continue;
        }
        // SAFETY: rx holds at least one response header.
        let cand = unsafe { &*(rx as *const UxfsRespHdr) };
        if !reply_matches(cand, req) {
            continue;
        }
        let plen = cand.payload_len;
        if plen > resp_cap || (plen as usize) > (in_len as usize - core::mem::size_of::<UxfsRespHdr>()) {
            return -EIO;
        }
        // SAFETY: `resp` is the caller's header slot; payload follows the
        // received header and fits both bounds checked above.
        unsafe {
            core::ptr::copy_nonoverlapping(rx as *const UxfsRespHdr, resp, 1);
            if plen != 0 && !resp_payload.is_null() {
                core::ptr::copy_nonoverlapping(
                    rx.add(core::mem::size_of::<UxfsRespHdr>()),
                    resp_payload,
                    plen as usize,
                );
            }
            if !resp_payload_len.is_null() {
                *resp_payload_len = plen;
            }
            return (*resp).status;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::cell::RefCell;

    thread_local! {
        static WORLD: RefCell<World> = RefCell::new(World::new());
    }

    #[derive(Default)]
    struct World {
        locked: bool,
        has_ep: bool,
        shutdown: bool,
        now: u64,
        sleep_cost: u64,
        send_left: u32,
        send_would_block: u32,
        rx: Vec<u8>,
        rx_mode: u8, // 0 ok, 1 would_block, 2 error
        recv_attempts: u32,
        sent: bool,
    }

    impl World {
        fn new() -> Self {
            Self { locked: false, has_ep: true, shutdown: false, now: 0, sleep_cost: 1_000_000, send_left: 0, send_would_block: 0, rx: Vec::new(), rx_mode: 0, recv_attempts: 0, sent: false }
        }
    }

    extern "C" fn now_cb() -> u64 { WORLD.with(|w| w.borrow().now) }
    extern "C" fn sleep_cb() { WORLD.with(|w| { let c = w.borrow().sleep_cost; let mut b = w.borrow_mut(); b.now += c; }); }
    extern "C" fn trylock_cb(_: *mut c_void) -> c_int { WORLD.with(|w| { let mut b = w.borrow_mut(); if b.locked { 0 } else { b.locked = true; 1 } }) }
    extern "C" fn unlock_cb(_: *mut c_void) { WORLD.with(|w| w.borrow_mut().locked = false); }
    extern "C" fn send_cb(_: *mut c_void, _: *const u8, _: u32, _: u64, _: c_int) -> c_int {
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            if b.send_would_block > 0 { b.send_would_block -= 1; return SEND_WOULD_BLOCK; }
            if b.send_left > 0 { b.send_left -= 1; return SEND_ERROR; }
            b.sent = true;
            SEND_OK
        })
    }
    extern "C" fn recv_cb(_: *mut c_void, buf: *mut u8, cap: u32, _: u64, _: c_int, out_len: *mut u32) -> c_int {
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            b.recv_attempts += 1;
            // Model the time a channel receive iteration actually takes so a
            // stream of stale frames cannot livelock the deadline test.
            b.now += 1_000_000;
            if b.rx_mode == 1 { return RECV_WOULD_BLOCK; }
            if b.rx_mode == 2 { return RECV_ERROR; }
            let n = core::cmp::min(b.rx.len(), cap as usize);
            unsafe { core::ptr::copy_nonoverlapping(b.rx.as_ptr(), buf, n); *out_len = n as u32; }
            n as c_int
        })
    }
    extern "C" fn has_ep_cb(_: *mut c_void) -> c_int { WORLD.with(|w| w.borrow().has_ep as c_int) }
    extern "C" fn shutdown_cb(_: *mut c_void) -> c_int { WORLD.with(|w| w.borrow().shutdown as c_int) }

    fn table() -> UxfsRpcOps {
        UxfsRpcOps {
            now_ns: Some(now_cb), sleep_tick: Some(sleep_cb),
            try_lock: Some(trylock_cb), unlock: Some(unlock_cb),
            send: Some(send_cb), recv: Some(recv_cb),
            has_ep: Some(has_ep_cb), shutdown_started: Some(shutdown_cb),
        }
    }

    fn req(op: u32, id: u32) -> UxfsReqHdr {
        UxfsReqHdr { magic: UFS_REQ_MAGIC, version: 1, opcode: op, req_id: id, ino: 1, arg0: 0, arg1: 0, name_len: 0, payload_len: 0 }
    }

    fn resp_frame(op: u32, id: u32, status: i32, payload: &[u8]) -> Vec<u8> {
        let h = UxfsRespHdr { magic: UFS_RESP_MAGIC, opcode: op, req_id: id, status, out0: 0, out1: 0, out2: 0, payload_len: payload.len() as u32, _pad: 0 };
        let mut v = unsafe { core::slice::from_raw_parts(&h as *const _ as *const u8, core::mem::size_of::<UxfsRespHdr>()) }.to_vec();
        v.extend_from_slice(payload);
        v
    }

    fn run(op: u32, id: u32, deadline: u64) -> (c_int, UxfsRespHdr, Vec<u8>, bool) {
        let r = req(op, id);
        let mut resp = UxfsRespHdr { magic: 0, opcode: 0, req_id: 0, status: 0, out0: 0, out1: 0, out2: 0, payload_len: 0, _pad: 0 };
        let mut payload = [0u8; 32];
        let mut plen = 0u32;
        let mut sent = 0;
        let msg = [0u8; 16];
        let t = table();
        let rc = unsafe {
            a20_uxfs_rpc_run(core::ptr::null_mut(), &t, &r, msg.as_ptr(), 16,
                vec![0u8; 64].as_mut_ptr(), 64, &mut resp, payload.as_mut_ptr(), 32,
                &mut plen, deadline, &mut sent)
        };
        (rc, resp, payload[..plen as usize].to_vec(), sent != 0)
    }

    #[test]
    fn matching_reply_with_payload() {
        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.rx = resp_frame(UFS_OP_INIT, 7, 0, b"abc"); });
        let (rc, r, p, sent) = run(UFS_OP_INIT, 7, 1000);
        assert_eq!(rc, 0);
        assert_eq!(p, b"abc");
        assert_eq!(r.req_id, 7);
        assert!(sent);
        WORLD.with(|w| assert!(!w.borrow().locked));
    }

    #[test]
    fn stale_replies_are_dropped() {
        WORLD.with(|w| {
            let mut b = w.borrow_mut();
            *b = World::new();
            let mut v = resp_frame(UFS_OP_INIT, 6, 0, b"");
            v.extend_from_slice(&resp_frame(UFS_OP_INIT, 7, -13, b""));
            b.rx = v;
        });
        // The fake recv returns the whole concatenation; the first header is
        // stale so the loop must retry and hit the same buffer again.  A real
        // channel consumes one frame per call, so emulate that by setting a
        // one-frame stale reply followed by the good one in two worlds.
        WORLD.with(|w| { w.borrow_mut().rx = resp_frame(UFS_OP_INIT, 6, 0, b""); });
        // No further frame: a bounded deadline must expire rather than accept.
        let (rc, _, _, sent) = run(UFS_OP_INIT, 7, 5_000_000);
        assert!(sent);
        assert_eq!(rc, -ETIMEDOUT);

        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.rx = resp_frame(UFS_OP_INIT, 7, -13, b""); });
        let (rc2, _, _, _) = run(UFS_OP_INIT, 7, 5_000_000);
        assert_eq!(rc2, -13);
    }

    #[test]
    fn shutdown_admission() {
        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.shutdown = true; });
        let (rc, _, _, sent) = run(UFS_OP_LOOKUP, 1, 1000);
        assert_eq!(rc, -ESHUTDOWN);
        assert!(!sent);
        WORLD.with(|w| { w.borrow_mut().rx = resp_frame(UFS_OP_CLOSE, 2, 0, b""); });
        let (rc2, _, _, _) = run(UFS_OP_CLOSE, 2, 1000);
        assert_eq!(rc2, 0);
    }

    #[test]
    fn no_endpoint_is_shutdown() {
        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.has_ep = false; });
        let (rc, _, _, _) = run(UFS_OP_INIT, 1, 1000);
        assert_eq!(rc, -ESHUTDOWN);
    }

    #[test]
    fn send_retry_and_deadline() {
        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.rx = resp_frame(UFS_OP_INIT, 1, 0, b""); b.send_would_block = 3; });
        let (rc, _, _, sent) = run(UFS_OP_INIT, 1, 100_000_000);
        assert_eq!(rc, 0);
        assert!(sent);

        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.send_would_block = 1000; b.sleep_cost = 2_000_000; });
        let (rc2, _, _, sent2) = run(UFS_OP_INIT, 1, 5_000_000);
        assert_eq!(rc2, -ETIMEDOUT);
        assert!(!sent2);
    }

    #[test]
    fn recv_would_block_until_deadline() {
        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.rx_mode = 1; b.sleep_cost = 2_000_000; });
        let (rc, _, _, _) = run(UFS_OP_INIT, 1, 5_000_000);
        assert_eq!(rc, -ETIMEDOUT);
    }

    #[test]
    fn lock_wait_respects_deadline() {
        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.locked = true; b.sleep_cost = 2_000_000; });
        let (rc, _, _, _) = run(UFS_OP_INIT, 1, 5_000_000);
        assert_eq!(rc, -ETIMEDOUT);
        assert!(WORLD.with(|w| w.borrow().locked));
    }

    #[test]
    fn oversized_payload_is_protocol_error() {
        WORLD.with(|w| { let mut b = w.borrow_mut(); *b = World::new(); b.rx = resp_frame(UFS_OP_INIT, 1, 0, &[0u8; 40]); });
        let (rc, _, _, _) = run(UFS_OP_INIT, 1, 1000);
        assert_eq!(rc, -EIO);
    }
}
