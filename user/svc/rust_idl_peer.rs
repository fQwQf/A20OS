//! A20OS Native IDL peer in Rust — the smallest user-space Rust client that
//! speaks the *existing* v1 svcman/echod channel protocol against the real C
//! service (user/svc/echod.c).  There is no simulated peer, no second service
//! and no new protocol version anywhere in this file.
//!
//! Wire contract — single source of truth is user/svc/a20_services.idl and its
//! generated user/svc/a20_services_idl.h, `interface a20_services version 1`:
//!
//!     message ENVELOPE   { u16 version; u16 type; u32 size; }
//!     message SVCMGR_ECHO { u32 seq; }
//!     const SVCMGR_REQ_ECHO  = 0x21;   const SVCMGR_REQ_CRASH = 0x22;
//!     const A20_SVC_CRASH_CODE = 42;
//!
//! `size` is the length of the whole message, envelope included — exactly what
//! user/svc/svcman.c sends (`sizeof(req)`) and what user/svc/echod.c checks
//! (`env.size != blen`).  echod validates nothing else:
//!
//!     blen  < sizeof(a20_idl_envelope_t)   -> drop (short)
//!     version != 1                          -> drop (unsupported version)
//!     size    != blen                       -> drop (malformed)
//!     type    == SVCMGR_REQ_CRASH           -> exit A20_SVC_CRASH_CODE
//!     otherwise                             -> echo the bytes back verbatim
//!
//! Every rule above is asserted against the live service below.  Unknown but
//! *valid* message types are asserted to be echoed — that is echod's documented
//! behaviour (docs/hybrid-kernel/05-idl-and-personality.md §1.4).
//!
//! Syscall-argument structures are mirrored field-for-field from
//! user/liba20rt/a20_types.h and pinned with size/offset assertions, so any
//! drift between these Rust bindings and the C ABI is a compile error rather
//! than a silent wire difference.  tools/xlator/rust_idl_peer_wire_check.c
//! independently re-derives the same bytes from the generated IDL header.

#![no_std]
#![no_main]

use core::ffi::c_int;
use core::mem::{offset_of, size_of};
use core::panic::PanicInfo;
use core::sync::atomic::{AtomicU32, Ordering};

#[allow(dead_code)]
mod a20_services_idl {
    include!(env!("A20_IDL_RUST_BINDINGS"));
}

#[path = "idl_negotiation.rs"]
mod idl_negotiation;

// ===========================================================================
// Constants mirrored from user/liba20rt/a20_types.h, a20_syscall.h and from
// the generated user/svc/a20_services_idl.h.  Each one names its header of
// origin so a rename in the SDK is visible here.
// ===========================================================================

type Handle = u32; // a20_types.h: typedef uint32_t a20_handle_t
type Status = i64; // a20_types.h: typedef int64_t a20_status_t
type Rights = u64; // a20_types.h: typedef uint64_t a20_rights_t

const A20_OK: Status = 0;
const A20_HANDLE_NULL: Handle = 0xFFFF_FFFF; // a20_types.h
const A20_OFFSET_CURRENT: u64 = u64::MAX; // a20_types.h: ((uint64_t)-1)

// a20_syscall.h: Handle (0x0100), Task/Thread (0x0200), Path/FS (0x0400),
// Event/IPC (0x0500).
const A20_SYS_HANDLE_CLOSE: u64 = 0x0100;
const A20_SYS_HANDLE_WRITE: u64 = 0x0402;
const A20_SYS_TASK_EXIT: u64 = 0x0200;
const A20_SYS_TASK_SPAWN: u64 = 0x0201;
const A20_SYS_TASK_WAIT: u64 = 0x0202;
const A20_SYS_TASK_KILL: u64 = 0x0203;
const A20_SYS_THREAD_SLEEP: u64 = 0x0207;
const A20_SYS_CLOCK_GET: u64 = 0x0700;
const A20_SYS_PATH_OPEN: u64 = 0x0400;
const A20_SYS_CHANNEL_CREATE: u64 = 0x0504;
const A20_SYS_CHANNEL_SEND: u64 = 0x0505;
const A20_SYS_CHANNEL_RECV: u64 = 0x0506;
const A20_SYS_CHANNEL_CALL: u64 = 0x0508;
const A20_TASK_WAIT_NONBLOCK: u64 = 0x1; // user/liba20rt/a20_task.h
// task_kill() takes a signal number. SIGKILL is 9 in the shared kernel signal
// ABI; signal 0 is only a permission/handle probe and does not terminate.
const A20_SIGKILL: u32 = 9;

// a20_types.h rights bits.
const A20_RIGHT_READ: Rights = 1 << 0;
const A20_RIGHT_WRITE: Rights = 1 << 1;
const A20_RIGHT_EXEC: Rights = 1 << 2;

// a20_types.h: #define A20_MSG_NONBLOCK (1u << 0)
const A20_MSG_NONBLOCK: u32 = 1 << 0;

// a20_types.h error codes, negated in a20_status_t.
const A20_ERR_BAD_HANDLE: Status = -5;
const A20_ERR_INVALID_ARGUMENT: Status = -12;
const A20_ERR_WOULD_BLOCK: Status = -18;
const A20_ERR_TIMED_OUT: Status = -19;
const A20_ERR_CANCELED: Status = -20;

// user/svc/a20_services_idl.h, generated from user/svc/a20_services.idl v1.
const A20_SERVICES_IDL_VERSION: u16 = a20_services_idl::A20_SERVICES_IDL_VERSION;
const A20_SVC_CRASH_CODE: c_int = a20_services_idl::A20_SVC_CRASH_CODE as c_int;
const SVCMGR_REQ_ECHO: u16 = a20_services_idl::SVCMGR_REQ_ECHO as u16;
const SVCMGR_REQ_CRASH: u16 = a20_services_idl::SVCMGR_REQ_CRASH as u16;
const SVCMGR_REQ_VERSION_NEGOTIATE: u16 =
    a20_services_idl::SVCMGR_REQ_VERSION_NEGOTIATE as u16;
const SVCMGR_REP_VERSION_NEGOTIATE: u16 =
    a20_services_idl::SVCMGR_REP_VERSION_NEGOTIATE as u16;
const A20_SVC_ENDPOINT_SLOT: Handle = a20_services_idl::A20_SVC_ENDPOINT_SLOT as Handle;

// Keep service names aligned with the Native per-architecture image tags.
#[cfg(target_arch = "riscv64")]
const ECHOD_PATH: &[u8] = b"/bin/svc-echod-rv";
#[cfg(target_arch = "riscv32")]
const ECHOD_PATH: &[u8] = b"/bin/svc-echod-rv32";
#[cfg(target_arch = "aarch64")]
const ECHOD_PATH: &[u8] = b"/bin/svc-echod-aarch64";
#[cfg(target_arch = "x86_64")]
const ECHOD_PATH: &[u8] = b"/bin/svc-echod-x86_64";
#[cfg(idl_ack_test)]
#[cfg(target_arch = "riscv64")]
const ACK_ADVERSARY_PATH: &[u8] = b"/bin/rust-idl-adversary-rv";
#[cfg(idl_ack_test)]
#[cfg(target_arch = "riscv32")]
const ACK_ADVERSARY_PATH: &[u8] = b"/bin/rust-idl-adversary-rv32";
#[cfg(idl_ack_test)]
#[cfg(target_arch = "aarch64")]
const ACK_ADVERSARY_PATH: &[u8] = b"/bin/rust-idl-adversary-aarch64";
#[cfg(idl_ack_test)]
#[cfg(target_arch = "x86_64")]
const ACK_ADVERSARY_PATH: &[u8] = b"/bin/rust-idl-adversary-x86_64";

// The peer's own exit status.  Nothing here is part of the wire protocol.
const EXIT_PASS: c_int = 0;
const EXIT_FAIL: c_int = 1;
const EXIT_NO_START_INFO: c_int = 90;
const EXIT_PANIC: c_int = 200;

// ===========================================================================
// ABI structures, mirrored from user/liba20rt/a20_types.h
// ===========================================================================

/// a20_types.h `a20_start_info_t`.  Only four fields are read.
#[repr(C)]
#[derive(Clone, Copy)]
struct StartInfo {
    size: u32,
    version: u32,
    argc: u32,
    envc: u32,
    auxc: u32,
    reserved0: u32,
    argv: u64,
    envp: u64,
    auxv: u64,
    root_dir: Handle,
    cwd_dir: Handle,
    stdin_handle: Handle,
    stdout_handle: Handle,
    stderr_handle: Handle,
    self_task: Handle,
    main_thread: Handle,
    default_event_queue: Handle,
    page_size: u64,
    user_clock_freq: u64,
    service_registry: Handle,
    pad_registry: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct Iovec {
    base: u64,
    len: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct IoArgs {
    size: u32,
    version: u32,
    handle: Handle,
    pad0: u32,
    iov: u64,
    iov_count: u32,
    pad1: u32,
    offset: u64,
    out_count: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ChannelCreateArgs {
    size: u32,
    version: u32,
    msg_capacity: u32,
    flags: u32,
    type_: u64,
    out_endpoints: [Handle; 2],
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MsgSendArgs {
    size: u32,
    version: u32,
    channel: Handle,
    pad: u32,
    data: u64,
    data_len: u32,
    flags: u32,
    handles: u64,
    handle_count: u32,
    transfer_rights: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct MsgRecvArgs {
    size: u32,
    version: u32,
    channel: Handle,
    pad: u32,
    data_buf: u64,
    data_buf_len: u32,
    pad2: u32,
    handle_buf: u64,
    handle_buf_count: u32,
    flags: u32,
    out_data_len: u64,
    out_handle_count: u32,
    out_rights_buf: u64,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct ChannelCallArgs {
    size: u32,
    version: u32,
    channel: Handle,
    flags: u32,
    data: u64,
    data_len: u32,
    pad: u32,
    handles: u64,
    handle_count: u32,
    transfer_rights: u64,
    reply_buf: u64,
    reply_buf_len: u32,
    pad2: u32,
    reply_handle_buf: u64,
    reply_handle_buf_count: u32,
    reply_rights_buf: u64,
    out_reply_len: u32,
    out_reply_handles: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct PathOpenArgs {
    size: u32,
    version: u32,
    dir: Handle,
    flags: u32,
    rights: Rights,
    path: u64,
    path_len: u32,
    mode: u32,
    out_handle: Handle,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct SpawnHandle {
    handle: Handle,
    rights: Rights,
    target_slot: u32,
    flags: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct TaskSpawnArgs {
    size: u32,
    version: u32,
    image: Handle,
    root_dir: Handle,
    cwd_dir: Handle,
    event_queue: Handle,
    argv: u64,
    envp: u64,
    argc: u32,
    envc: u32,
    handles: u64,
    handle_count: u32,
    flags: u32,
    out_task: Handle,
    stdin_handle: Handle,
    stdout_handle: Handle,
    stderr_handle: Handle,
    reserved: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
struct TaskStatus {
    size: u32,
    version: u32,
    exit_code: c_int,
    exit_reason: u32,
    utime_ns: u64,
    stime_ns: u64,
}

/// user/svc/a20_services_idl.h `a20_idl_envelope_t` = message ENVELOPE.
#[repr(C)]
#[derive(Clone, Copy)]
struct Envelope {
    version: u16,
    type_: u16,
    size: u32,
}

/// user/svc/a20_services_idl.h `a20_idl_svcmgr_echo_t` = message SVCMGR_ECHO.
#[repr(C)]
#[derive(Clone, Copy)]
struct SvcmgrEcho {
    seq: u32,
}

/// The exact composite request user/svc/svcman.c:echo_verify() sends.
#[repr(C)]
#[derive(Clone, Copy)]
struct EchoRequest {
    env: Envelope,
    body: SvcmgrEcho,
}

/// The exact crash request user/svc/svcman.c sends: envelope only, 8 bytes.
#[repr(C)]
#[derive(Clone, Copy)]
struct CrashRequest {
    env: Envelope,
}

// --- ABI pinning: a mismatch with the C headers must not compile -----------
const _: () = assert!(size_of::<StartInfo>() == 104);
const _: () = assert!(align_of_ptr());
const _: () = assert!(offset_of!(StartInfo, argc) == 8);
const _: () = assert!(offset_of!(StartInfo, root_dir) == 48);
const _: () = assert!(offset_of!(StartInfo, stdout_handle) == 60);

const _: () = assert!(size_of::<Iovec>() == 16);
const _: () = assert!(offset_of!(Iovec, len) == 8);

const _: () = assert!(size_of::<IoArgs>() == 48);
const _: () = assert!(offset_of!(IoArgs, iov) == 16);
const _: () = assert!(offset_of!(IoArgs, offset) == 32);
const _: () = assert!(offset_of!(IoArgs, out_count) == 40);

const _: () = assert!(size_of::<ChannelCreateArgs>() == 32);
const _: () = assert!(offset_of!(ChannelCreateArgs, type_) == 16);
const _: () = assert!(offset_of!(ChannelCreateArgs, out_endpoints) == 24);

const _: () = assert!(size_of::<MsgSendArgs>() == 56);
const _: () = assert!(offset_of!(MsgSendArgs, data) == 16);
const _: () = assert!(offset_of!(MsgSendArgs, handles) == 32);
const _: () = assert!(offset_of!(MsgSendArgs, transfer_rights) == 48);

const _: () = assert!(size_of::<MsgRecvArgs>() == 72);
const _: () = assert!(offset_of!(MsgRecvArgs, data_buf) == 16);
const _: () = assert!(offset_of!(MsgRecvArgs, handle_buf) == 32);
const _: () = assert!(offset_of!(MsgRecvArgs, out_data_len) == 48);
const _: () = assert!(offset_of!(MsgRecvArgs, out_rights_buf) == 64);

const _: () = assert!(size_of::<ChannelCallArgs>() == 104);
const _: () = assert!(offset_of!(ChannelCallArgs, data) == 16);
const _: () = assert!(offset_of!(ChannelCallArgs, handles) == 32);
const _: () = assert!(offset_of!(ChannelCallArgs, transfer_rights) == 48);
const _: () = assert!(offset_of!(ChannelCallArgs, reply_buf) == 56);
const _: () = assert!(offset_of!(ChannelCallArgs, reply_handle_buf) == 72);
const _: () = assert!(offset_of!(ChannelCallArgs, reply_rights_buf) == 88);
const _: () = assert!(offset_of!(ChannelCallArgs, out_reply_len) == 96);

const _: () = assert!(size_of::<PathOpenArgs>() == 48);
const _: () = assert!(offset_of!(PathOpenArgs, rights) == 16);
const _: () = assert!(offset_of!(PathOpenArgs, path) == 24);
const _: () = assert!(offset_of!(PathOpenArgs, out_handle) == 40);

const _: () = assert!(size_of::<SpawnHandle>() == 24);
const _: () = assert!(offset_of!(SpawnHandle, rights) == 8);
const _: () = assert!(offset_of!(SpawnHandle, target_slot) == 16);

const _: () = assert!(size_of::<TaskSpawnArgs>() == 88);
const _: () = assert!(offset_of!(TaskSpawnArgs, argv) == 24);
const _: () = assert!(offset_of!(TaskSpawnArgs, handles) == 48);
const _: () = assert!(offset_of!(TaskSpawnArgs, out_task) == 64);
const _: () = assert!(offset_of!(TaskSpawnArgs, stdout_handle) == 72);
const _: () = assert!(offset_of!(TaskSpawnArgs, reserved) == 80);

const _: () = assert!(size_of::<TaskStatus>() == 32);
const _: () = assert!(offset_of!(TaskStatus, exit_code) == 8);

// --- Wire pinning against the generated IDL header -----------------------
// message ENVELOPE { u16 version; u16 type; u32 size; } packs to 8 bytes with
// no padding, and SVCMGR_ECHO { u32 seq; } to 4 bytes.
const _: () = assert!(size_of::<Envelope>() == 8);
const _: () = assert!(offset_of!(Envelope, type_) == 2);
const _: () = assert!(offset_of!(Envelope, size) == 4);
const _: () = assert!(size_of::<SvcmgrEcho>() == 4);
const _: () = assert!(size_of::<EchoRequest>() == 12);
const _: () = assert!(offset_of!(EchoRequest, body) == 8);
const _: () = assert!(size_of::<CrashRequest>() == 8);

const fn align_of_ptr() -> bool {
    core::mem::align_of::<StartInfo>() == 8
}

// ===========================================================================
// Wire vectors
//
// The hex literals below are the only source of truth for the bytes this peer
// puts on the channel.  tools/xlator/rust_idl_peer_wire_check.c parses the
// `wire:<name>:<hex>` markers *out of this file* and re-derives the same bytes
// from user/svc/a20_services_idl.h, so the two cannot drift silently.
//
// All values are little-endian, the byte order every architecture in this tree
// builds the Native ABI with.
// ===========================================================================

// wire:echo_exact:010021000c00000005000000
// wire:echo_unknown_type:01007b000c000000efbeadde
// wire:crash_request:0100220008000000
// wire:drop_version_mismatch:020021000c000000ddccbbaa
// wire:drop_size_mismatch:0100210009000000ddccbbaa
// wire:drop_short_message:01002100

const fn nibble(b: u8) -> u8 {
    match b {
        b'0'..=b'9' => b - b'0',
        b'a'..=b'f' => b - b'a' + 10,
        b'A'..=b'F' => b - b'A' + 10,
        _ => panic!("wire vector is not hex"),
    }
}

const fn hex_bytes<const N: usize>(s: &str) -> [u8; N] {
    let raw = s.as_bytes();
    assert!(raw.len() == 2 * N, "wire vector length mismatch");
    let mut out = [0u8; N];
    let mut i = 0;
    while i < N {
        out[i] = (nibble(raw[2 * i]) << 4) | nibble(raw[2 * i + 1]);
        i += 1;
    }
    out
}

/// SVCMGR_REQ_ECHO with seq=5.  Byte-identical to what svcman.c sends.
const WIRE_ECHO_EXACT: [u8; 12] = hex_bytes::<12>("010021000c00000005000000");
/// A type the IDL does not define (0x7B).  Still a *valid* v1 envelope, so
/// echod's documented behaviour is to echo it rather than to drop it.
const WIRE_ECHO_UNKNOWN_TYPE: [u8; 12] =
    hex_bytes::<12>("01007b000c000000efbeadde");
/// SVCMGR_REQ_CRASH, envelope only.  Never echoed; echod exits 42 instead.
const WIRE_CRASH_REQUEST: [u8; 8] = hex_bytes::<8>("0100220008000000");
/// Valid envelope shape but version 2: unsupported, must be dropped.
const WIRE_DROP_VERSION_MISMATCH: [u8; 12] =
    hex_bytes::<12>("020021000c000000ddccbbaa");
/// version 1 with a lying `size` (9 for a 12-byte message): malformed, drop.
const WIRE_DROP_SIZE_MISMATCH: [u8; 12] =
    hex_bytes::<12>("0100210009000000ddccbbaa");
/// Four bytes only — shorter than sizeof(a20_idl_envelope_t): drop.
const WIRE_DROP_SHORT_MESSAGE: [u8; 4] = hex_bytes::<4>("01002100");

/// Build the primary v1 request from the IDL-generated Rust binding.  The
/// hard-coded wire vector above remains an independent oracle, and main checks
/// that the generated codec emits those exact bytes before sending it.
fn generated_echo_frame() -> [u8; 12] {
    let payload = a20_services_idl::svcmgr_echo_encode(
        &a20_services_idl::SVCMGR_ECHO { seq: 5 });
    let envelope = a20_services_idl::envelope_encode(
        &a20_services_idl::ENVELOPE {
            version: a20_services_idl::A20_SERVICES_IDL_VERSION,
            r#type: a20_services_idl::SVCMGR_REQ_ECHO as u16,
            size: (a20_services_idl::ENVELOPE_WIRE_SIZE + payload.len()) as u32,
        });
    let mut frame = [0u8; 12];
    frame[..envelope.len()].copy_from_slice(&envelope);
    frame[envelope.len()..].copy_from_slice(&payload);
    frame
}

fn generated_negotiation_frame(min_version: u16, max_version: u16,
                               nonce: u32) -> [u8; 16] {
    let payload = a20_services_idl::svcmgr_version_request_encode(
        &a20_services_idl::SVCMGR_VERSION_REQUEST {
            min_version, max_version, nonce,
        });
    let envelope = a20_services_idl::envelope_encode(
        &a20_services_idl::ENVELOPE {
            version: a20_services_idl::A20_SERVICES_IDL_VERSION,
            r#type: SVCMGR_REQ_VERSION_NEGOTIATE,
            size: (a20_services_idl::ENVELOPE_WIRE_SIZE + payload.len()) as u32,
        });
    let mut frame = [0u8; 16];
    frame[..envelope.len()].copy_from_slice(&envelope);
    frame[envelope.len()..].copy_from_slice(&payload);
    frame
}

/// Perform the new request/reply exchange or the deliberately narrow legacy
/// fallback. A v1 echo is recognized only when every returned byte equals the
/// full request; then an independent ordinary echo proves the service is
/// still responsive. No malformed ACK or NO_COMMON result can downgrade.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum NegotiationOutcome { Selected(u16), NoCommon, LegacyEcho, Rejected }

fn reject_ack(label: &str, expected_reject: bool, reason: &str)
    -> Result<NegotiationOutcome, ()>
{
    if expected_reject {
        Ok(NegotiationOutcome::Rejected)
    } else {
        fail(label, reason);
        Err(())
    }
}

fn negotiate_service(ep: Handle, min_version: u16, max_version: u16,
                     nonce: u32, label: &str, expected_reject: bool)
    -> Result<NegotiationOutcome, ()>
{
    let request = generated_negotiation_frame(min_version, max_version, nonce);
    wire(label, "tx", &request);
    let start = monotonic_now_ns().map_err(|st| {
        put_status("clock_get", label, st);
    })?;
    let deadline = start.saturating_add(2_000_000_000);
    send_until(ep, label, &request, deadline).map_err(|st| {
        put_status("channel_send", label, st);
    })?;
    let mut reply = [0u8; 64];
    let n = recv_until(ep, label, &mut reply, deadline).map_err(|st| {
        if st != A20_ERR_TIMED_OUT { put_status("channel_recv", label, st); }
    })?;
    let got = reply.get(..n).ok_or_else(|| fail(label, "reply exceeds buffer"))?;
    wire(label, "rx", got);

    if got.len() == request.len() && bytes_eq(got, &request) {
        if !idl_negotiation::legacy_v1_in_range(min_version, max_version) {
            return reject_ack(label, expected_reject,
                              "legacy service only offers v1 outside client range");
        }
        put_line("RUST_NATIVE_SVC: negotiation legacy-echo detected");
        if !expect_echo(ep, "legacy_liveness_echo", &generated_echo_frame()) {
            fail(label, "legacy echo challenge failed");
            return Err(());
        }
        return Ok(NegotiationOutcome::LegacyEcho);
    }

    let Some(envelope_bytes) = got.get(..a20_services_idl::ENVELOPE_WIRE_SIZE) else {
        return reject_ack(label, expected_reject,
                          "negotiation reply is shorter than envelope");
    };
    let Some(envelope) = a20_services_idl::envelope_decode(envelope_bytes) else {
        return reject_ack(label, expected_reject,
                          "negotiation reply envelope is malformed");
    };
    if envelope.version != a20_services_idl::A20_SERVICES_IDL_VERSION ||
        envelope.r#type != SVCMGR_REP_VERSION_NEGOTIATE ||
        envelope.size as usize != got.len() ||
        got.len() != a20_services_idl::ENVELOPE_WIRE_SIZE +
                     a20_services_idl::SVCMGR_VERSION_RESPONSE_WIRE_SIZE {
        return reject_ack(label, expected_reject,
                          "wrong negotiation reply type, version, or length");
    }
    let Some(response_bytes) = got.get(a20_services_idl::ENVELOPE_WIRE_SIZE..) else {
        return reject_ack(label, expected_reject,
                          "negotiation response payload is missing");
    };
    let Some(response) = a20_services_idl::svcmgr_version_response_decode(response_bytes) else {
        return reject_ack(label, expected_reject,
                          "negotiation response payload is malformed");
    };
    match idl_negotiation::validate_response(
        min_version, max_version, nonce,
        response.min_version, response.max_version, response.selected_version,
        response.status, response.nonce) {
        Ok(Some(selected)) => {
            put_str("RUST_NATIVE_SVC: negotiation selected=");
            put_u64(selected as u64);
            put(b"\n");
            Ok(NegotiationOutcome::Selected(selected))
        }
        Ok(None) => {
            put_line("RUST_NATIVE_SVC: negotiation no-common");
            Ok(NegotiationOutcome::NoCommon)
        }
        Err(_) => {
            reject_ack(label, expected_reject, "invalid negotiation acknowledgement")
        }
    }
}

// Pin each vector against the structs above, so the hex literals and the
// mirrored ABI cannot disagree.  (Element-wise indexing, not slices: `Index`
// with a range is not const-callable.)
const _: () = assert!(WIRE_ECHO_EXACT[0] == 1);
const _: () = assert!(WIRE_ECHO_EXACT[1] == 0);
const _: () = assert!(WIRE_ECHO_EXACT[2] == 0x21);
const _: () = assert!(WIRE_ECHO_EXACT[3] == 0);
const _: () = assert!(WIRE_ECHO_EXACT[4] == 12);
const _: () = assert!(WIRE_ECHO_EXACT[5] == 0);
const _: () = assert!(WIRE_ECHO_EXACT[6] == 0);
const _: () = assert!(WIRE_ECHO_EXACT[7] == 0);
const _: () = assert!(WIRE_ECHO_EXACT[8] == 5);
const _: () = assert!(WIRE_ECHO_EXACT[9] == 0);
const _: () = assert!(WIRE_ECHO_EXACT[10] == 0);
const _: () = assert!(WIRE_ECHO_EXACT[11] == 0);

const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[0] == 1);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[1] == 0);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[2] == 0x7b);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[3] == 0);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[4] == 12);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[5] == 0);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[6] == 0);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[7] == 0);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[8] == 0xef);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[9] == 0xbe);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[10] == 0xad);
const _: () = assert!(WIRE_ECHO_UNKNOWN_TYPE[11] == 0xde);

const _: () = assert!(WIRE_CRASH_REQUEST[0] == 1);
const _: () = assert!(WIRE_CRASH_REQUEST[1] == 0);
const _: () = assert!(WIRE_CRASH_REQUEST[2] == SVCMGR_REQ_CRASH as u8);
const _: () = assert!(WIRE_CRASH_REQUEST[3] == 0);
const _: () = assert!(WIRE_CRASH_REQUEST[4] == 8);
const _: () = assert!(WIRE_CRASH_REQUEST[5] == 0);
const _: () = assert!(WIRE_CRASH_REQUEST[6] == 0);
const _: () = assert!(WIRE_CRASH_REQUEST[7] == 0);

const _: () = assert!(WIRE_DROP_VERSION_MISMATCH[0] == 2);
const _: () = assert!(WIRE_DROP_VERSION_MISMATCH[1] == 0);
const _: () = assert!(WIRE_DROP_VERSION_MISMATCH[2] == SVCMGR_REQ_ECHO as u8);
const _: () = assert!(WIRE_DROP_VERSION_MISMATCH[4] == 12);

const _: () = assert!(WIRE_DROP_SIZE_MISMATCH[0] == 1);
const _: () = assert!(WIRE_DROP_SIZE_MISMATCH[2] == SVCMGR_REQ_ECHO as u8);
const _: () = assert!(WIRE_DROP_SIZE_MISMATCH[4] == 9);
const _: () = assert!(WIRE_DROP_SIZE_MISMATCH[5] == 0);

const _: () = assert!(WIRE_DROP_SHORT_MESSAGE[0] == 1);
const _: () = assert!(WIRE_DROP_SHORT_MESSAGE[1] == 0);
const _: () = assert!(WIRE_DROP_SHORT_MESSAGE[2] == SVCMGR_REQ_ECHO as u8);
const _: () = assert!(WIRE_DROP_SHORT_MESSAGE[3] == 0);

// ===========================================================================
// Syscall layer
// ===========================================================================

mod sys {
    use super::*;
    extern "C" {
        fn a20_rust_peer_syscall6(nr: u64, a0: u64, a1: u64, a2: u64,
                                  a3: u64, a4: u64, a5: u64) -> Status;
    }

    /// Delegate architecture-specific register placement and trap behavior
    /// to the Native SDK's `a20_syscall6` implementation.
    #[cfg(any(target_arch = "riscv64", target_arch = "riscv32", target_arch = "aarch64", target_arch = "x86_64"))]
    pub unsafe fn syscall6(
        nr: u64,
        a0: u64,
        a1: u64,
        a2: u64,
        a3: u64,
        a4: u64,
        a5: u64,
    ) -> Status {
        a20_rust_peer_syscall6(nr, a0, a1, a2, a3, a4, a5)
    }

    #[cfg(not(any(target_arch = "riscv64", target_arch = "riscv32", target_arch = "aarch64", target_arch = "x86_64")))]
    pub unsafe fn syscall6(
        _nr: u64,
        _a0: u64,
        _a1: u64,
        _a2: u64,
        _a3: u64,
        _a4: u64,
        _a5: u64,
    ) -> Status {
        compile_error!(
            "the Rust Native peer supports only installed matching 64-bit \
             Native targets"
        );
    }
}

static OUT_HANDLE: AtomicU32 = AtomicU32::new(A20_HANDLE_NULL);
static ROOT_DIR: AtomicU32 = AtomicU32::new(A20_HANDLE_NULL);

// user/svc/rust_peer_crt0.c pulls in liba20rt/crt0_a20.h, which defines this
// for the C programs.  The Rust peer reads it through the same C ABI instead
// of re-deriving the start_info pointer from the stack.
extern "C" {
    fn a20_get_start_info() -> *mut StartInfo;
}

fn out() -> Handle {
    OUT_HANDLE.load(Ordering::Relaxed)
}

fn root() -> Handle {
    ROOT_DIR.load(Ordering::Relaxed)
}

// ---- Typed wrappers over the eleven syscalls this peer needs -------------

fn thread_sleep_ns(ns: u64) -> Status {
    unsafe { sys::syscall6(A20_SYS_THREAD_SLEEP, ns, 0, 0, 0, 0, 0) }
}

fn monotonic_now_ns() -> Result<u64, Status> {
    let mut ns = 0u64;
    let st = unsafe {
        sys::syscall6(A20_SYS_CLOCK_GET, 1, &mut ns as *mut u64 as u64, 0, 0, 0, 0)
    };
    if st < 0 { Err(st) } else { Ok(ns) }
}

fn task_exit(code: c_int) -> ! {
    unsafe {
        sys::syscall6(A20_SYS_TASK_EXIT, code as u64, 0, 0, 0, 0, 0);
    }
    // A20OS task_exit never returns.  Park, so a future kernel change cannot
    // turn this into an accidental fall-through into the caller's frame.
    loop {
        core::hint::spin_loop();
    }
}

fn task_kill(task: Handle, reason: u32) -> Status {
    unsafe {
        sys::syscall6(A20_SYS_TASK_KILL, task as u64, reason as u64, 0, 0, 0, 0)
    }
}

fn handle_close(h: Handle) -> Status {
    unsafe { sys::syscall6(A20_SYS_HANDLE_CLOSE, h as u64, 0, 0, 0, 0, 0) }
}

/// Poll the real SDK nonblocking wait API using the transaction's existing
/// absolute deadline. An endpoint can close before the task is fully reaped,
/// so a blocking wait here would otherwise make the protocol unbounded.
fn task_wait_until(task: Handle, status: &mut TaskStatus, deadline: u64) -> Status {
    loop {
        status.size = size_of::<TaskStatus>() as u32;
        status.version = 1;
        let st = unsafe {
            sys::syscall6(
                A20_SYS_TASK_WAIT,
                task as u64,
                A20_TASK_WAIT_NONBLOCK,
                status as *mut TaskStatus as u64,
                0,
                0,
                0,
            )
        };
        if st != A20_ERR_WOULD_BLOCK { return st; }
        match pause_before_deadline(deadline) {
            Ok(()) => {}
            Err(st) => return st,
        }
    }
}

fn put(bytes: &[u8]) {
    if out() == A20_HANDLE_NULL {
        return;
    }
    let iov = Iovec {
        base: bytes.as_ptr() as u64,
        len: bytes.len() as u64,
    };
    let args = IoArgs {
        size: size_of::<IoArgs>() as u32,
        version: 1,
        handle: out(),
        pad0: 0,
        iov: &iov as *const Iovec as u64,
        iov_count: 1,
        pad1: 0,
        offset: A20_OFFSET_CURRENT,
        out_count: 0,
    };
    unsafe {
        sys::syscall6(A20_SYS_HANDLE_WRITE, &args as *const IoArgs as u64, 0, 0, 0, 0, 0);
    }
}

// ---- IPC -----------------------------------------------------------------

fn channel_create() -> Result<[Handle; 2], Status> {
    let mut args = ChannelCreateArgs {
        size: size_of::<ChannelCreateArgs>() as u32,
        version: 1,
        msg_capacity: 0,
        flags: 0,
        type_: 0,
        out_endpoints: [A20_HANDLE_NULL; 2],
    };
    let st = unsafe {
        sys::syscall6(
            A20_SYS_CHANNEL_CREATE,
            &mut args as *mut ChannelCreateArgs as u64,
            0,
            0,
            0,
            0,
            0,
        )
    };
    if st != A20_OK {
        return Err(st);
    }
    Ok(args.out_endpoints)
}

fn channel_send(ep: Handle, bytes: &[u8]) -> Status {
    let args = MsgSendArgs {
        size: size_of::<MsgSendArgs>() as u32,
        version: 1,
        channel: ep,
        pad: 0,
        data: bytes.as_ptr() as u64,
        data_len: bytes.len() as u32,
        flags: A20_MSG_NONBLOCK,
        handles: 0,
        handle_count: 0,
        transfer_rights: 0,
    };
    unsafe {
        sys::syscall6(A20_SYS_CHANNEL_SEND, &args as *const MsgSendArgs as u64, 0, 0, 0, 0, 0)
    }
}

/// One non-blocking receive.
///
/// Ok(None) — mailbox empty (A20_ERR_WOULD_BLOCK).
/// Ok(Some) — a message of n bytes arrived.
/// Err      — any other kernel status, including -A20_ERR_CANCELED when the
///            peer endpoint is gone rather than merely empty.
fn try_recv(ep: Handle, buf: &mut [u8]) -> Result<Option<usize>, Status> {
    let mut args = MsgRecvArgs {
        size: size_of::<MsgRecvArgs>() as u32,
        version: 1,
        channel: ep,
        pad: 0,
        data_buf: buf.as_mut_ptr() as u64,
        data_buf_len: buf.len() as u32,
        pad2: 0,
        handle_buf: 0,
        handle_buf_count: 0,
        flags: A20_MSG_NONBLOCK,
        out_data_len: 0,
        out_handle_count: 0,
        out_rights_buf: 0,
    };
    let st = unsafe {
        sys::syscall6(A20_SYS_CHANNEL_RECV, &mut args as *mut MsgRecvArgs as u64, 0, 0, 0, 0, 0)
    };
    if st == A20_ERR_WOULD_BLOCK { return Ok(None); }
    if st < A20_OK { return Err(st); }
    // sys_a20_channel_recv returns the delivered length and publishes the
    // same count in out_data_len. Reject inconsistent successful results.
    if st as usize != args.out_data_len as usize {
        return Err(A20_ERR_INVALID_ARGUMENT);
    }
    Ok(Some(args.out_data_len as usize))
}

fn pause_before_deadline(deadline: u64) -> Result<(), Status> {
    let now = monotonic_now_ns()?;
    if now >= deadline { return Err(A20_ERR_TIMED_OUT); }
    let st = thread_sleep_ns((deadline - now).min(1_000_000));
    if st == A20_OK { Ok(()) } else { Err(st) }
}

/// Retry a nonblocking send until the bounded deadline. In particular, never
/// assume WOULD_BLOCK means a fused call delivered its request.
fn send_until(ep: Handle, label: &str, req: &[u8], deadline: u64) -> Result<(), Status> {
    loop {
        let st = channel_send(ep, req);
        if st >= A20_OK { return Ok(()); }
        if st == A20_ERR_WOULD_BLOCK {
            pause_before_deadline(deadline)?;
        } else {
            put_status("channel_send", label, st);
            return Err(st);
        }
    }
}

fn recv_until(ep: Handle, label: &str, buf: &mut [u8], deadline: u64)
    -> Result<usize, Status>
{
    loop {
        match try_recv(ep, buf)? {
            Some(n) => return Ok(n),
            None => pause_before_deadline(deadline).map_err(|st| {
                if st == A20_ERR_TIMED_OUT { fail(label, "reply deadline expired"); }
                else { put_status("wait_reply", label, st); }
                st
            })?,
        }
    }
}

/// Absolute path open, exactly as user/svc/svcman.c:spawn_echod() does it
/// (dir = A20_HANDLE_NULL, rights = READ|EXEC).
fn path_open(path: &[u8]) -> Result<Handle, Status> {
    let mut args = PathOpenArgs {
        size: size_of::<PathOpenArgs>() as u32,
        version: 1,
        dir: A20_HANDLE_NULL,
        flags: 0,
        rights: A20_RIGHT_READ | A20_RIGHT_EXEC,
        path: path.as_ptr() as u64,
        path_len: path.len() as u32,
        mode: 0,
        out_handle: A20_HANDLE_NULL,
    };
    let st = unsafe {
        sys::syscall6(A20_SYS_PATH_OPEN, &mut args as *mut PathOpenArgs as u64, 0, 0, 0, 0, 0)
    };
    if st != A20_OK {
        return Err(st);
    }
    if args.out_handle == A20_HANDLE_NULL {
        return Err(A20_ERR_BAD_HANDLE);
    }
    Ok(args.out_handle)
}

/// user/svc/svcman.c:spawn_echod(), the same shape: fresh channel, install the
/// service endpoint at A20_SVC_ENDPOINT_SLOT, spawn, keep the client side.
fn spawn_echod() -> Result<(Handle, Handle), Status> {
    spawn_service(ECHOD_PATH)
}

fn spawn_service(path: &[u8]) -> Result<(Handle, Handle), Status> {
    let pair = channel_create()?;
    let image = match path_open(path) {
        Ok(h) => h,
        Err(st) => {
            let _ = handle_close(pair[0]);
            let _ = handle_close(pair[1]);
            return Err(st);
        }
    };

    let sh = SpawnHandle {
        handle: pair[1],
        rights: A20_RIGHT_READ | A20_RIGHT_WRITE,
        target_slot: A20_SVC_ENDPOINT_SLOT,
        flags: 0,
    };

    let mut ta = TaskSpawnArgs {
        size: size_of::<TaskSpawnArgs>() as u32,
        version: 2, // v2 args carry the inherited stdio handles
        image,
        root_dir: root(),
        cwd_dir: A20_HANDLE_NULL,
        event_queue: A20_HANDLE_NULL,
        argv: 0,
        envp: 0,
        argc: 0,
        envc: 0,
        handles: &sh as *const SpawnHandle as u64,
        handle_count: 1,
        flags: 0,
        out_task: A20_HANDLE_NULL,
        stdin_handle: A20_HANDLE_NULL,
        stdout_handle: out(),
        stderr_handle: out(),
        reserved: 0,
    };

    // out_task is written back into the caller's struct by the kernel, so the
    // struct must stay addressable across the trap.
    let st = unsafe {
        sys::syscall6(A20_SYS_TASK_SPAWN, &mut ta as *mut TaskSpawnArgs as u64, 0, 0, 0, 0, 0)
    };
    let task = ta.out_task;
    let _ = handle_close(image);
    let _ = handle_close(pair[1]); // the object lives on in the child
    if st < 0 {
        let _ = handle_close(pair[0]);
        return Err(st);
    }
    Ok((pair[0], task))
}

// ===========================================================================
// Reporting (nothing here touches the protocol)
//
// These helpers deliberately avoid slice indexing, `core::str::from_utf8` and
// any other core entry point that lowers to an out-of-line libcore function:
// the peer is linked as a plain object against liba20rt, so the only symbols
// the link is allowed to need from outside this crate are `memcpy`,
// `memmove`, `memset` (user/liba20rt/a20_compiler_rt.c) and
// `a20_get_start_info` (user/svc/rust_peer_crt0.c).  Keeping that set exactly
// equal to what user/svc/echod.c already needs is what lets the peer reuse the
// existing Native svc recipe unchanged.
// ===========================================================================

/// Byte-for-byte equality with no bounds-check path: `a` and `b` are walked
/// by iterator, so no slice indexing and therefore no libcore panic helper is
/// referenced from here.
fn bytes_eq(a: &[u8], b: &[u8]) -> bool {
    if a.len() != b.len() {
        return false;
    }
    for (x, y) in a.iter().zip(b.iter()) {
        if x != y {
            return false;
        }
    }
    true
}

struct Hex {
    buf: [u8; 2 * 64],
    len: usize,
}

impl Hex {
    fn new() -> Self {
        Self {
            buf: [0; 2 * 64],
            len: 0,
        }
    }

    /// Lower-case hex of the first 64 bytes of `data`.
    fn fmt(&mut self, data: &[u8]) -> &[u8] {
        const D: &[u8; 16] = b"0123456789abcdef";
        let mut n = 0usize;
        for b in data.iter().take(64) {
            // `n` is bounded by 2 * 64 and only ever even, so both stores are
            // in range; `set` keeps the verifier from inserting a bounds check
            // (and with it a libcore panic call) here.
            let n1 = n + 1;
            if let Some(slot) = self.buf.get_mut(n) {
                *slot = D[(b >> 4) as usize];
            }
            if let Some(slot) = self.buf.get_mut(n1) {
                *slot = D[(b & 0x0f) as usize];
            }
            n += 2;
        }
        self.len = n;
        // Invariant: len <= buf.len(), and the bytes are ASCII hex digits.
        unsafe { core::slice::from_raw_parts(self.buf.as_ptr(), self.len) }
    }
}

fn put_str(s: &str) {
    put(s.as_bytes());
}

fn put_line(s: &str) {
    put_str(s);
    put(b"\n");
}

/// Print an unsigned decimal.  Digits are emitted into the tail of a fixed
/// buffer so the byte range to print can be described without slicing.
fn put_u64(v: u64) {
    let mut buf = [0u8; 20];
    let mut n = 0usize;
    let mut v = v;
    loop {
        let slot = 20 - 1 - n;
        if let Some(out) = buf.get_mut(slot) {
            *out = b'0' + (v % 10) as u8;
        }
        n += 1;
        v /= 10;
        if v == 0 {
            break;
        }
    }
    let start = 20 - n;
    if let Some(tail) = buf.get(start..) {
        put(tail);
    }
}

fn fail(label: &str, detail: &str) {
    put_str("RUST_NATIVE_SVC: FAIL ");
    put_str(label);
    if !detail.is_empty() {
        put(b" ");
        put_str(detail);
    }
    put(b"\n");
}

fn put_status(what: &str, label: &str, st: Status) {
    put_str("RUST_NATIVE_SVC: FAIL ");
    put_str(label);
    put(b" ");
    put_str(what);
    put_str(" status=");
    put_u64(st as u64);
    put(b"\n");
}

fn wire(label: &str, dir: &str, bytes: &[u8]) {
    let mut hex = Hex::new();
    put_str("RUST_NATIVE_SVC: ");
    put_str(dir);
    put(b" ");
    put_str(label);
    put(b" ");
    put(hex.fmt(bytes));
    put_str(" (");
    put_u64(bytes.len() as u64);
    put_line(" bytes)");
}

/// A vector the live service must echo byte-for-byte.
fn expect_echo(ep: Handle, label: &str, req: &[u8]) -> bool {
    wire(label, "tx", req);
    let mut reply = [0u8; 64];
    let deadline = match monotonic_now_ns() {
        Ok(now) => now.saturating_add(2_000_000_000),
        Err(st) => { put_status("clock_get", label, st); return false; }
    };
    match send_until(ep, label, req, deadline)
        .and_then(|()| recv_until(ep, label, &mut reply, deadline)) {
        Ok(n) => {
            let got = match reply.get(..n) {
                Some(s) => s,
                None => {
                    fail(label, "reply longer than the receive buffer");
                    return false;
                }
            };
            wire(label, "rx", got);
            if got.len() != req.len() || !bytes_eq(got, req) {
                fail(label, "was not echoed byte-for-byte");
                return false;
            }
            true
        }
        Err(st) => {
            if st != A20_ERR_TIMED_OUT { put_status("channel_exchange", label, st); }
            false
        }
    }
}

/// A malformed or unsupported vector must be dropped while the service stays
/// alive. Cancellation is an error here; only expect_peer_exit accepts it.
fn expect_silent(ep: Handle, label: &str, req: &[u8], timeout_ns: u64) -> bool {
    wire(label, "tx", req);
    let start = match monotonic_now_ns() {
        Ok(now) => now,
        Err(st) => { put_status("clock_get", label, st); return false; }
    };
    let deadline = start.saturating_add(timeout_ns);
    if let Err(st) = send_until(ep, label, req, deadline) {
        if st != A20_ERR_TIMED_OUT { put_status("channel_send", label, st); }
        else { fail(label, "send deadline expired"); }
        return false;
    }
    let mut buf = [0u8; 64];
    let mut hex = Hex::new();
    loop {
        match try_recv(ep, &mut buf) {
            Ok(Some(n)) => {
                let got = match buf.get(..n) {
                    Some(s) => s,
                    None => {
                        fail(label, "reply longer than the receive buffer");
                        return false;
                    }
                };
                fail(label, "must be dropped, was echoed");
                put_str("RUST_NATIVE_SVC:   got ");
                put(hex.fmt(got));
                put(b"\n");
                return false;
            }
            Ok(None) => {}
            Err(A20_ERR_CANCELED) => {
                fail(label, "service exited while malformed request was expected to be dropped");
                return false;
            }
            Err(st) => {
                put_status("channel_recv", label, st);
                return false;
            }
        }
        match pause_before_deadline(deadline) {
            Ok(()) => {}
            Err(A20_ERR_TIMED_OUT) => return true,
            Err(st) => { put_status("wait_silent", label, st); return false; }
        }
    }
}

/// The crash vector is the sole case where endpoint cancellation is expected.
/// A bounded deadline distinguishes the intended service exit from a silent
/// hang, and task_wait below verifies the actual exit status.
fn expect_peer_exit(ep: Handle, label: &str, req: &[u8], timeout_ns: u64) -> Option<u64> {
    wire(label, "tx", req);
    let start = match monotonic_now_ns() {
        Ok(now) => now,
        Err(st) => { put_status("clock_get", label, st); return None; }
    };
    let deadline = start.saturating_add(timeout_ns);
    if let Err(st) = send_until(ep, label, req, deadline) {
        put_status("channel_send", label, st);
        return None;
    }
    let mut buf = [0u8; 64];
    loop {
        match try_recv(ep, &mut buf) {
            Err(A20_ERR_CANCELED) => return Some(deadline),
            Ok(Some(_)) => { fail(label, "crash request received a reply"); return None; }
            Ok(None) => {}
            Err(st) => { put_status("channel_recv", label, st); return None; }
        }
        match pause_before_deadline(deadline) {
            Ok(()) => {}
            Err(A20_ERR_TIMED_OUT) => { fail(label, "service did not exit before deadline"); return None; }
            Err(st) => { put_status("wait_exit", label, st); return None; }
        }
    }
}

// ===========================================================================
// Entry point
// ===========================================================================

/// main(), reached from user/svc/rust_peer_crt0.c -> _start_c() exactly as it
/// is for the C programs (user/liba20rt/crt0_a20.h).
#[no_mangle]
pub extern "C" fn main(_argc: c_int, _argv: *const *const u8, _envp: *const *const u8) -> c_int {
    let si = unsafe {
        let p = a20_get_start_info();
        if p.is_null() {
            return EXIT_NO_START_INFO;
        }
        &*p
    };
    if si.stdout_handle == A20_HANDLE_NULL {
        return EXIT_NO_START_INFO;
    }
    OUT_HANDLE.store(si.stdout_handle, Ordering::Relaxed);
    ROOT_DIR.store(si.root_dir, Ordering::Relaxed);

    // The contract this peer is pinned to, printed for the QEMU log.
    put_str("RUST_NATIVE_SVC: idl_version=");
    put_u64(A20_SERVICES_IDL_VERSION as u64);
    put_str(" echo_type=");
    put_u64(SVCMGR_REQ_ECHO as u64);
    put_str(" crash_type=");
    put_u64(SVCMGR_REQ_CRASH as u64);
    put_str(" crash_code=");
    put_u64(A20_SVC_CRASH_CODE as u64);
    put_str(" endpoint_slot=");
    put_u64(A20_SVC_ENDPOINT_SLOT as u64);
    put_str(" service=");
    put(ECHOD_PATH);
    put(b"\n");

    // -- 1. spawn the real C service -------------------------------------
    let (ep, task) = match spawn_echod() {
        Ok(v) => v,
        Err(st) => {
            put_status("task_spawn", "spawn", st);
            return EXIT_FAIL;
        }
    };
    put_line("RUST_NATIVE_SVC: service ready");

    // -- 0. negotiate the unchanged v1 envelope --------------------------
    let legacy_peer = match negotiate_service(ep, 1, 3, 0x1357_9bdf, "negotiate_overlap", false) {
        Ok(NegotiationOutcome::Selected(1)) => false,
        Ok(NegotiationOutcome::LegacyEcho) => {
            put_line("RUST_NATIVE_SVC: legacy v1 echo fallback verified");
            true
        }
        Ok(_) => {
            fail("negotiate_overlap", "server did not select highest common v1");
            retire(task, ep);
            return EXIT_FAIL;
        }
        Err(()) => { retire(task, ep); return EXIT_FAIL; }
    };
    if !legacy_peer {
        match negotiate_service(ep, 2, 3, 0x2468_ace0, "negotiate_no_common", false) {
            Ok(NegotiationOutcome::NoCommon) => {}
            Ok(_) => {
                fail("negotiate_no_common", "server accepted or echoed a disjoint range");
                retire(task, ep);
                return EXIT_FAIL;
            }
            Err(()) => { retire(task, ep); return EXIT_FAIL; }
        }
    }

    // -- 2. exact v1 echo ------------------------------------------------
    let generated_echo = generated_echo_frame();
    if generated_echo != WIRE_ECHO_EXACT {
        fail("echo_exact", "generated IDL codec disagrees with independent v1 bytes");
        retire(task, ep);
        return EXIT_FAIL;
    }
    if !expect_echo(ep, "echo_exact", &generated_echo) {
        retire(task, ep);
        return EXIT_FAIL;
    }

    // -- 3. unknown but valid type is still echoed ------------------------
    if !expect_echo(ep, "echo_unknown_type", &WIRE_ECHO_UNKNOWN_TYPE) {
        retire(task, ep);
        return EXIT_FAIL;
    }

    // -- 4. malformed / unsupported envelopes are dropped -----------------
    if !expect_silent(ep, "drop_version_mismatch", &WIRE_DROP_VERSION_MISMATCH, 200_000_000) {
        retire(task, ep);
        return EXIT_FAIL;
    }
    if !expect_echo(ep, "service_alive_after_version_drop", &generated_echo) {
        retire(task, ep);
        return EXIT_FAIL;
    }
    if !expect_silent(ep, "drop_size_mismatch", &WIRE_DROP_SIZE_MISMATCH, 200_000_000) {
        retire(task, ep);
        return EXIT_FAIL;
    }
    if !expect_echo(ep, "service_alive_after_size_drop", &generated_echo) {
        retire(task, ep);
        return EXIT_FAIL;
    }
    if !expect_silent(ep, "drop_short_message", &WIRE_DROP_SHORT_MESSAGE, 200_000_000) {
        retire(task, ep);
        return EXIT_FAIL;
    }
    if !expect_echo(ep, "service_alive_after_short_drop", &generated_echo) {
        retire(task, ep);
        return EXIT_FAIL;
    }
    put_line("RUST_NATIVE_SVC: malformed envelopes dropped");

    // -- 5. the crash request is not an ordinary echo ---------------------
    let crash_deadline = match expect_peer_exit(ep, "crash_request", &WIRE_CRASH_REQUEST, 3_000_000_000) {
        Some(deadline) => deadline,
        None => { retire(task, ep); return EXIT_FAIL; }
    };

    // -- 6. the service really exited, with A20_SVC_CRASH_CODE -------------
    let mut status = TaskStatus {
        size: 0,
        version: 0,
        exit_code: 0,
        exit_reason: 0,
        utime_ns: 0,
        stime_ns: 0,
    };
    let st = task_wait_until(task, &mut status, crash_deadline);
    if st != A20_OK {
        put_status("task_wait", "crash", st);
        let _ = retire(task, ep);
        return EXIT_FAIL;
    }
    put_str("RUST_NATIVE_SVC: crash detected exit_code=");
    put_u64(status.exit_code as u64);
    put(b"\n");
    if status.exit_code != A20_SVC_CRASH_CODE {
        fail("crash", "unexpected exit code");
        let _ = close_task_and_endpoint(task, ep);
        return EXIT_FAIL;
    }
    if !close_task_and_endpoint(task, ep) {
        fail("crash", "failed to close task/endpoint handles after reap");
        return EXIT_FAIL;
    }

    // -- 7. respawn and prove the service is functional again --------------
    let _ = thread_sleep_ns(50_000_000); // svcman.c's BACKOFF_MS
    let (ep2, task2) = match spawn_echod() {
        Ok(v) => v,
        Err(st) => {
            put_status("task_spawn", "respawn", st);
            return EXIT_FAIL;
        }
    };
    if !expect_echo(ep2, "echo_exact", &WIRE_ECHO_EXACT) {
        retire(task2, ep2);
        return EXIT_FAIL;
    }
    put_line("RUST_NATIVE_SVC: healed");

    // -- 8. clean shutdown, mirroring svcman.c -----------------------------
    if !retire(task2, ep2) {
        fail("shutdown", "service did not terminate and reap cleanly");
        return EXIT_FAIL;
    }

    #[cfg(idl_ack_test)]
    if !run_adversarial_ack_cases() { return EXIT_FAIL; }

    put_line("RUST_NATIVE_SVC: PASS restarts=1");
    EXIT_PASS
}

/// Exercise the production negotiation path against a Rust service that
/// deliberately emits malformed or inconsistent acknowledgments. Each child
/// handles one request and exits, so an accidental downgrade challenge or
/// business echo cannot be mistaken for a successful negative case.
#[cfg(idl_ack_test)]
fn run_adversarial_ack_cases() -> bool {
    // Positive control: an exact legacy echo that does cover v1 must be
    // accepted through the same negotiation path that rejects the malformed
    // ACKs below.  Without it, "legacy fallback" would only be proven for
    // rejection, not acceptance.
    let cases: [(&str, u16, u16, u32, bool); 10] = [
        ("legacy_echo_accepts_v1", 1, 3, 0xa00a_000a, false),
        ("wrong_nonce", 1, 3, 0xa001_0001, true),
        ("selection_outside_overlap", 1, 3, 0xa002_0002, true),
        ("short_frame", 1, 3, 0xa003_0003, true),
        ("trailing_frame", 1, 3, 0xa004_0004, true),
        ("status_selection_conflict", 1, 3, 0xa005_0005, true),
        ("legacy_echo_excludes_v1", 2, 3, 0xa006_0006, true),
        ("unknown_status", 1, 3, 0xa007_0007, true),
        ("unknown_reply_type", 1, 3, 0xa008_0008, true),
        ("unsupported_envelope_version", 1, 3, 0xa009_0009, true),
    ];
    for (label, min_version, max_version, nonce, expected_reject) in cases {
        let (ep, task) = match spawn_service(ACK_ADVERSARY_PATH) {
            Ok(v) => v,
            Err(st) => {
                put_status("task_spawn", label, st);
                return false;
            }
        };
        let outcome = negotiate_service(ep, min_version, max_version,
                                        nonce, label, true);
        if expected_reject {
            if !matches!(outcome, Ok(NegotiationOutcome::Rejected)) {
                fail(label, "malformed acknowledgment was not rejected by negotiation path");
                retire(task, ep);
                return false;
            }
        } else if !matches!(outcome, Ok(NegotiationOutcome::LegacyEcho)) {
            fail(label, "in-range legacy echo was not accepted");
            retire(task, ep);
            return false;
        }
        let deadline = match monotonic_now_ns() {
            Ok(now) => now.saturating_add(2_000_000_000),
            Err(st) => {
                put_status("clock_get", label, st);
                retire(task, ep);
                return false;
            }
        };
        let mut status = TaskStatus {
            size: 0, version: 0, exit_code: 0, exit_reason: 0,
            utime_ns: 0, stime_ns: 0,
        };
        let waited = task_wait_until(task, &mut status, deadline);
        if waited != A20_OK || status.exit_code != 0 {
            fail(label, "adversarial peer did not exit cleanly after one response");
            let _ = retire(task, ep);
            return false;
        }
        if !close_task_and_endpoint(task, ep) {
            fail(label, "failed to close adversarial peer handles");
            return false;
        }
        if expected_reject {
            put_str("IDL_ACK_REJECT: case=");
            put(label.as_bytes());
            put_line(" PASS rejected-before-fallback response_sent=1 cleanup=PASS");
        } else {
            put_str("IDL_ACK_ACCEPT: case=");
            put(label.as_bytes());
            put_line(" PASS accepted-legacy-in-range response_sent=1 cleanup=PASS");
        }
    }
    put_line("IDL_ACK_ADVERSARIAL: PASS cases=10 cleanup=10");
    true
}

/// Terminate the child, prove it was reaped before the deadline, and close
/// both owned handles. A successful parent exit alone is not lifecycle proof.
fn retire(task: Handle, ep: Handle) -> bool {
    let deadline = match monotonic_now_ns() {
        Ok(now) => now.saturating_add(2_000_000_000),
        Err(st) => {
            put_status("clock_get", "retire", st);
            let _ = task_kill(task, A20_SIGKILL);
            let _ = close_task_and_endpoint(task, ep);
            return false;
        }
    };
    let kill_st = task_kill(task, A20_SIGKILL);
    let mut status = TaskStatus {
        size: 0,
        version: 0,
        exit_code: 0,
        exit_reason: 0,
        utime_ns: 0,
        stime_ns: 0,
    };
    let wait_st = task_wait_until(task, &mut status, deadline);
    if wait_st != A20_OK {
        put_status("task_wait", "retire", wait_st);
        if kill_st < A20_OK { put_status("task_kill", "retire", kill_st); }
    } else {
        put_str("RUST_NATIVE_SVC: retired exit_code=");
        put_u64(status.exit_code as u64);
        put(b"\n");
    }
    let close_ok = close_task_and_endpoint(task, ep);
    wait_st == A20_OK && close_ok
}

fn close_task_and_endpoint(task: Handle, ep: Handle) -> bool {
    let ep_st = handle_close(ep);
    let task_st = handle_close(task);
    if ep_st < A20_OK { put_status("handle_close", "endpoint", ep_st); }
    if task_st < A20_OK { put_status("handle_close", "task", task_st); }
    ep_st >= A20_OK && task_st >= A20_OK
}

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    task_exit(EXIT_PANIC)
}
