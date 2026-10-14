//! Rust-owned user filesystem service lifecycle and UFS request dispatch.
//! Filesystem algorithms and sector primitives remain in the C backend layer.
#![no_std]
#![no_main]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;

#[path = "ufsd_protocol.rs"]
mod protocol;
#[allow(dead_code)]
mod idl { include!(env!("A20_IDL_RUST_BINDINGS")); }

type Handle = u32;
const HANDLE_NULL: Handle = u32::MAX;
const ERR_WOULD_BLOCK: i64 = -18;
const ERR_INVALID: i32 = -22;
const ERR_IO: i32 = -5;
const EVENT_MESSAGE_READY: u64 = 1 << 8;
const EVENT_PEER_CLOSED: u64 = 1 << 9;
const FS_EVENT_TAG: u64 = 1;
const SERVICE_EVENT_TAG: u64 = 2;
const BACKEND_FAT: u32 = 1;
const BACKEND_VNFS: u32 = 2;

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
    if number == 0 { digits[count] = b'0'; count = 1; }
    while number != 0 { digits[count] = b'0' + (number % 10) as u8; count += 1; number /= 10; }
    let mut encoded = [0u8; 10];
    for i in 0..count { encoded[i] = digits[count - i - 1]; }
    log(handle, &encoded[..count]);
    log(handle, suffix);
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

fn dispatch_request(
    backend: u32,
    request: &protocol::Request,
    name: *const c_char,
    payload: *const u8,
    rename_target: *const c_char,
    response: &mut protocol::Response,
) -> i64 {
    if backend != BACKEND_FAT && backend != BACKEND_VNFS { return -(ERR_INVALID as i64); }
    unsafe {
        match request.opcode {
            protocol::OP_INIT => { response.out0 = protocol::ROOT_INO; 0 }
            protocol::OP_LOOKUP => a20_ufsd_backend_lookup(backend, request.ino, name,
                                                            request.name_len, response),
            protocol::OP_GETATTR => a20_ufsd_backend_getattr(backend, request.ino, response),
            protocol::OP_READDIR => a20_ufsd_backend_readdir(backend, request.ino,
                                                              request.arg0, response),
            protocol::OP_CREATE => a20_ufsd_backend_create(backend, request.ino, name,
                                                            request.arg0 as u32, response),
            protocol::OP_MKDIR => a20_ufsd_backend_mkdir(backend, request.ino, name, response),
            protocol::OP_UNLINK => a20_ufsd_backend_unlink(backend, request.ino, name),
            protocol::OP_RMDIR => a20_ufsd_backend_rmdir(backend, request.ino, name),
            protocol::OP_TRUNCATE => a20_ufsd_backend_truncate(backend, request.ino, request.arg0),
            protocol::OP_READ => a20_ufsd_backend_read(backend, request.ino, request.arg0,
                                                        request.arg1 as u32, response),
            protocol::OP_WRITE => a20_ufsd_backend_write(backend, request.ino, request.arg0,
                                                          payload, request.payload_len, response),
            protocol::OP_RENAME => a20_ufsd_backend_rename(backend, request.ino, name,
                request.arg0, rename_target, response),
            protocol::OP_SYNC => a20_ufsd_backend_sync(backend),
            protocol::OP_STATFS => { a20_ufsd_backend_statfs(backend, response); 0 }
            _ => -(38i64),
        }
    }
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
    loop {
        if service_watch {
            loop {
                let result = unsafe { pump_supervision(service_endpoint, output) };
                if result == 2 { return 42; }
                if result <= 0 { break; }
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
        if status < 0 { return 0; }
        let header_len = core::mem::size_of::<protocol::Request>();
        if (length as usize) < header_len { continue; }
        let request = unsafe { core::ptr::read_unaligned(bytes.as_ptr().cast::<protocol::Request>()) };
        if request.magic != protocol::REQ_MAGIC || request.version != protocol::PROTO_VERSION { continue; }
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
        let result = dispatch_request(backend, &request, name_pointer,
            payload_bytes.as_ptr(), rename_pointer, &mut response);
        response.status = result.clamp(i32::MIN as i64, i32::MAX as i64) as i32;
        let cap = unsafe { a20_ufsd_payload_capacity() } as usize;
        if response.payload_len as usize > cap {
            response.status = -(ERR_IO);
            response.payload_len = 0;
        }
        let _ = payload_pointer;
        if unsafe { a20_ufsd_send(filesystem.raw(), &response) } < 0 { return 0; }
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
        log(output, b"UFSD: mount backend failed\n"); return 1;
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
