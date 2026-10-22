//! Rust-owned user filesystem service lifecycle and UFS request dispatch.
//! Filesystem algorithms and sector primitives remain in the C backend layer.
#![no_std]
#![no_main]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;

#[path = "ufsd_protocol.rs"]
mod protocol;
#[path = "fscompat/bcache_user.rs"]
mod bcache;
#[path = "jbd2_legacy_policy.rs"]
mod jbd2_legacy_policy;
#[path = "jbd2_txn.rs"]
mod jbd2_txn;
#[path = "jbd2_record.rs"]
mod jbd2_record;
#[path = "ext4_orphan.rs"]
mod ext4_orphan;
#[path = "../../kernel/fs/diskfs/ext4_extent_txn.rs"]
mod ext4_extent_txn;
#[path = "ufs_lifetime.rs"]
mod lifetime;
#[path = "ufsd_supervision.rs"]
mod supervision;
#[allow(dead_code)]
mod idl { include!(env!("A20_IDL_RUST_BINDINGS")); }

type Handle = u32;
const HANDLE_NULL: Handle = u32::MAX;
const ERR_WOULD_BLOCK: i64 = -18;
const ERR_INVALID: i32 = -22;
const ERR_IO: i32 = -5;
const ERR_BUSY: i64 = 16;
const EVENT_MESSAGE_READY: u64 = 1 << 8;
const EVENT_PEER_CLOSED: u64 = 1 << 9;
const FS_EVENT_TAG: u64 = 1;
const SERVICE_EVENT_TAG: u64 = 2;
const BACKEND_FAT: u32 = 1;
const BACKEND_VNFS: u32 = 2;
const ERR_NOTSUPP: i64 = -95;
#[cfg(ufsd_test_close_token_drop_ack_once)]
const TEST_CLOSE_REPLAY_SENTINEL: u64 = 0x52504c59; /* "RPLY" */

#[repr(C, align(8))]
struct MessageBuffer([u8; protocol::MAX_MESSAGE]);

unsafe extern "C" {
    fn a20_ufsd_startup(stdout: *mut Handle) -> i64;
    fn a20_ufsd_set_block_index(block_index: i32) -> i64;
    fn a20_ufsd_channel_create(endpoints: *mut Handle) -> i64;
    fn a20_ufsd_path_create(path: *const c_char, path_len: u32) -> i64;
    fn a20_ufsd_serve(endpoint: Handle, block_index: i32, path: *const c_char,
                       path_len: u32, flags: u32) -> i64;
    fn a20_ufsd_umount(path: *const c_char, path_len: u32) -> i64;
    fn a20_ufsd_capacity(block_index: i32, sectors: *mut u64) -> i64;
    fn a20_ufsd_monotonic_ns(out_ns: *mut u64) -> i64;
    fn a20_ufsd_event_queue_create(queue: *mut Handle) -> i64;
    fn a20_ufsd_event_watch(queue: Handle, target: Handle, mask: u64, tag: u64) -> i64;
    fn a20_ufsd_event_wait(queue: Handle) -> i64;
    fn a20_ufsd_recv(endpoint: Handle, bytes: *mut u8, capacity: u32,
                      length: *mut u32) -> i64;
    fn a20_ufsd_send(endpoint: Handle, response: *const protocol::Response) -> i64;
    fn a20_ufsd_send_bytes(endpoint: Handle, bytes: *const u8, length: u32) -> i64;
    fn a20_ufsd_close(handle: Handle) -> i64;
    fn a20_ufsd_log(handle: Handle, bytes: *const u8, length: u32);
    fn a20_ufsd_backend_mount(backend: u32, fstype: *const c_char) -> i64;
    fn a20_ufsd_backend_mount_readonly(backend: u32, fstype: *const c_char) -> i64;
    fn a20_ufsd_backend_unmount(backend: u32) -> i64;
    fn a20_ufsd_backend_lookup(backend: u32, ino: u64, name: *const c_char,
                               name_len: u32, response: *mut protocol::Response) -> i64;
    fn a20_ufsd_backend_getattr(backend: u32, ino: u64,
                                response: *mut protocol::Response) -> i64;
    fn a20_ufsd_backend_readdir(backend: u32, ino: u64, skip: u64,
                                response: *mut protocol::Response) -> i64;
    fn a20_ufsd_backend_create(backend: u32, ino: u64, name: *const c_char,
                               mode: u32, response: *mut protocol::Response) -> i64;
    fn a20_ufsd_backend_mkdir(backend: u32, ino: u64, name: *const c_char,
                              response: *mut protocol::Response) -> i64;
    fn a20_ufsd_backend_unlink(backend: u32, ino: u64, name: *const c_char) -> i64;
    fn a20_ufsd_backend_link(backend: u32, dir: u64, name: *const c_char,
                             target: u64) -> i64;
    fn a20_ufsd_backend_rmdir(backend: u32, ino: u64, name: *const c_char) -> i64;
    fn a20_ufsd_backend_truncate(backend: u32, ino: u64, size: u64) -> i64;
    fn a20_ufsd_backend_read(backend: u32, ino: u64, offset: u64, count: u32,
                             response: *mut protocol::Response) -> i64;
    fn a20_ufsd_backend_write(backend: u32, ino: u64, offset: u64,
                              bytes: *const u8, count: u32,
                              response: *mut protocol::Response) -> i64;
    fn a20_ufsd_backend_rename(backend: u32, old_dir: u64, old_name: *const c_char,
                               new_dir: u64, new_name: *const c_char,
                               response: *mut protocol::Response) -> i64;
    fn a20_ufsd_backend_sync(backend: u32) -> i64;
    fn a20_ufsd_backend_statfs(backend: u32, response: *mut protocol::Response);
    fn a20_ufsd_backend_has_inode(backend: u32, ino: u64) -> i64;
    fn a20_ufsd_backend_open(backend: u32, ino: u64) -> i64;
    fn a20_ufsd_backend_close(backend: u32, ino: u64) -> i64;
    fn a20_ufsd_backend_link_count(backend: u32, ino: u64, links: *mut u64) -> i64;
    fn a20_ufsd_backend_reap_unlinked(backend: u32, ino: u64) -> i64;
    fn a20_ufsd_backend_drop_inode(backend: u32, ino: u64) -> i64;
    fn a20_ufsd_payload_buffer() -> *mut u8;
    fn a20_ufsd_payload_capacity() -> u32;
}

struct OwnedHandle(Handle);

impl OwnedHandle {
    fn new(raw: Handle) -> Self { Self(raw) }
    fn raw(&self) -> Handle { self.0 }
    fn close_now(&mut self) -> i64 {
        if self.0 == HANDLE_NULL { return 0; }
        let raw = core::mem::replace(&mut self.0, HANDLE_NULL);
        unsafe { a20_ufsd_close(raw) }
    }
}

impl Drop for OwnedHandle {
    fn drop(&mut self) { let _ = self.close_now(); }
}

struct MountedFs {
    path: *const c_char,
    path_len: u32,
    active: bool,
}

impl Drop for MountedFs {
    fn drop(&mut self) {
        if self.active {
            unsafe { let _ = a20_ufsd_umount(self.path, self.path_len); }
            self.active = false;
        }
    }
}

fn log(handle: Handle, bytes: &[u8]) {
    unsafe { a20_ufsd_log(handle, bytes.as_ptr(), bytes.len() as u32) }
}

fn log_number(handle: Handle, prefix: &[u8], value: u32, suffix: &[u8]) {
    log(handle, prefix);
    let mut digits = [0u8; 10];
    let mut count = 0usize;
    let mut number = value;
    if number == 0 { unsafe { digits.as_mut_ptr().write(b'0') }; count = 1; }
    while number != 0 {
        unsafe { digits.as_mut_ptr().add(count).write(b'0' + (number % 10) as u8) };
        count += 1;
        number /= 10;
    }
    let mut encoded = [0u8; 10];
    for i in 0..count {
        unsafe { encoded.as_mut_ptr().add(i).write(digits.as_ptr().add(count - i - 1).read()); }
    }
    log(handle, unsafe { core::slice::from_raw_parts(encoded.as_ptr(), count) });
    log(handle, suffix);
}

#[inline]
fn diagnostic_log_number(handle: Handle, prefix: &[u8], value: u32, suffix: &[u8]) {
    #[cfg(ufsd_test_close_orphan)]
    log_number(handle, prefix, value, suffix);
    #[cfg(not(ufsd_test_close_orphan))]
    let _ = (handle, prefix, value, suffix);
}

#[inline]
fn diagnostic_log(handle: Handle, bytes: &[u8]) {
    #[cfg(ufsd_test_close_orphan)]
    log(handle, bytes);
    #[cfg(not(ufsd_test_close_orphan))]
    let _ = (handle, bytes);
}

unsafe fn c_bytes<'a>(pointer: *const c_char, max_len: usize) -> Option<&'a [u8]> {
    if pointer.is_null() { return None; }
    let bytes = pointer.cast::<u8>();
    for len in 0..max_len {
        if unsafe { *bytes.add(len) } == 0 {
            return Some(unsafe { core::slice::from_raw_parts(bytes, len) });
        }
    }
    None
}

fn decimal_arg(bytes: &[u8]) -> Option<i32> { protocol::parse_block_index(bytes) }

fn reap_and_drop(backend: u32, ino: u64) -> i64 {
    unsafe {
        let reap = a20_ufsd_backend_reap_unlinked(backend, ino);
        if reap != 0 { return reap; }
        let sync = a20_ufsd_backend_sync(backend);
        if sync != 0 { return sync; }
        a20_ufsd_backend_drop_inode(backend, ino)
    }
}

/// Consume one open instance exactly as the pre-token service did.
/// `accepted_token` (0 = legacy close) is recorded in the recent-closed
/// table once the open was consumed, even when the orphan reap afterwards
/// failed: the table records acceptance only, and cleanup retry stays on
/// the existing SYNC / mount-recovery paths.
fn close_open_instance(output: Handle, backend: u32,
                       lifetimes: &mut lifetime::Lifetimes, ino: u64,
                       accepted_token: u64) -> i64 {
    match lifetimes.close_with_backend(ino, accepted_token,
        || unsafe { a20_ufsd_backend_close(backend, ino) }) {
        Err(error) => error,
        Ok(lifetime::CloseOutcome::Replayed) => 0,
        Ok(lifetime::CloseOutcome::Consumed { release_orphan: release }) => {
            if !release { return 0; }
            diagnostic_log(output, b"UFSD_RUST_LIFETIME_PENDING: zero-open orphan\n");
            let rc = reap_and_drop(backend, ino);
            if rc == 0 {
                match lifetimes.reaped(ino) {
                    Ok(()) => {
                        diagnostic_log(output, b"UFSD_RUST_LIFETIME_REAPED: PASS\n");
                        0
                    }
                    Err(error) => error,
                }
            } else {
                diagnostic_log_number(output, b"UFSD_RUST_LIFETIME_REAP_ERROR: errno=",
                           rc.unsigned_abs() as u32, b"\n");
                diagnostic_log(output, b"UFSD_RUST_LIFETIME_REAP_FAILED: pending retained\n");
                rc
            }
        }
    }
}

fn sync_and_retry_pending(_output: Handle, backend: u32,
                          lifetimes: &mut lifetime::Lifetimes) -> i64 {
    #[cfg(ufsd_test_close_orphan)]
    {
    let mut cursor = 0;
    if lifetimes.pending(&mut cursor).is_some() {
        diagnostic_log(_output, b"UFSD_RUST_SYNC_RETRY: pending\n");
    }
    }
    let sync = unsafe { a20_ufsd_backend_sync(backend) };
    if sync != 0 { return sync; }
    let mut cursor = 0;
    while let Some(ino) = lifetimes.pending(&mut cursor) {
        let rc = reap_and_drop(backend, ino);
        if rc != 0 { return rc; }
        if let Err(error) = lifetimes.reaped(ino) { return error; }
    }
    0
}

fn graceful_backend_unmount(output: Handle, backend: u32,
                            lifetimes: &mut lifetime::Lifetimes,
                            retry_safe: &mut bool) -> i64 {
    if !lifetimes.all_closed() {
        *retry_safe = true;
        return -(ERR_BUSY as i64);
    }
    let sync = sync_and_retry_pending(output, backend, lifetimes);
    if sync != 0 { return sync; }
    let ret = unsafe { a20_ufsd_backend_unmount(backend) };
    // VNFS returns EBUSY only from its preflight open-count check, before it
    // drops registry references or begins ext4 teardown.
    if ret == -(ERR_BUSY as i64) { *retry_safe = true; }
    ret
}

fn dispatch_request(
    output: Handle,
    backend: u32,
    lifetimes: &mut lifetime::Lifetimes,
    request: &protocol::Request,
    name: *const c_char,
    payload: *const u8,
    rename_target: *const c_char,
    response: &mut protocol::Response,
) -> i64 {
    if backend != BACKEND_FAT && backend != BACKEND_VNFS { return -(ERR_INVALID as i64); }
    const ERR_INCONSISTENT: i64 = -117;
    if !lifetimes.request_allowed(request.opcode, protocol::OP_INIT) {
        #[cfg(ufsd_test_close_orphan)]
        diagnostic_log_number(output, b"UFSD_QUARANTINE_REJECT: opcode=",
                              request.opcode, b"\n");
        return ERR_INCONSISTENT;
    }
    let result = unsafe {
        match request.opcode {
            protocol::OP_INIT => {
                response.out0 = protocol::ROOT_INO;
                // Close-token negotiation: the client advertises the
                // capability in request arg0; the server answers with the
                // token capability in out1 (which implies the v2 lifetime
                // protocol). Without the advertisement, out1 carries the
                // plain lifetime capability exactly as before.
                let tokens = protocol::close_confirm_requested(backend, request.arg0);
                lifetimes.set_close_tokens_enabled(tokens);
                response.out1 = if tokens { protocol::CLOSE_CONFIRM_CAPABILITY }
                                else { protocol::lifetime_capability_for(backend) };
                response.out2 = protocol::SHUTDOWN_CAPABILITY;
                if backend == BACKEND_VNFS &&
                   request.version == protocol::LEGACY_PROTO_VERSION {
                    diagnostic_log(output, b"UFSD_RUST_LIFETIME_CAPABILITY: v2\n");
                }
                0
            }
            protocol::OP_LOOKUP => a20_ufsd_backend_lookup(backend, request.ino, name,
                                                            request.name_len, response),
            protocol::OP_GETATTR => a20_ufsd_backend_getattr(backend, request.ino, response),
            protocol::OP_READDIR => a20_ufsd_backend_readdir(backend, request.ino,
                                                              request.arg0, response),
            protocol::OP_CREATE => a20_ufsd_backend_create(backend, request.ino, name,
                                                            request.arg0 as u32, response),
            protocol::OP_MKDIR => a20_ufsd_backend_mkdir(backend, request.ino, name, response),
            protocol::OP_UNLINK => {
                if backend == BACKEND_FAT {
                    return a20_ufsd_backend_unlink(backend, request.ino, name);
                }
                let mut target = protocol::Response::default();
                let lookup = a20_ufsd_backend_lookup(backend, request.ino, name,
                                                      request.name_len, &mut target);
                if lookup != 0 { lookup }
                else {
                    let rc = a20_ufsd_backend_unlink(backend, request.ino, name);
                    if rc != 0 { rc }
                    else {
                        let mut links = 0u64;
                        let links_rc = a20_ufsd_backend_link_count(backend,
                                                                    target.out0,
                                                                    &mut links);
                        if links_rc != 0 { links_rc }
                        else if links == 0 {
                            #[cfg(ufsd_test_close_orphan)]
                            diagnostic_log_number(output, b"UFSD_RUST_LIFETIME_UNLINK: opens=",
                                lifetimes.open_count(target.out0), b"\n");
                            if lifetimes.unlinked(target.out0) {
                                reap_and_drop(backend, target.out0)
                            } else { 0 }
                        } else { 0 }
                    }
                }
            }
            protocol::OP_LINK => {
                if request.version != protocol::PROTO_VERSION || backend != BACKEND_VNFS {
                    return ERR_NOTSUPP;
                }
                a20_ufsd_backend_link(backend, request.ino, name, request.arg0)
            }
            protocol::OP_RMDIR => a20_ufsd_backend_rmdir(backend, request.ino, name),
            protocol::OP_TRUNCATE => a20_ufsd_backend_truncate(backend, request.ino, request.arg0),
            protocol::OP_READ => a20_ufsd_backend_read(backend, request.ino, request.arg0,
                                                        request.arg1 as u32, response),
            protocol::OP_WRITE => a20_ufsd_backend_write(backend, request.ino, request.arg0,
                                                          payload, request.payload_len, response),
            protocol::OP_RENAME => a20_ufsd_backend_rename(backend, request.ino, name,
                request.arg0, rename_target, response),
            protocol::OP_SYNC => sync_and_retry_pending(output, backend, lifetimes),
            protocol::OP_OPEN => {
                if request.version != protocol::PROTO_VERSION || backend != BACKEND_VNFS {
                    return ERR_NOTSUPP;
                }
                let exists = a20_ufsd_backend_has_inode(backend, request.ino);
                if exists != 0 { exists }
                else {
                    match lifetimes.open_with_backend(request.ino,
                        || a20_ufsd_backend_open(backend, request.ino)) {
                        Err(error) => error,
                        Ok(token) => {
                            #[cfg(ufsd_test_close_orphan)]
                            diagnostic_log_number(output, b"UFSD_RUST_LIFETIME_OPEN: opens=",
                                       lifetimes.open_count(request.ino), b"\n");
                            response.out2 = token;
                            0
                        }
                    }
                }
            }
            protocol::OP_CLOSE => {
                if request.version != protocol::PROTO_VERSION || backend != BACKEND_VNFS {
                    return ERR_NOTSUPP;
                }
                close_open_instance(output, backend, lifetimes, request.ino,
                                    request.arg0)
            }
            protocol::OP_CLOSE_CONFIRM => {
                if request.version != protocol::PROTO_VERSION || backend != BACKEND_VNFS {
                    return ERR_NOTSUPP;
                }
                match lifetimes.confirm_close(request.ino, request.arg0) {
                    Ok(()) => 0,
                    Err(error) => error,
                }
            }
            protocol::OP_STATFS => { a20_ufsd_backend_statfs(backend, response); 0 }
            _ => -(38i64),
        }
    };
    // A failed rollback means allocation state is not known. Stop all
    // further service requests for this generation, including SYNC retry.
    #[cfg(ufsd_test_close_orphan)]
    if result == ERR_INCONSISTENT {
        diagnostic_log(output, b"UFSD_QUARANTINE_ENTER: inconsistent state\n");
    }
    lifetimes.observe_result(result, ERR_INCONSISTENT)
}

unsafe fn pump_supervision(endpoint: Handle, output: Handle) -> i32 {
    let mut bytes = [0u8; 64];
    let mut length = 0u32;
    let status = unsafe { a20_ufsd_recv(endpoint, bytes.as_mut_ptr(), bytes.len() as u32, &mut length) };
    if status == ERR_WOULD_BLOCK { return 0; }
    if status < 0 { return -1; }
    let frame = match bytes.get(..length as usize) { Some(frame) => frame, None => return 1 };
    let header = match frame.get(..8) { Some(header) => header, None => return 1 };
    let version = u16::from_le_bytes([header[0], header[1]]);
    let kind = u16::from_le_bytes([header[2], header[3]]);
    let size = u32::from_le_bytes([header[4], header[5], header[6], header[7]]) as usize;
    if version != 1 || size != frame.len() { return 1; }
    if kind == 0x22 { return 2; }
    let sent = unsafe { a20_ufsd_send_bytes(endpoint, frame.as_ptr(), frame.len() as u32) };
    if sent < 0 { return -1; }
    let _ = output;
    1
}

static mut RX: MessageBuffer = MessageBuffer([0; protocol::MAX_MESSAGE]);

fn serve_loop(
    output: Handle,
    filesystem: &OwnedHandle,
    queue: &OwnedHandle,
    service_endpoint: Handle,
    service_watch: bool,
    backend: u32,
) -> i32 {
    let mut lifetimes = lifetime::Lifetimes::new();
    // Test-only transport fault: the first successful CLOSE for a live,
    // nonzero token is consumed normally, but its reply is dropped once.
    // The per-service-loop state is absent from production builds.
    #[cfg(ufsd_test_close_token_drop_ack_once)]
    let mut close_token_drop_ack_pending = true;
    // Same test-only transport fault for the confirmation: the first accepted
    // CONFIRM is processed but its reply is dropped once, so the client sees
    // an ambiguous confirmation and has to retry it (or leave the bounded
    // tombstone entry in place) without the close being redone.
    #[cfg(ufsd_test_close_token_drop_ack_once)]
    let mut confirm_drop_ack_pending = true;
    let mut boot_ns = 0u64;
    if unsafe { a20_ufsd_monotonic_ns(&mut boot_ns) } == 0 {
        lifetimes.seed_token_epoch(boot_ns);
    }
    loop {
        if service_watch {
            loop {
                let result = unsafe { pump_supervision(service_endpoint, output) };
                match supervision::action_for_supervision_result(result) {
                    supervision::WatchAction::CrashExit => {
                        // Do not call the graceful unmount path: this must
                        // leave the on-disk no-journal marker dirty.
                        return supervision::SVCMGR_CRASH_EXIT;
                    }
                    supervision::WatchAction::Continue => {
                        if result <= 0 { break; }
                    }
                }
            }
        }
        let mut length = 0u32;
        let bytes = unsafe { &mut (*core::ptr::addr_of_mut!(RX)).0 };
        let status = unsafe { a20_ufsd_recv(filesystem.raw(), bytes.as_mut_ptr(),
                                              bytes.len() as u32, &mut length) };
        if status == ERR_WOULD_BLOCK {
            if unsafe { a20_ufsd_event_wait(queue.raw()) } < 0 { continue; }
            continue;
        }
        if status < 0 {
            /* UXFS peer shutdown is the service's graceful unmount signal.
             * A task killed by the supervisor never reaches this path and
             * correctly leaves the on-disk no-journal state dirty. */
            let mut retry_safe = false;
            let rc = graceful_backend_unmount(output, backend, &mut lifetimes,
                                               &mut retry_safe);
            return if rc == 0 { 0 } else { 1 };
        }
        let header_len = core::mem::size_of::<protocol::Request>();
        if (length as usize) < header_len { continue; }
        let request = unsafe { core::ptr::read_unaligned(bytes.as_ptr().cast::<protocol::Request>()) };
        if request.magic != protocol::REQ_MAGIC ||
           (request.version != protocol::PROTO_VERSION && request.version != protocol::LEGACY_PROTO_VERSION) { continue; }
        let Some(layout) = protocol::validate_frame(length as usize, &request) else {
            let response = protocol::Response { magic: protocol::RESP_MAGIC,
                opcode: request.opcode, req_id: request.req_id, status: -ERR_INVALID, ..protocol::Response::default() };
            let _ = unsafe { a20_ufsd_send(filesystem.raw(), &response) };
            continue;
        };
        let name_bytes = &bytes[layout.name.clone()];
        let payload_bytes = &bytes[layout.payload.clone()];
        if !protocol::valid_name(name_bytes) {
            let response = protocol::Response { magic: protocol::RESP_MAGIC,
                opcode: request.opcode, req_id: request.req_id, status: -ERR_INVALID,
                ..protocol::Response::default() };
            let _ = unsafe { a20_ufsd_send(filesystem.raw(), &response) };
            continue;
        }
        let mut name = [0u8; protocol::MAX_NAME + 1];
        name[..name_bytes.len()].copy_from_slice(name_bytes);
        let name_pointer = name.as_ptr().cast::<c_char>();
        let mut rename_target = [0u8; protocol::MAX_NAME + 1];
        if request.opcode == protocol::OP_RENAME {
            if payload_bytes.len() > protocol::MAX_NAME {
                let response = protocol::Response { magic: protocol::RESP_MAGIC,
                    opcode: request.opcode, req_id: request.req_id, status: -ERR_INVALID,
                    ..protocol::Response::default() };
                let _ = unsafe { a20_ufsd_send(filesystem.raw(), &response) };
                continue;
            }
            rename_target[..payload_bytes.len()].copy_from_slice(payload_bytes);
        }
        let rename_pointer = rename_target.as_ptr().cast::<c_char>();
        let mut response = protocol::Response { magic: protocol::RESP_MAGIC,
            opcode: request.opcode, req_id: request.req_id, status: 0, ..protocol::Response::default() };
        let payload_pointer = unsafe { a20_ufsd_payload_buffer() };
        let _capacity = unsafe { a20_ufsd_payload_capacity() };
        let shutdown_request = request.opcode == protocol::OP_SHUTDOWN;
        #[cfg(ufsd_test_close_token_drop_ack_once)]
        let close_token_drop_candidate = request.opcode == protocol::OP_CLOSE &&
            request.arg0 != 0 &&
            lifetimes.token_lookup(request.ino, request.arg0) == lifetime::TokenLookup::Live;
        #[cfg(ufsd_test_close_token_drop_ack_once)]
        let close_token_replay_candidate = request.opcode == protocol::OP_CLOSE &&
            request.arg0 != 0 &&
            lifetimes.token_lookup(request.ino, request.arg0) == lifetime::TokenLookup::Replay;
        let result = if shutdown_request {
            if request.version != protocol::PROTO_VERSION || request.ino != protocol::ROOT_INO ||
               request.name_len != 0 || request.payload_len != 0 {
                -(ERR_INVALID as i64)
            } else {
                let mut retry_safe = false;
                let rc = graceful_backend_unmount(output, backend, &mut lifetimes,
                                                   &mut retry_safe);
                if retry_safe { response.out1 = protocol::SHUTDOWN_RETRY_SAFE; }
                rc
            }
        } else {
            dispatch_request(output, backend, &mut lifetimes, &request, name_pointer,
                payload_bytes.as_ptr(), rename_pointer, &mut response)
        };
        response.status = result.clamp(i32::MIN as i64, i32::MAX as i64) as i32;
        let cap = unsafe { a20_ufsd_payload_capacity() } as usize;
        if response.payload_len as usize > cap {
            response.status = -(ERR_IO);
            response.payload_len = 0;
        }
        let _ = payload_pointer;
        #[cfg(ufsd_test_close_token_drop_ack_once)]
        if confirm_drop_ack_pending && request.opcode == protocol::OP_CLOSE_CONFIRM &&
           result == 0 && request.arg0 != 0 {
            confirm_drop_ack_pending = false;
            log(output, b"UFSD_CONFIRM_DROP_ACK_ONCE: accepted CONFIRM reply dropped\n");
            continue;
        }
        #[cfg(ufsd_test_close_token_drop_ack_once)]
        if close_token_drop_ack_pending && close_token_drop_candidate && result == 0 &&
           lifetimes.token_lookup(request.ino, request.arg0) == lifetime::TokenLookup::Replay {
            close_token_drop_ack_pending = false;
            log(output, b"UFSD_CLOSE_TOKEN_DROP_ACK_ONCE: accepted CLOSE reply dropped\n");
            // Keep the service loop alive so the kernel can retry the same
            // token and receive the idempotent replay ACK on the next request.
            continue;
        }
        #[cfg(ufsd_test_close_token_drop_ack_once)]
        if close_token_replay_candidate && result == 0 {
            // This marker is confined to the explicit test ELF and contains
            // no token value. The pre-dispatch snapshot proves this request
            // was already accepted before it entered the dedup path. The
            // otherwise-unused CLOSE out2 carries a test-only wire sentinel
            // so kernel-console evidence survives service stdout teardown.
            response.out2 = TEST_CLOSE_REPLAY_SENTINEL;
            log(output, b"UFSD_CLOSE_TOKEN_REPLAY_ACK: PASS\n");
        }
        if unsafe { a20_ufsd_send(filesystem.raw(), &response) } < 0 {
            let mut retry_safe = false;
            let rc = graceful_backend_unmount(output, backend, &mut lifetimes,
                                               &mut retry_safe);
            return if rc == 0 { 0 } else { 1 };
        }
        if shutdown_request && result == 0 {
            return 0;
        }
    }
}

#[no_mangle]
pub extern "C" fn main(argc: c_int, argv: *const *const c_char,
                        _envp: *const *const c_char) -> c_int {
    let args = if argv.is_null() { &[][..] } else { unsafe { core::slice::from_raw_parts(argv, argc.max(0) as usize) } };
    let mount_arg = args.get(1).copied().unwrap_or(b"/ufs\0".as_ptr().cast());
    let block_arg = args.get(2).copied();
    let fstype_arg = args.get(3).copied().unwrap_or(b"fat\0".as_ptr().cast());
    let mount_bytes = match unsafe { c_bytes(mount_arg, 512) } { Some(s) if !s.is_empty() => s, _ => return 3 };
    let fstype = match unsafe { c_bytes(fstype_arg, 32) } { Some(s) => s, _ => return 3 };
    let backend = match protocol::backend_for(fstype) { Some(v) => v, None => return 3 };
    let block_index = match block_arg {
        Some(ptr) => match unsafe { c_bytes(ptr, 16).and_then(decimal_arg) } { Some(v) => v, None => return 3 },
        None => 2,
    };
    if block_index < 0 { return 3; }
    let mount_path = mount_arg;
    let mount_len = mount_bytes.len() as u32;
    let mut mounted = MountedFs { path: mount_path, path_len: mount_len, active: false };

    let mut output = HANDLE_NULL;
    if unsafe { a20_ufsd_startup(&mut output) } < 0 || output == HANDLE_NULL { return 90; }
    let _ = unsafe { a20_ufsd_set_block_index(block_index) };
    let mut pair = [HANDLE_NULL; 2];
    if unsafe { a20_ufsd_channel_create(pair.as_mut_ptr()) } < 0 {
        log(output, b"UFSD: channel create failed\n"); return 1;
    }
    let filesystem = OwnedHandle::new(pair[0]);
    let mut server_end = OwnedHandle::new(pair[1]);
    let _ = unsafe { a20_ufsd_path_create(mount_path, mount_len) };
    let flags = if fstype == b"iso9660" { 1 } else { 0 };
    if unsafe { a20_ufsd_serve(server_end.raw(), block_index, mount_path, mount_len, flags) } < 0 {
        log(output, b"UFSD: fs_serve failed\n"); return 2;
    }
    let _ = server_end.close_now();
    mounted.active = true;
    let mut capacity = 0u64;
    let _ = unsafe { a20_ufsd_capacity(block_index, &mut capacity) };
    if capacity == 0 {
        log_number(output, b"UFSD: no block device at index ", block_index as u32, b"; exiting cleanly\n");
        return 0;
    }
    if unsafe { a20_ufsd_backend_mount(backend, fstype_arg) } != 0 {
        if backend == BACKEND_VNFS && fstype == b"ext4" &&
           unsafe { a20_ufsd_backend_mount_readonly(backend, fstype_arg) } == 0 {
            log(output, b"UFSD: ext4 mounted read-only by explicit legacy JBD2 checksum downgrade\n");
        } else {
            log(output, b"UFSD: mount backend failed\n"); return 1;
        }
    }
    log(output, b"UFSD: serving "); log(output, mount_bytes); log(output, b" as "); log(output, fstype); log(output, b"\n");
    let mut raw_queue = HANDLE_NULL;
    if unsafe { a20_ufsd_event_queue_create(&mut raw_queue) } < 0 { return 1; }
    let queue = OwnedHandle::new(raw_queue);
    if unsafe { a20_ufsd_event_watch(queue.raw(), filesystem.raw(),
        EVENT_MESSAGE_READY | EVENT_PEER_CLOSED, FS_EVENT_TAG) } < 0 { return 1; }
    let service_endpoint = idl::A20_SVC_PING_SLOT as Handle;
    let service_watch = unsafe { a20_ufsd_event_watch(queue.raw(), service_endpoint,
        EVENT_MESSAGE_READY, SERVICE_EVENT_TAG) } >= 0;
    if !service_watch { log(output, b"UFSD: watch svc ep failed; continuing without it\n"); }
    let exit_code = serve_loop(output, &filesystem, &queue, service_endpoint,
                               service_watch, backend);
    drop(queue);
    drop(filesystem);
    exit_code
}

#[panic_handler]
fn panic(_: &PanicInfo<'_>) -> ! { loop { core::hint::spin_loop(); } }
