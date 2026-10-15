//! Rust implementation of the Native service endpoint, paired with the C
//! client fixture in `user/tests/test_native_rust_idl_service.c`.
//!
//! This deliberately implements the existing v1 envelope and its generated
//! IDL codecs.  It is a selectable experiment; C `echod.c` remains the
//! production default.
#![no_std]
#![no_main]

use core::ffi::c_int;
use core::mem::{offset_of, size_of};
use core::panic::PanicInfo;

#[allow(dead_code)]
mod idl {
    include!(env!("A20_IDL_RUST_BINDINGS"));
}

const SYS_TASK_EXIT: u64 = 0x0200;
const SYS_THREAD_SLEEP: u64 = 0x0207;
const SYS_CHANNEL_SEND: u64 = 0x0505;
const SYS_CHANNEL_RECV: u64 = 0x0506;
const HANDLE_NULL: u32 = u32::MAX;
const ERR_WOULD_BLOCK: i64 = -18;
const ERR_CANCELED: i64 = -20;
const MSG_NONBLOCK: u32 = 1;

#[repr(C)]
struct MsgSendArgs {
    size: u32,
    version: u32,
    channel: u32,
    pad: u32,
    data: u64,
    data_len: u32,
    flags: u32,
    handles: u64,
    handle_count: u32,
    transfer_rights: u64,
}

#[repr(C)]
struct MsgRecvArgs {
    size: u32,
    version: u32,
    channel: u32,
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

// These mirror `a20_msg_send_args_t` / `a20_msg_recv_args_t` in the SDK.
// Keeping size and critical offsets pinned prevents silent syscall ABI drift.
const _: () = assert!(size_of::<MsgSendArgs>() == 56);
const _: () = assert!(offset_of!(MsgSendArgs, data) == 16);
const _: () = assert!(offset_of!(MsgSendArgs, handles) == 32);
const _: () = assert!(offset_of!(MsgSendArgs, transfer_rights) == 48);
const _: () = assert!(size_of::<MsgRecvArgs>() == 72);
const _: () = assert!(offset_of!(MsgRecvArgs, data_buf) == 16);
const _: () = assert!(offset_of!(MsgRecvArgs, out_data_len) == 48);
const _: () = assert!(offset_of!(MsgRecvArgs, out_rights_buf) == 64);

extern "C" {
    fn a20_rust_peer_syscall6(nr: u64, a0: u64, a1: u64, a2: u64,
                              a3: u64, a4: u64, a5: u64) -> i64;
}

#[cfg(any(target_arch = "riscv64", target_arch = "riscv32", target_arch = "aarch64", target_arch = "x86_64"))]
unsafe fn syscall6(nr: u64, a0: u64, a1: u64, a2: u64, a3: u64,
                   a4: u64, a5: u64) -> i64 {
    a20_rust_peer_syscall6(nr, a0, a1, a2, a3, a4, a5)
}

#[cfg(not(any(target_arch = "riscv64", target_arch = "riscv32", target_arch = "aarch64", target_arch = "x86_64")))]
compile_error!("Rust IDL service requires a matching Native target");

fn recv(endpoint: u32, bytes: &mut [u8]) -> Result<Option<usize>, i64> {
    let mut args = MsgRecvArgs {
        size: size_of::<MsgRecvArgs>() as u32,
        version: 1,
        channel: endpoint,
        pad: 0,
        data_buf: bytes.as_mut_ptr() as u64,
        data_buf_len: bytes.len() as u32,
        pad2: 0,
        handle_buf: 0,
        handle_buf_count: 0,
        flags: MSG_NONBLOCK,
        out_data_len: 0,
        out_handle_count: 0,
        out_rights_buf: 0,
    };
    let status = unsafe {
        syscall6(SYS_CHANNEL_RECV, &mut args as *mut MsgRecvArgs as u64,
                0, 0, 0, 0, 0)
    };
    if status == ERR_WOULD_BLOCK {
        return Ok(None);
    }
    if status < 0 {
        return Err(status);
    }
    if status as u64 != args.out_data_len || args.out_data_len > bytes.len() as u64 {
        return Err(-12);
    }
    Ok(Some(args.out_data_len as usize))
}

fn send(endpoint: u32, bytes: &[u8]) -> i64 {
    let args = MsgSendArgs {
        size: size_of::<MsgSendArgs>() as u32,
        version: 1,
        channel: endpoint,
        pad: 0,
        data: bytes.as_ptr() as u64,
        data_len: bytes.len() as u32,
        flags: 0,
        handles: 0,
        handle_count: 0,
        transfer_rights: 0,
    };
    unsafe {
        syscall6(SYS_CHANNEL_SEND, &args as *const MsgSendArgs as u64,
                 0, 0, 0, 0, 0)
    }
}

fn sleep_1ms() {
    unsafe { syscall6(SYS_THREAD_SLEEP, 1_000_000, 0, 0, 0, 0, 0); }
}

enum Action { Continue, Crash }

fn process(endpoint: u32, request: &[u8], len: usize) -> Result<Action, i64> {
    let Some(frame) = request.get(..len) else { return Ok(Action::Continue); };
    let Some(header) = frame.get(..idl::ENVELOPE_WIRE_SIZE) else {
        return Ok(Action::Continue);
    };
    let Some(envelope) = idl::envelope_decode(header) else {
        return Ok(Action::Continue);
    };
    if envelope.version != idl::A20_SERVICES_IDL_VERSION || envelope.size as usize != len {
        return Ok(Action::Continue); // Invalid v1 frames are ignored like the C service.
    }
    if envelope.r#type == idl::SVCMGR_REQ_CRASH as u16 {
        return Ok(Action::Crash);
    }
    if envelope.r#type == idl::SVCMGR_REQ_VERSION_NEGOTIATE as u16 {
        let Some(payload) = frame.get(idl::ENVELOPE_WIRE_SIZE..) else {
            return Ok(Action::Continue);
        };
        let Some(input) = idl::svcmgr_version_request_decode(payload) else {
            return Ok(Action::Continue);
        };
        let server_min = 1u16;
        let server_max = 1u16;
        let common_min = core::cmp::max(input.min_version, server_min);
        let common_max = core::cmp::min(input.max_version, server_max);
        let (selected, status) = if input.min_version == 0 || input.min_version > input.max_version {
            (0, idl::SVCMGR_VERSION_NEGOTIATE_INVALID_RANGE as u16)
        } else if common_min > common_max {
            (0, idl::SVCMGR_VERSION_NEGOTIATE_NO_COMMON as u16)
        } else {
            (common_max, idl::SVCMGR_VERSION_NEGOTIATE_OK as u16)
        };
        let response = idl::SVCMGR_VERSION_RESPONSE {
            min_version: server_min,
            max_version: server_max,
            selected_version: selected,
            status,
            nonce: input.nonce,
        };
        let payload = idl::svcmgr_version_response_encode(&response);
        let env = idl::envelope_encode(&idl::ENVELOPE {
            version: idl::A20_SERVICES_IDL_VERSION,
            r#type: idl::SVCMGR_REP_VERSION_NEGOTIATE as u16,
            size: (idl::ENVELOPE_WIRE_SIZE + payload.len()) as u32,
        });
        let mut reply = [0u8; idl::ENVELOPE_WIRE_SIZE + idl::SVCMGR_VERSION_RESPONSE_WIRE_SIZE];
        reply[..idl::ENVELOPE_WIRE_SIZE].copy_from_slice(&env);
        reply[idl::ENVELOPE_WIRE_SIZE..].copy_from_slice(&payload);
        let sent = send(endpoint, &reply);
        if sent < 0 { return Err(sent); }
        return Ok(Action::Continue);
    }
    // Preserve echod's established rule: valid non-crash requests, including
    // unknown message types, are echoed byte-for-byte.
    let sent = send(endpoint, frame);
    if sent < 0 { return Err(sent); }
    Ok(Action::Continue)
}

#[no_mangle]
pub extern "C" fn main(_argc: c_int, _argv: *const *const u8,
                       _envp: *const *const u8) -> c_int {
    let endpoint = idl::A20_SVC_ENDPOINT_SLOT as u32;
    let ping_endpoint = idl::A20_SVC_PING_SLOT as u32;
    if endpoint == HANDLE_NULL { return 90; }
    let mut bytes = [0u8; 64];
    loop {
        let mut did_work = false;
        for ep in [ping_endpoint, endpoint] {
            if ep == HANDLE_NULL { continue; }
            let is_service_endpoint = ep == endpoint;
            match recv(ep, &mut bytes) {
                Ok(Some(len)) => {
                    did_work = true;
                    match process(ep, &bytes, len) {
                        Ok(Action::Continue) => {}
                        // C echod's optional ping pump consumes CRASH but its
                        // caller ignores the positive return value; only the
                        // service endpoint terminates on that message.
                        Ok(Action::Crash) if is_service_endpoint => {
                            return idl::A20_SVC_CRASH_CODE as c_int;
                        }
                        Ok(Action::Crash) => {}
                        // The C ping pump deliberately ignores every channel
                        // error. The main endpoint's closed/revoked state is
                        // a normal clean shutdown; send failures use the same
                        // clean-shutdown policy as C echod.
                        Err(_) if !is_service_endpoint => {}
                        Err(_) => return 0,
                    }
                }
                Ok(None) => {}
                // Ping is optional and its receive errors are non-fatal.
                Err(_) if !is_service_endpoint => {}
                Err(ERR_CANCELED) => return 0,
                Err(_) => return 0,
            }
        }
        if !did_work { sleep_1ms(); }
    }
}

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    unsafe { syscall6(SYS_TASK_EXIT, 200, 0, 0, 0, 0, 0); }
    loop { core::hint::spin_loop(); }
}
