//! Rust policy and service loop for the supervised Goldfish RTC endpoint.
#![no_std]
#![no_main]

use core::ffi::c_int;
use core::panic::PanicInfo;

#[allow(dead_code)]
mod idl { include!(env!("A20_IDL_RUST_BINDINGS")); }

unsafe extern "C" {
    fn a20_rtcd_init(base: *mut u64, stdout: *mut u32) -> i64;
    fn a20_rtcd_queue_create(queue: *mut u32) -> i64;
    fn a20_rtcd_watch(queue: u32, handle: u32, tag: u64) -> i64;
    fn a20_rtcd_irq_listen(queue: u32, tag: u64) -> i64;
    fn a20_rtcd_recv(endpoint: u32, bytes: *mut u8, length: *mut u32) -> i64;
    fn a20_rtcd_send(endpoint: u32, bytes: *const u8, length: u32) -> i64;
    fn a20_rtcd_wait(queue: u32, tag: *mut u64) -> i64;
    fn a20_rtcd_read_ns(base: u64) -> u64;
    fn a20_rtcd_set_alarm(base: u64, deadline_ns: u64);
    fn a20_rtcd_clear_alarm(base: u64);
    fn a20_rtcd_irq_ack() -> i64;
    fn a20_rtcd_log(handle: u32, bytes: *const u8, length: u32);
}

const HANDLE_NULL: u32 = u32::MAX;
const ERR_WOULD_BLOCK: i64 = -18;
const CH_TAG: u64 = 0x52435443;
const IRQ_TAG: u64 = 0x52435449;

fn log(handle: u32, bytes: &[u8]) { unsafe { a20_rtcd_log(handle, bytes.as_ptr(), bytes.len() as u32) } }

fn reply(endpoint: u32, kind: u16, ns: u64) {
    let body = idl::RTCD_TIME_RESPONSE {
        seconds: ns / 1_000_000_000,
        nanoseconds: ns % 1_000_000_000,
    };
    let payload = idl::rtcd_time_response_encode(&body);
    let env = idl::envelope_encode(&idl::ENVELOPE {
        version: idl::A20_SERVICES_IDL_VERSION as u16,
        r#type: kind,
        size: (idl::ENVELOPE_WIRE_SIZE + payload.len()) as u32,
    });
    let mut frame = [0u8; idl::ENVELOPE_WIRE_SIZE + idl::RTCD_TIME_RESPONSE_WIRE_SIZE];
    frame[..idl::ENVELOPE_WIRE_SIZE].copy_from_slice(&env);
    frame[idl::ENVELOPE_WIRE_SIZE..].copy_from_slice(&payload);
    unsafe { let _ = a20_rtcd_send(endpoint, frame.as_ptr(), frame.len() as u32); }
}

enum Work { Empty, Continue, Crash }

fn drain(endpoint: u32, base: u64, alarm_endpoint: &mut u32, pending: &mut bool) -> Work {
    let mut bytes = [0u8; 64];
    let mut length = bytes.len() as u32;
    let status = unsafe { a20_rtcd_recv(endpoint, bytes.as_mut_ptr(), &mut length) };
    if status == ERR_WOULD_BLOCK { return Work::Empty; }
    if status < 0 { return Work::Empty; }
    let frame = match bytes.get(..length as usize) { Some(f) => f, None => return Work::Continue };
    let header = match frame.get(..idl::ENVELOPE_WIRE_SIZE) { Some(h) => h, None => return Work::Continue };
    let env = match idl::envelope_decode(header) { Some(e) => e, None => return Work::Continue };
    if env.version as u16 != idl::A20_SERVICES_IDL_VERSION as u16 || env.size as usize != frame.len() {
        return Work::Continue;
    }
    match env.r#type {
        x if x == idl::RTCD_REQ_CRASH as u16 => Work::Crash,
        x if x == idl::RTCD_REQ_TIME as u16 => { reply(endpoint, idl::RTCD_REPLY_TIME as u16, unsafe { a20_rtcd_read_ns(base) }); Work::Continue }
        x if x == idl::RTCD_REQ_ALARM as u16 => {
            let payload = match frame.get(idl::ENVELOPE_WIRE_SIZE..) { Some(p) => p, None => return Work::Continue };
            let req = match idl::rtcd_alarm_request_decode(payload) { Some(r) => r, None => return Work::Continue };
            let deadline = unsafe { a20_rtcd_read_ns(base) }.wrapping_add((req.milliseconds as u64).wrapping_mul(1_000_000));
            unsafe { a20_rtcd_set_alarm(base, deadline); }
            *alarm_endpoint = endpoint;
            *pending = true;
            Work::Continue
        }
        _ => Work::Continue,
    }
}

#[no_mangle]
pub extern "C" fn main(_argc: c_int, _argv: *const *const u8, _envp: *const *const u8) -> c_int {
    let mut base = 0u64;
    let mut stdout = HANDLE_NULL;
    if unsafe { a20_rtcd_init(&mut base, &mut stdout) } < 0 || base == 0 { return 2; }
    log(stdout, b"rtcd: start\nrtcd: mmio mapped\n");
    let mut queue = HANDLE_NULL;
    if unsafe { a20_rtcd_queue_create(&mut queue) } < 0 { return 3; }
    let endpoint = idl::A20_RTCD_EP_SLOT as u32;
    if unsafe { a20_rtcd_watch(queue, endpoint, CH_TAG) } < 0 { return 4; }
    log(stdout, b"rtcd: ep watched\n");
    let ping = idl::A20_SVC_PING_SLOT as u32;
    let have_ping = unsafe { a20_rtcd_watch(queue, ping, CH_TAG) } >= 0;
    if unsafe { a20_rtcd_irq_listen(queue, IRQ_TAG) } < 0 { return 5; }
    log(stdout, b"rtcd: irq listening\n");
    let mut pending = false;
    let mut alarm_endpoint = endpoint;
    loop {
        loop { match drain(endpoint, base, &mut alarm_endpoint, &mut pending) { Work::Empty => break, Work::Crash => return 42, Work::Continue => {} } }
        if have_ping {
            loop { match drain(ping, base, &mut alarm_endpoint, &mut pending) { Work::Empty => break, Work::Crash => return 42, Work::Continue => {} } }
        }
        let mut tag = 0u64;
        if unsafe { a20_rtcd_wait(queue, &mut tag) } < 0 { continue; }
        if tag == IRQ_TAG {
            unsafe { a20_rtcd_clear_alarm(base); }
            if pending { reply(alarm_endpoint, idl::RTCD_REPLY_ALARM as u16, unsafe { a20_rtcd_read_ns(base) }); pending = false; }
            unsafe { let _ = a20_rtcd_irq_ack(); }
        }
    }
}

#[panic_handler]
fn panic(_: &PanicInfo<'_>) -> ! { loop { core::hint::spin_loop(); } }
