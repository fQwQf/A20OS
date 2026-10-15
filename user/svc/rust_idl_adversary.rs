//! One-shot adversarial IDL peer used by the N04 guest test.
//! It receives one real v1 negotiation request and responds according to its
//! nonce with a malformed/inconsistent acknowledgment or an exact legacy echo.
#![no_std]
#![no_main]

use core::ffi::c_int;
use core::mem::{offset_of, size_of};
use core::panic::PanicInfo;

mod idl { include!(env!("A20_IDL_RUST_BINDINGS")); }

const SYS_TASK_EXIT: u64 = 0x0200;
const SYS_THREAD_SLEEP: u64 = 0x0207;
const SYS_CLOCK_GET: u64 = 0x0700;
const SYS_CHANNEL_SEND: u64 = 0x0505;
const SYS_CHANNEL_RECV: u64 = 0x0506;
const HANDLE_NULL: u32 = u32::MAX;
const ERR_WOULD_BLOCK: i64 = -18;
const MSG_NONBLOCK: u32 = 1;

#[repr(C)]
struct MsgSendArgs {
    size: u32, version: u32, channel: u32, pad: u32,
    data: u64, data_len: u32, flags: u32, handles: u64,
    handle_count: u32, transfer_rights: u64,
}
#[repr(C)]
struct MsgRecvArgs {
    size: u32, version: u32, channel: u32, pad: u32,
    data_buf: u64, data_buf_len: u32, pad2: u32,
    handle_buf: u64, handle_buf_count: u32, flags: u32,
    out_data_len: u64, out_handle_count: u32, out_rights_buf: u64,
}
const _: () = assert!(size_of::<MsgSendArgs>() == 56);
const _: () = assert!(offset_of!(MsgSendArgs, data) == 16);
const _: () = assert!(size_of::<MsgRecvArgs>() == 72);
const _: () = assert!(offset_of!(MsgRecvArgs, out_data_len) == 48);

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
compile_error!("IDL adversary requires an installed matching Native target");

fn sleep_1ms() { unsafe { syscall6(SYS_THREAD_SLEEP, 1_000_000, 0, 0, 0, 0, 0); } }

fn now_ns() -> Result<u64, i64> {
    let mut ns = 0u64;
    let st = unsafe { syscall6(SYS_CLOCK_GET, 1, &mut ns as *mut u64 as u64, 0, 0, 0, 0) };
    if st < 0 { Err(st) } else { Ok(ns) }
}

fn recv(endpoint: u32, buf: &mut [u8]) -> Result<Option<usize>, i64> {
    let mut args = MsgRecvArgs {
        size: size_of::<MsgRecvArgs>() as u32, version: 1,
        channel: endpoint, pad: 0, data_buf: buf.as_mut_ptr() as u64,
        data_buf_len: buf.len() as u32, pad2: 0, handle_buf: 0,
        handle_buf_count: 0, flags: MSG_NONBLOCK, out_data_len: 0,
        out_handle_count: 0, out_rights_buf: 0,
    };
    let st = unsafe { syscall6(SYS_CHANNEL_RECV, &mut args as *mut _ as u64, 0, 0, 0, 0, 0) };
    if st == ERR_WOULD_BLOCK { return Ok(None); }
    if st < 0 { return Err(st); }
    if st as u64 != args.out_data_len || args.out_data_len > buf.len() as u64 {
        return Err(-12);
    }
    Ok(Some(args.out_data_len as usize))
}

fn send(endpoint: u32, data: &[u8]) -> i64 {
    let args = MsgSendArgs {
        size: size_of::<MsgSendArgs>() as u32, version: 1,
        channel: endpoint, pad: 0, data: data.as_ptr() as u64,
        data_len: data.len() as u32, flags: 0, handles: 0,
        handle_count: 0, transfer_rights: 0,
    };
    unsafe { syscall6(SYS_CHANNEL_SEND, &args as *const _ as u64, 0, 0, 0, 0, 0) }
}

fn response(nonce: u32, client_min: u16, client_max: u16,
            out: &mut [u8; 21]) -> usize {
    let request = idl::SVCMGR_VERSION_REQUEST {
        min_version: client_min, max_version: client_max, nonce,
    };
    let legacy = nonce == 0xa006_0006;
    if legacy {
        let env = idl::envelope_encode(&idl::ENVELOPE {
            version: idl::A20_SERVICES_IDL_VERSION,
            r#type: idl::SVCMGR_REQ_VERSION_NEGOTIATE as u16,
            size: (idl::ENVELOPE_WIRE_SIZE + idl::SVCMGR_VERSION_REQUEST_WIRE_SIZE) as u32,
        });
        out[..env.len()].copy_from_slice(&env);
        let req = idl::svcmgr_version_request_encode(&request);
        out[env.len()..env.len()+req.len()].copy_from_slice(&req);
        return env.len()+req.len();
    }

    let (selected, status, reply_nonce) = match nonce {
        0xa001_0001 => (1, idl::SVCMGR_VERSION_NEGOTIATE_OK as u16, nonce ^ 1),
        0xa002_0002 => (2, idl::SVCMGR_VERSION_NEGOTIATE_OK as u16, nonce),
        0xa005_0005 => (1, idl::SVCMGR_VERSION_NEGOTIATE_NO_COMMON as u16, nonce),
        // The wire schema defines no reserved/flag bits in this response;
        // exercise an unknown status value instead of inventing a bitfield.
        0xa007_0007 => (0, 0x8001, nonce),
        _ => (1, idl::SVCMGR_VERSION_NEGOTIATE_OK as u16, nonce),
    };
    let payload = idl::svcmgr_version_response_encode(&idl::SVCMGR_VERSION_RESPONSE {
        min_version: 1, max_version: 1, selected_version: selected,
        status, nonce: reply_nonce,
    });
    let is_short = nonce == 0xa003_0003;
    let is_trailing = nonce == 0xa004_0004;
    let frame_len = idl::ENVELOPE_WIRE_SIZE + payload.len();
    let advertised_len = if is_short { frame_len - 1 } else if is_trailing { frame_len + 1 } else { frame_len };
    let reply_type = if nonce == 0xa008_0008 {
        0x7b // unknown message type: not a negotiation response
    } else {
        idl::SVCMGR_REP_VERSION_NEGOTIATE as u16
    };
    let reply_version = if nonce == 0xa009_0009 {
        2 // unsupported envelope version
    } else {
        idl::A20_SERVICES_IDL_VERSION
    };
    let env = idl::envelope_encode(&idl::ENVELOPE {
        version: reply_version,
        r#type: reply_type,
        size: advertised_len as u32,
    });
    out[..env.len()].copy_from_slice(&env);
    out[env.len()..env.len()+payload.len()].copy_from_slice(&payload);
    if is_short { frame_len - 1 } else if is_trailing { out[frame_len] = 0xee; frame_len + 1 } else { frame_len }
}

#[no_mangle]
pub extern "C" fn main(_argc: c_int, _argv: *const *const u8,
                       _envp: *const *const u8) -> c_int {
    let endpoint = idl::A20_SVC_ENDPOINT_SLOT as u32;
    if endpoint == HANDLE_NULL { return 80; }
    let mut request_buf = [0u8; 64];
    let deadline = match now_ns() { Ok(now) => now.saturating_add(2_000_000_000), Err(_) => return 88 };
    let request_len = loop {
        match recv(endpoint, &mut request_buf) {
            Ok(Some(n)) => break n,
            Ok(None) => {
                match now_ns() {
                    Ok(now) if now < deadline => sleep_1ms(),
                    _ => return 89,
                }
            }
            Err(_) => return 81,
        }
    };
    let Some(env_bytes) = request_buf.get(..idl::ENVELOPE_WIRE_SIZE) else { return 82; };
    let Some(env) = idl::envelope_decode(env_bytes) else { return 83; };
    if env.version != idl::A20_SERVICES_IDL_VERSION ||
       env.r#type != idl::SVCMGR_REQ_VERSION_NEGOTIATE as u16 ||
       env.size as usize != request_len { return 84; }
    let Some(payload) = request_buf.get(idl::ENVELOPE_WIRE_SIZE..request_len) else { return 85; };
    let Some(req) = idl::svcmgr_version_request_decode(payload) else { return 86; };
    let mut reply = [0u8; 21];
    let reply_len = response(req.nonce, req.min_version, req.max_version, &mut reply);
    if send(endpoint, &reply[..reply_len]) < 0 { return 87; }
    0
}

#[panic_handler]
fn panic(_: &PanicInfo) -> ! {
    unsafe { syscall6(SYS_TASK_EXIT, 200, 0, 0, 0, 0, 0); }
    loop { core::hint::spin_loop(); }
}
