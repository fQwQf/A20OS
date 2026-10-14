//! Selectable Rust implementation of the system service supervisor.
//!
//! Rust owns the manifest service records, generation-tagged watches,
//! registry publication decision, health state, restart budget and retirement
//! transitions. `svcmgr_rust_abi.c` contains only Native SDK/syscall adapters
//! and the spawn resource transaction.
#![cfg_attr(not(test), no_std)]
#![cfg_attr(not(test), no_main)]

use core::ffi::{c_char, c_int};
#[cfg(not(test))]
use core::panic::PanicInfo;

#[path = "svcmgr_lifecycle.rs"]
mod lifecycle;
#[allow(dead_code)]
mod idl {
    include!(env!("A20_IDL_RUST_BINDINGS"));
}

type Handle = u32;
const A20_OK: i64 = 0;
const HANDLE_NULL: Handle = u32::MAX;
const EXIT_FAIL: c_int = 1;
const SIGKILL: u32 = 9; // verified Native task-kill signal ABI
const WOULD_BLOCK: i64 = -18;
const REGISTRY_TAG: u64 = 0x5253_5643_0000_0001;
const REG_OP_LOOKUP: u32 = 1;
const EVENT_EXITED: u32 = 7;
const EVENT_MESSAGE_READY: u32 = 8;
const BACKOFF_MS: u64 = 50;

#[repr(C)]
#[derive(Clone, Copy)]
struct Event {
    user_data: u64,
    data0: u64,
}

extern "C" {
    fn a20_svcmgr_startup(out_stdout: *mut Handle, out_root: *mut Handle) -> i64;
    fn a20_svcmgr_registry_claim() -> i64;
    fn a20_svcmgr_event_queue_create(out_queue: *mut Handle) -> i64;
    fn a20_svcmgr_event_watch(queue: Handle, target: Handle, mask: u64, user_data: u64) -> i64;
    fn a20_svcmgr_event_cancel(queue: Handle, target: Handle) -> i64;
    fn a20_svcmgr_event_wait(queue: Handle, timeout_ns: u64, out_event: *mut Event) -> i64;
    fn a20_svcmgr_channel_create(out_endpoints: *mut Handle) -> i64;
    fn a20_svcmgr_path_open(path: *const c_char, out_image: *mut Handle) -> i64;
    fn a20_svcmgr_task_spawn(
        image: Handle,
        root: Handle,
        argv: *const *const c_char,
        argc: u32,
        service_child: Handle,
        service_slot: u32,
        ping_child: Handle,
        ping_slot: u32,
        stdout_handle: Handle,
        out_task: *mut Handle,
    ) -> i64;
    fn a20_svcmgr_channel_send(endpoint: Handle, bytes: *const u8, length: u32) -> i64;
    fn a20_svcmgr_registry_recv(
        endpoint: Handle,
        bytes: *mut u8,
        capacity: u32,
        out_length: *mut u32,
        handles: *mut Handle,
        handle_capacity: u32,
        out_handle_count: *mut u32,
    ) -> i64;
    fn a20_svcmgr_registry_reply(
        endpoint: Handle,
        status: i64,
        service_endpoint: Handle,
        has_endpoint: u32,
    ) -> i64;
    fn a20_svcmgr_close(handle: Handle) -> i64;
    fn a20_svcmgr_task_kill(task: Handle, signal: u32) -> i64;
    fn a20_svcmgr_task_wait(task: Handle, out_exit_code: *mut i32) -> i64;
    fn a20_svcmgr_now_ms() -> u64;
    fn a20_svcmgr_sleep_ms(milliseconds: u64) -> i64;
    fn a20_svcmgr_write(output: Handle, bytes: *const u8, length: u32);
    fn a20_svcmgr_read_cmdline(
        out_bytes: *mut u8,
        capacity: u32,
        out_length: *mut u32,
    ) -> i64;
    fn a20_svcmgr_panic_exit(code: i32) -> !;
}

#[derive(Clone, Copy)]
struct Definition {
    name: &'static [u8],
    path: &'static [u8],
    endpoint_slot: u32,
    ping_kind: u16,
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
struct UfsdBlockConfig {
    index: u32,
    from_cmdline: bool,
}

const DEFAULT_UFSD_BLOCK: u32 = 1;
const MAX_UFSD_BLOCK: u32 = 15;

/// Parse the manager-owned block-device policy from raw `/proc/cmdline` bytes.
/// C only reads the file; checked decimal parsing and the accepted range live here.
fn parse_ufsd_block_config(command_line: &[u8]) -> UfsdBlockConfig {
    const KEY: &[u8] = b"a20.ufsd_blk=";
    let mut config = UfsdBlockConfig {
        index: DEFAULT_UFSD_BLOCK,
        from_cmdline: false,
    };
    let mut cursor = 0;
    while cursor < command_line.len() {
        while command_line
            .get(cursor)
            .map_or(false, |byte| matches!(*byte, b' ' | b'\t' | b'\n'))
        {
            cursor += 1;
        }
        let start = cursor;
        while command_line
            .get(cursor)
            .map_or(false, |byte| !matches!(*byte, b' ' | b'\t' | b'\n'))
        {
            cursor += 1;
        }
        if start == cursor {
            break;
        }
        if let Some(index) = parse_ufsd_block_token(command_line, start, cursor, KEY) {
            config.index = index;
            config.from_cmdline = true;
        }
    }
    config
}

fn parse_ufsd_block_token(bytes: &[u8], start: usize, end: usize, key: &[u8]) -> Option<u32> {
    let value_start = start.checked_add(key.len())?;
    if value_start >= end {
        return None;
    }
    for (offset, expected) in key.iter().enumerate() {
        if bytes.get(start.checked_add(offset)?).copied() != Some(*expected) {
            return None;
        }
    }
    let mut value = 0u32;
    for cursor in value_start..end {
        let byte = bytes.get(cursor).copied()?;
        if !byte.is_ascii_digit() {
            return None;
        }
        value = value.checked_mul(10)?.checked_add((byte - b'0') as u32)?;
        if value > MAX_UFSD_BLOCK {
            return None;
        }
    }
    (value != 0).then_some(value)
}

fn load_ufsd_block_config() -> (UfsdBlockConfig, i64) {
    let mut bytes = [0u8; 512];
    let mut length = 0u32;
    let status = unsafe {
        a20_svcmgr_read_cmdline(bytes.as_mut_ptr(), bytes.len() as u32, &mut length)
    };
    let config = if status < 0 {
        UfsdBlockConfig {
            index: DEFAULT_UFSD_BLOCK,
            from_cmdline: false,
        }
    } else {
        let length = (length as usize).min(bytes.len());
        parse_ufsd_block_config(&bytes[..length])
    };
    (config, status)
}

struct Service {
    def: Definition,
    task: OwnedHandle,
    client_ep: OwnedHandle,
    ping_ep: OwnedHandle,
    restart_after_retire: bool,
    retired_exit_code: Option<i32>,
    life: lifecycle::Lifecycle,
}

#[derive(Clone, Copy)]
struct PendingReply {
    status: i64,
    service_index: u8,
    generation: u32,
}

impl PendingReply {
    const EMPTY: Self = Self {
        status: -1,
        service_index: 0,
        generation: 0,
    };
}

struct ReplyQueue {
    entries: [PendingReply; 16],
    head: usize,
    len: usize,
}

impl ReplyQueue {
    const fn new() -> Self {
        Self {
            entries: [PendingReply::EMPTY; 16],
            head: 0,
            len: 0,
        }
    }

    fn push(&mut self, reply: PendingReply) -> bool {
        if self.len == self.entries.len() {
            return false;
        }
        let tail = (self.head + self.len) % self.entries.len();
        self.entries[tail] = reply;
        self.len += 1;
        true
    }

    fn front(&self) -> Option<PendingReply> {
        if self.len == 0 {
            None
        } else {
            Some(self.entries[self.head])
        }
    }

    fn pop(&mut self) {
        if self.len == 0 {
            return;
        }
        self.entries[self.head] = PendingReply::EMPTY;
        self.head = (self.head + 1) % self.entries.len();
        self.len -= 1;
    }
}

impl Service {
    const fn new(def: Definition) -> Self {
        Self {
            def,
            task: OwnedHandle::none(),
            client_ep: OwnedHandle::none(),
            ping_ep: OwnedHandle::none(),
            restart_after_retire: false,
            retired_exit_code: None,
            life: lifecycle::Lifecycle::boot(),
        }
    }
}

/// A manager-owned handle. Ownership moves into Service only after both
/// generation event watches have been installed successfully.
struct OwnedHandle(Handle);

impl OwnedHandle {
    const fn none() -> Self {
        Self(HANDLE_NULL)
    }
    fn new(handle: Handle) -> Self {
        Self(handle)
    }
    fn raw(&self) -> Handle {
        self.0
    }
    fn take(&mut self) -> Handle {
        let handle = self.0;
        self.0 = HANDLE_NULL;
        handle
    }

    fn close(&mut self) -> i64 {
        if self.0 == HANDLE_NULL {
            return 0;
        }
        let status = unsafe { a20_svcmgr_close(self.0) };
        if status >= 0 {
            self.0 = HANDLE_NULL;
        }
        status
    }
}

impl Drop for OwnedHandle {
    fn drop(&mut self) {
        if self.0 != HANDLE_NULL {
            unsafe {
                let _ = a20_svcmgr_close(self.0);
            }
            self.0 = HANDLE_NULL;
        }
    }
}

struct SpawnAttempt {
    image: OwnedHandle,
    task: OwnedHandle,
    service_parent: OwnedHandle,
    service_child: OwnedHandle,
    ping_parent: OwnedHandle,
    ping_child: OwnedHandle,
}

impl SpawnAttempt {
    const fn new() -> Self {
        Self {
            image: OwnedHandle::none(),
            task: OwnedHandle::none(),
            service_parent: OwnedHandle::none(),
            service_child: OwnedHandle::none(),
            ping_parent: OwnedHandle::none(),
            ping_child: OwnedHandle::none(),
        }
    }
}

fn write(output: Handle, text: &[u8]) {
    unsafe {
        a20_svcmgr_write(output, text.as_ptr(), text.len() as u32);
    }
}

fn write_u64(output: Handle, value: u64) {
    // u64 needs at most 20 decimal digits; fill from the tail without Rust
    // slice indexing so the freestanding link does not need core's hosted
    // bounds-panic formatter.
    let mut bytes = [0u8; 20];
    let mut cursor = bytes.len();
    let mut n = value;
    if n == 0 {
        cursor -= 1;
        unsafe { bytes.as_mut_ptr().add(cursor).write(b'0') };
    }
    while n != 0 {
        cursor -= 1;
        unsafe { bytes.as_mut_ptr().add(cursor).write(b'0' + (n % 10) as u8) };
        n /= 10;
    }
    let digits = unsafe {
        core::slice::from_raw_parts(bytes.as_ptr().add(cursor), bytes.len() - cursor)
    };
    write(output, digits);
}

fn log_status(output: Handle, prefix: &[u8], status: i64) {
    write(output, prefix);
    write(output, b" st=-");
    write_u64(output, status.unsigned_abs());
    write(output, b"\n");
}

fn log_named(output: Handle, prefix: &[u8], service: &Service) {
    write(output, prefix);
    write(output, service.def.name);
    write(output, b"\n");
}

fn decimal_arg(buf: &mut [u8; 12], value: u32) -> *const c_char {
    buf.fill(0);
    let mut digits = [0u8; 10];
    let mut count = 0usize;
    let mut n = value;
    if n == 0 {
        // A u32 has at most ten decimal digits, so every write below is
        // within this fixed buffer. Raw pointer access keeps freestanding
        // binaries independent of Rust's hosted bounds-panic formatter.
        unsafe { digits.as_mut_ptr().write(b'0') };
        count = 1;
    } else {
        while n != 0 {
            unsafe { digits.as_mut_ptr().add(count).write(b'0' + (n % 10) as u8) };
            count += 1;
            n /= 10;
        }
    }
    for i in 0..count {
        unsafe {
            buf.as_mut_ptr().add(i).write(
                digits.as_ptr().add(count - i - 1).read(),
            );
        }
    }
    unsafe { buf.as_mut_ptr().add(count).write(0) };
    buf.as_ptr().cast()
}

fn spawn_one(
    service: &mut Service,
    root: Handle,
    output: Handle,
    block_index: u32,
    queue: Handle,
) -> i64 {
    let Some(generation) = service.life.next_generation() else {
        service.life.state = lifecycle::State::Failed;
        return -1;
    };
    let mut block_buf = [0u8; 12];
    let path = service.def.path.as_ptr().cast::<c_char>();
    let mut argv = [core::ptr::null::<c_char>(); 4];
    argv[0] = path;
    let argc = if service.def.ping_kind == 2 {
        argv[1] = b"/ufs\0".as_ptr().cast();
        argv[2] = decimal_arg(&mut block_buf, block_index);
        argv[3] = b"fat\0".as_ptr().cast();
        4
    } else {
        1
    };

    let mut attempt = SpawnAttempt::new();
    let mut pair = [HANDLE_NULL; 2];
    let status = unsafe { a20_svcmgr_channel_create(pair.as_mut_ptr()) };
    if status < 0 {
        return status;
    }
    attempt.service_parent = OwnedHandle::new(pair[0]);
    attempt.service_child = OwnedHandle::new(pair[1]);
    pair = [HANDLE_NULL; 2];
    let status = unsafe { a20_svcmgr_channel_create(pair.as_mut_ptr()) };
    if status < 0 {
        return status;
    }
    attempt.ping_parent = OwnedHandle::new(pair[0]);
    attempt.ping_child = OwnedHandle::new(pair[1]);
    let mut image = HANDLE_NULL;
    let status = unsafe { a20_svcmgr_path_open(path, &mut image) };
    if status < 0 {
        return status;
    }
    attempt.image = OwnedHandle::new(image);
    let mut task = HANDLE_NULL;
    let status = unsafe {
        a20_svcmgr_task_spawn(
            attempt.image.raw(),
            root,
            argv.as_ptr(),
            argc,
            attempt.service_child.raw(),
            service.def.endpoint_slot,
            attempt.ping_child.raw(),
            idl::A20_SVC_PING_SLOT as u32,
            output,
            &mut task,
        )
    };
    if status < 0 {
        return status;
    }
    attempt.task = OwnedHandle::new(task);

    let exit_tag = lifecycle::exit_tag(service_index(service.def.name), generation);
    let ping_tag = lifecycle::pong_tag(service_index(service.def.name), generation);
    let watch_exit = unsafe { a20_svcmgr_event_watch(queue, task, 1u64 << EVENT_EXITED, exit_tag) };
    if watch_exit < 0 {
        if !retire_attempt(&mut attempt, queue, output) {
            adopt_retiring_attempt(service, &mut attempt);
        }
        return watch_exit;
    }
    let watch_ping = unsafe {
        a20_svcmgr_event_watch(
            queue,
            attempt.ping_parent.raw(),
            1u64 << EVENT_MESSAGE_READY,
            ping_tag,
        )
    };
    if watch_ping < 0 {
        if !retire_attempt(&mut attempt, queue, output) {
            adopt_retiring_attempt(service, &mut attempt);
        }
        return watch_ping;
    }
    if !service
        .life
        .spawned(generation, unsafe { a20_svcmgr_now_ms() })
    {
        if !retire_attempt(&mut attempt, queue, output) {
            adopt_retiring_attempt(service, &mut attempt);
        }
        return -1;
    }
    service.task = OwnedHandle::new(attempt.task.take());
    service.client_ep = OwnedHandle::new(attempt.service_parent.take());
    service.ping_ep = OwnedHandle::new(attempt.ping_parent.take());
    service.retired_exit_code = None;
    // Child endpoints and image remain owned by attempt and close on return.
    A20_OK
}

fn retire_attempt(attempt: &mut SpawnAttempt, queue: Handle, output: Handle) -> bool {
    if attempt.task.raw() == HANDLE_NULL {
        return true;
    }
    let status = unsafe { a20_svcmgr_task_kill(attempt.task.raw(), SIGKILL) };
    if status < 0 {
        log_status(output, b"SVC_MGR: spawn rollback kill failed", status);
        return false;
    }
    let mut code = 0;
    match unsafe { a20_svcmgr_task_wait(attempt.task.raw(), &mut code) } {
        1 => {
            let _ = unsafe { a20_svcmgr_event_cancel(queue, attempt.task.raw()) };
            true
        }
        0 => false,
        status => {
            log_status(output, b"SVC_MGR: spawn rollback wait failed", status);
            false
        }
    }
}

fn adopt_retiring_attempt(service: &mut Service, attempt: &mut SpawnAttempt) {
    service.task = OwnedHandle::new(attempt.task.take());
    service.client_ep = OwnedHandle::new(attempt.service_parent.take());
    service.ping_ep = OwnedHandle::new(attempt.ping_parent.take());
    service.life.retirement_started();
    service.restart_after_retire = false;
    service.retired_exit_code = None;
}

fn poll_retiring(
    index: usize,
    service: &mut Service,
    queue: Handle,
    root: Handle,
    output: Handle,
    block_index: u32,
) {
    if !matches!(
        service.life.state,
        lifecycle::State::Retiring | lifecycle::State::Dead | lifecycle::State::Failed
    ) {
        return;
    }
    if service.task.raw() == HANDLE_NULL {
        retry_confirmed_retirement(index, service, queue, root, output, block_index);
        return;
    }
    let mut exit_code = 0;
    match unsafe { a20_svcmgr_task_wait(service.task.raw(), &mut exit_code) } {
        1 => {
            let _ = unsafe { a20_svcmgr_event_cancel(queue, service.task.raw()) };
            let _ = unsafe { a20_svcmgr_event_cancel(queue, service.ping_ep.raw()) };
            if !observe_exit_poll(service, 1, exit_code) {
                return;
            }
            retry_confirmed_retirement(index, service, queue, root, output, block_index);
        }
        0 => {
            let status = unsafe { a20_svcmgr_task_kill(service.task.raw(), SIGKILL) };
            if status < 0 {
                log_status(output, b"SVC_MGR: retirement retry kill failed", status);
            }
        }
        status => {
            log_status(output, b"SVC_MGR: retirement poll failed", status);
            let kill = unsafe { a20_svcmgr_task_kill(service.task.raw(), SIGKILL) };
            if kill < 0 {
                log_status(output, b"SVC_MGR: retirement retry kill failed", kill);
            }
        }
    }
}

/// Record a confirmed process exit exactly once. Both the event-queue path
/// and the nonblocking wait-poll path use this production manager transition.
fn observe_confirmed_exit(service: &mut Service, exit_code: i32) -> bool {
    if service.retired_exit_code.is_some() {
        return service.life.state == lifecycle::State::Retiring;
    }
    match service.life.state {
        lifecycle::State::Retiring => {}
        lifecycle::State::Up | lifecycle::State::PendingPing
            if service.task.raw() != HANDLE_NULL =>
        {
            service.life.retirement_started();
            service.restart_after_retire = true;
        }
        _ => return false,
    }
    service.retired_exit_code = Some(exit_code);
    true
}

fn observe_exit_event(service: &mut Service, generation: u32, exit_code: i32) -> bool {
    service.life.current_generation(generation) && observe_confirmed_exit(service, exit_code)
}

fn process_exit_event(
    index: usize,
    service: &mut Service,
    generation: u32,
    exit_code: i32,
    queue: Handle,
    root: Handle,
    output: Handle,
    block_index: u32,
) {
    if !service.life.current_generation(generation) {
        return;
    }
    let task = service.task.raw();
    if task != HANDLE_NULL {
        let _ = unsafe { a20_svcmgr_event_cancel(queue, task) };
    }
    let ping = service.ping_ep.raw();
    if ping != HANDLE_NULL {
        let _ = unsafe { a20_svcmgr_event_cancel(queue, ping) };
    }
    if observe_exit_event(service, generation, exit_code) {
        retry_confirmed_retirement(index, service, queue, root, output, block_index);
    }
}

fn observe_exit_poll(service: &mut Service, wait_status: i64, exit_code: i32) -> bool {
    wait_status == 1 && observe_confirmed_exit(service, exit_code)
}

fn take_closed_retirement(service: &mut Service) -> Option<i32> {
    if service.task.raw() == HANDLE_NULL
        && service.client_ep.raw() == HANDLE_NULL
        && service.ping_ep.raw() == HANDLE_NULL
    {
        service.retired_exit_code.take()
    } else {
        None
    }
}

fn service_index(name: &[u8]) -> u8 {
    if name == b"rtcd\0" {
        0
    } else if name == b"echod\0" {
        1
    } else {
        2
    }
}

fn close_handle(handle: &mut Handle) {
    if *handle != HANDLE_NULL {
        unsafe {
            let _ = a20_svcmgr_close(*handle);
        }
        *handle = HANDLE_NULL;
    }
}

fn close_owned(handle: &mut OwnedHandle, output: Handle) {
    let status = handle.close();
    if status < 0 {
        log_status(output, b"SVC_MGR: handle close failed", status);
    }
}

/// Shared completion path for both EXIT events and nonblocking task_wait
/// polling. It consumes the Retiring state once, and only restarts after the
/// task has exited and every handle from that generation has closed.
fn complete_retiring_service(
    index: usize,
    service: &mut Service,
    queue: Handle,
    root: Handle,
    output: Handle,
    block_index: u32,
    exit_code: i32,
) {
    if service.life.state != lifecycle::State::Retiring
        || service.task.raw() != HANDLE_NULL
        || service.client_ep.raw() != HANDLE_NULL
        || service.ping_ep.raw() != HANDLE_NULL
    {
        return;
    }
    let restart = service.restart_after_retire;
    service.restart_after_retire = false;
    let now = unsafe { a20_svcmgr_now_ms() };
    match service.life.complete_retirement(restart, exit_code, now) {
        Some(decision) => apply_exit_decision(
            index,
            service,
            queue,
            root,
            output,
            block_index,
            exit_code,
            decision,
        ),
        None => write(output, b"SVC_MGR: retiring service stopped\n"),
    }
}

/// Retry closing a generation after exit was positively observed. Preserve
/// the exit code across transient close failures so an EXIT event and a later
/// poll cannot lose or double-consume its restart decision.
fn retry_confirmed_retirement(
    index: usize,
    service: &mut Service,
    queue: Handle,
    root: Handle,
    output: Handle,
    block_index: u32,
) {
    // A missing task handle means the process is no longer reachable through
    // this manager. Endpoint cleanup is still retried even if no exit result
    // was retained (for example, shutdown after partial setup), but only an
    // explicit confirmed exit code may consume a restart decision.
    if service.task.raw() != HANDLE_NULL && service.retired_exit_code.is_none() {
        return;
    }
    let exit_code = service.retired_exit_code;
    if exit_code.is_some() {
        close_owned(&mut service.task, output);
    }
    close_owned(&mut service.client_ep, output);
    close_owned(&mut service.ping_ep, output);
    if exit_code.is_some() {
        if let Some(code) = take_closed_retirement(service) {
            complete_retiring_service(index, service, queue, root, output, block_index, code);
        }
    }
}

fn cleanup_generation(service: &mut Service, queue: Handle, output: Handle, kill: bool) -> bool {
    if service.task.raw() != HANDLE_NULL {
        if kill {
            let status = unsafe { a20_svcmgr_task_kill(service.task.raw(), SIGKILL) };
            if status < 0 {
                log_status(output, b"SVC_MGR: task kill failed", status);
                // This function is used only by shutdown/rollback. Keep the
                // live task tracked and let the shutdown poll retry; this path
                // must never turn into a health restart.
                service.life.retirement_started();
                service.restart_after_retire = false;
                return false;
            }
            let mut exit_code = 0;
            match unsafe { a20_svcmgr_task_wait(service.task.raw(), &mut exit_code) } {
                1 => {
                    service.retired_exit_code = Some(exit_code);
                    service.life.retirement_started();
                    service.restart_after_retire = false;
                }
                0 => {
                    service.life.retirement_started();
                    service.restart_after_retire = false;
                    return false;
                }
                waited => {
                    log_status(output, b"SVC_MGR: task wait failed", waited);
                    service.life.retirement_started();
                    service.restart_after_retire = false;
                    return false;
                }
            }
        }
        let _ = unsafe { a20_svcmgr_event_cancel(queue, service.task.raw()) };
        close_owned(&mut service.task, output);
    }
    if service.ping_ep.raw() != HANDLE_NULL {
        let _ = unsafe { a20_svcmgr_event_cancel(queue, service.ping_ep.raw()) };
    }
    close_owned(&mut service.client_ep, output);
    close_owned(&mut service.ping_ep, output);
    service.life.state = lifecycle::State::Retiring;
    service.restart_after_retire = false;
    service.task.raw() == HANDLE_NULL
        && service.client_ep.raw() == HANDLE_NULL
        && service.ping_ep.raw() == HANDLE_NULL
}

fn registry_drain(
    registry: Handle,
    services: &[Service],
    output: Handle,
    replies: &mut ReplyQueue,
) {
    // Bound work per event-loop turn and stop consuming requests when their
    // responses cannot be retained. The channel then provides backpressure.
    let mut processed = 0usize;
    while processed < 8 && replies.len < replies.entries.len() {
        let mut bytes = [0u8; 64];
        let mut handles = [HANDLE_NULL; 2];
        let mut length = 0u32;
        let mut handle_count = 0u32;
        let status = unsafe {
            a20_svcmgr_registry_recv(
                registry,
                bytes.as_mut_ptr(),
                bytes.len() as u32,
                &mut length,
                handles.as_mut_ptr(),
                handles.len() as u32,
                &mut handle_count,
            )
        };
        if status == -18 {
            break;
        } // A20_ERR_WOULD_BLOCK
        if status < 0 {
            log_status(output, b"SVC_MGR: registry receive failed", status);
            break;
        }
        let close_count = handle_count.min(handles.len() as u32) as usize;
        for handle in handles.iter_mut().take(close_count) {
            close_handle(handle);
        }
        processed += 1;
        let mut reply = PendingReply {
            status: -1,
            service_index: 0,
            generation: 0,
        };
        if length >= 4
            && u32::from_le_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]) == REG_OP_LOOKUP
        {
            let name_len = (length - 4).min(31) as usize;
            let name = &bytes[4..4 + name_len];
            if let Some((index, service)) = services.iter().enumerate().find(|(_, service)| {
                service.life.state == lifecycle::State::Up && name_matches(name, service.def.name)
            }) {
                reply.status = 0;
                reply.service_index = index as u8;
                reply.generation = service.life.generation;
            }
        }
        if replies.len != 0 {
            let _ = replies.push(reply);
        } else {
            let sent = send_registry_reply(registry, reply, services);
            if sent == WOULD_BLOCK {
                let _ = replies.push(reply);
            } else if sent < 0 {
                log_status(output, b"SVC_MGR: registry reply failed", sent);
            }
        }
    }
}

fn send_registry_reply(registry: Handle, reply: PendingReply, services: &[Service]) -> i64 {
    let (status, endpoint, has_endpoint) = if reply.status == 0 {
        match services.get(reply.service_index as usize) {
            Some(service)
                if service.life.state == lifecycle::State::Up
                    && service.life.current_generation(reply.generation) =>
            {
                (0, service.client_ep.raw(), 1)
            }
            _ => (-1, HANDLE_NULL, 0),
        }
    } else {
        (reply.status, HANDLE_NULL, 0)
    };
    unsafe { a20_svcmgr_registry_reply(registry, status, endpoint, has_endpoint) }
}

fn flush_registry_replies(
    registry: Handle,
    services: &[Service],
    output: Handle,
    replies: &mut ReplyQueue,
) {
    while let Some(reply) = replies.front() {
        let status = send_registry_reply(registry, reply, services);
        if status == WOULD_BLOCK {
            return;
        }
        replies.pop();
        if status < 0 {
            log_status(output, b"SVC_MGR: deferred registry reply failed", status);
        }
    }
}

fn name_matches(request: &[u8], registered: &[u8]) -> bool {
    let request_len = request
        .iter()
        .position(|b| *b == 0)
        .unwrap_or(request.len());
    let registered_len = registered
        .iter()
        .position(|b| *b == 0)
        .unwrap_or(registered.len());
    request_len == registered_len
        && request[..request_len]
            .iter()
            .zip(registered[..registered_len].iter())
            .all(|(left, right)| left == right)
}

fn send_ping(service: &mut Service, now: u64, output: Handle) {
    if service.life.ping_expired(now) {
        write(output, b"SVC_MGR: health ping timeout name=");
        write(output, service.def.name);
        write(output, b"\n");
        let status = unsafe { a20_svcmgr_task_kill(service.task.raw(), SIGKILL) };
        if status < 0 {
            log_status(output, b"SVC_MGR: SIGKILL failed", status);
        }
        // Track the generation before waiting for EXITED. The queue can miss
        // or delay that event, so the normal wait-poll path must be able to
        // finish this same retirement and apply restart policy exactly once.
        service.life.retirement_started();
        service.restart_after_retire = true;
        return;
    }
    if !service.life.ping_due(now) {
        return;
    }

    let kind = if service.def.ping_kind == 1 {
        idl::RTCD_REQ_TIME as u16
    } else {
        idl::SVCMGR_REQ_ECHO as u16
    };
    let frame = idl::envelope_encode(&idl::ENVELOPE {
        version: idl::A20_SERVICES_IDL_VERSION,
        r#type: kind,
        size: idl::ENVELOPE_WIRE_SIZE as u32,
    });
    let status = unsafe {
        a20_svcmgr_channel_send(service.ping_ep.raw(), frame.as_ptr(), frame.len() as u32)
    };
    if status >= 0 {
        service.life.ping_sent(now);
    } else {
        // Health channels are advisory just like in the C manager. A full or
        // closed peer endpoint leaves the service Up so its EXIT watch remains
        // authoritative; the C implementation silently retries on a later
        // turn rather than logging every expected peer-close race.
    }
}

fn apply_exit_decision(
    index: usize,
    service: &mut Service,
    queue: Handle,
    root: Handle,
    output: Handle,
    block_index: u32,
    exit_code: i32,
    decision: lifecycle::ExitDecision,
) {
    match decision {
        lifecycle::ExitDecision::CleanExit => {
            log_named(output, b"SVC_MGR: service completed name=", service);
            close_owned(&mut service.client_ep, output);
            close_owned(&mut service.ping_ep, output);
            close_owned(&mut service.task, output);
        }
        lifecycle::ExitDecision::RetireBudget => {
            log_named(
                output,
                b"SVC_MGR: FLAP budget exceeded, service marked failed name=",
                service,
            );
            close_owned(&mut service.client_ep, output);
            close_owned(&mut service.ping_ep, output);
            close_owned(&mut service.task, output);
        }
        lifecycle::ExitDecision::Restart => {
            write(output, b"SVC_MGR: crash detected name=");
            write(output, service.def.name);
            write(output, b" exit_code=");
            write_u64(output, exit_code.unsigned_abs() as u64);
            write(output, b"\n");
            close_owned(&mut service.client_ep, output);
            close_owned(&mut service.ping_ep, output);
            close_owned(&mut service.task, output);
            if service.client_ep.raw() != HANDLE_NULL
                || service.ping_ep.raw() != HANDLE_NULL
                || service.task.raw() != HANDLE_NULL
            {
                service.life.state = lifecycle::State::Failed;
                write(
                    output,
                    b"SVC_MGR: old generation handles not fully closed; restart withheld\n",
                );
                return;
            }
            unsafe {
                let _ = a20_svcmgr_sleep_ms(BACKOFF_MS);
            }
            let spawned = spawn_one(service, root, output, block_index, queue);
            if spawned < 0 {
                service.life.state = lifecycle::State::Failed;
                log_named(output, b"SVC_MGR: respawn failed name=", service);
                log_status(output, b"SVC_MGR: respawn status", spawned);
                return;
            }
            // spawn_one registers both watches before exposing the new UP
            // state. If either watch fails it kills, waits, and closes.
            write(output, b"SVC_MGR: healed name=");
            write(output, service.def.name);
            write(output, b" restarts=");
            write_u64(output, service.life.restarts as u64);
            write(output, b"\n");
        }
    }
    let _ = (index, queue);
}

fn manager_main() -> c_int {
    let mut output = HANDLE_NULL;
    let mut root = HANDLE_NULL;
    let started = unsafe { a20_svcmgr_startup(&mut output, &mut root) };
    if started < 0 || output == HANDLE_NULL {
        return 90;
    }

    let registry_result = unsafe { a20_svcmgr_registry_claim() };
    if registry_result < 0 {
        write(output, b"SVC_MGR: FAIL registry claim\n");
        return EXIT_FAIL;
    }
    let registry = registry_result as Handle;
    let mut queue = HANDLE_NULL;
    let queue_result = unsafe { a20_svcmgr_event_queue_create(&mut queue) };
    if queue_result < 0 {
        log_status(output, b"SVC_MGR: event queue create failed", queue_result);
        let _ = unsafe { a20_svcmgr_close(registry) };
        return EXIT_FAIL;
    }
    let watch_registry = unsafe {
        a20_svcmgr_event_watch(queue, registry, 1u64 << EVENT_MESSAGE_READY, REGISTRY_TAG)
    };
    if watch_registry < 0 {
        log_status(output, b"SVC_MGR: registry watch failed", watch_registry);
        let _ = unsafe { a20_svcmgr_close(queue) };
        let _ = unsafe { a20_svcmgr_close(registry) };
        return EXIT_FAIL;
    }

    let definitions = [
        Definition {
            name: b"rtcd\0",
            path: b"/bin/rtcd-rv.a20drv\0",
            endpoint_slot: idl::A20_RTCD_EP_SLOT as u32,
            ping_kind: 1,
        },
        Definition {
            name: b"echod\0",
            path: b"/bin/svc-echod-rv\0",
            endpoint_slot: idl::A20_SVC_ENDPOINT_SLOT as u32,
            ping_kind: 0,
        },
        Definition {
            name: b"ufsd\0",
            path: b"/bin/ufsd-rv\0",
            endpoint_slot: idl::A20_SVC_ENDPOINT_SLOT as u32,
            ping_kind: 2,
        },
    ];
    let mut services = [
        Service::new(definitions[0]),
        Service::new(definitions[1]),
        Service::new(definitions[2]),
    ];
    let (block_config, config_status) = load_ufsd_block_config();
    if config_status < 0 {
        log_status(output, b"SVC_MGR: ufsd block config failed", config_status);
    }
    write(output, b"SVC_MGR: ufsd blk=");
    write_u64(output, block_config.index as u64);
    write(
        output,
        if block_config.from_cmdline {
            b" (cmdline)\n"
        } else {
            b" (default)\n"
        },
    );

    for i in 0..services.len() {
        let spawned = spawn_one(&mut services[i], root, output, block_config.index, queue);
        if spawned < 0 {
            log_named(output, b"SVC_MGR: FAIL spawn name=", &services[i]);
            log_status(output, b"SVC_MGR: spawn status", spawned);
            shutdown(&mut services, queue, registry, output, root, block_config.index);
            return EXIT_FAIL;
        }
    }
    write(output, b"SVC_MGR: ready services=");
    write_u64(output, services.len() as u64);
    write(output, b"\n");

    let mut pending_replies = ReplyQueue::new();
    loop {
        let mut event = Event {
            user_data: 0,
            data0: 0,
        };
        let wait = unsafe { a20_svcmgr_event_wait(queue, 1_000_000_000, &mut event) };
        if wait < 0 {
            log_status(output, b"SVC_MGR: event wait failed", wait);
            shutdown(&mut services, queue, registry, output, root, block_config.index);
            return EXIT_FAIL;
        }
        if wait == 1 {
            if event.user_data == REGISTRY_TAG {
                // The channel is drained below even on timeout, because the
                // event queue may coalesce readiness notifications.
            } else {
                match lifecycle::decode_tag(event.user_data) {
                    lifecycle::EventTag::Exit { index, generation } => {
                        let idx = index as usize;
                        if idx < services.len() {
                            process_exit_event(
                                idx,
                                &mut services[idx],
                                generation,
                                event.data0 as i32,
                                queue,
                                root,
                                output,
                                block_config.index,
                            );
                        }
                    }
                    lifecycle::EventTag::Pong { index, generation } => {
                        let idx = index as usize;
                        if idx < services.len()
                            && services[idx].life.current_generation(generation)
                            && services[idx].life.state == lifecycle::State::PendingPing
                        {
                            let mut bytes = [0u8; 64];
                            let mut length = 0u32;
                            let mut count = 0u32;
                            let status = unsafe {
                                a20_svcmgr_registry_recv(
                                    services[idx].ping_ep.raw(),
                                    bytes.as_mut_ptr(),
                                    bytes.len() as u32,
                                    &mut length,
                                    core::ptr::null_mut(),
                                    0,
                                    &mut count,
                                )
                            };
                            if status >= 0 {
                                services[idx].life.pong(unsafe { a20_svcmgr_now_ms() });
                            }
                        }
                    }
                    lifecycle::EventTag::Other => {}
                }
            }
        }

        flush_registry_replies(registry, &services, output, &mut pending_replies);
        registry_drain(registry, &services, output, &mut pending_replies);
        let now = unsafe { a20_svcmgr_now_ms() };
        for (index, service) in services.iter_mut().enumerate() {
            poll_retiring(index, service, queue, root, output, block_config.index);
            send_ping(service, now, output);
        }
    }
}

fn shutdown(
    services: &mut [Service],
    queue: Handle,
    registry: Handle,
    output: Handle,
    root: Handle,
    block_index: u32,
) {
    for service in services.iter_mut() {
        let _ = cleanup_generation(service, queue, output, true);
    }
    // Keep ownership of every still-live task while polling; never return and
    // silently drop the only handle that can supervise or retire it.
    while services.iter().any(|service| {
        service.task.raw() != HANDLE_NULL
            || service.client_ep.raw() != HANDLE_NULL
            || service.ping_ep.raw() != HANDLE_NULL
            || service.retired_exit_code.is_some()
    }) {
        for (index, service) in services.iter_mut().enumerate() {
            poll_retiring(index, service, queue, root, output, block_index);
        }
        unsafe {
            let _ = a20_svcmgr_sleep_ms(100);
        }
    }
    let _ = unsafe { a20_svcmgr_close(queue) };
    let _ = unsafe { a20_svcmgr_close(registry) };
}

#[no_mangle]
#[cfg(not(test))]
pub extern "C" fn main(_argc: c_int, _argv: *const *const u8, _envp: *const *const u8) -> c_int {
    manager_main()
}

#[panic_handler]
#[cfg(not(test))]
fn panic(_info: &PanicInfo) -> ! {
    unsafe { a20_svcmgr_panic_exit(127) }
}

#[cfg(test)]
mod manager_tests {
    use super::*;
    use std::sync::atomic::{AtomicI32, AtomicU32, AtomicU64, Ordering};
    use std::sync::Mutex;

    static TEST_LOCK: Mutex<()> = Mutex::new(());
    static CLOSE_FAIL_HANDLE: AtomicU32 = AtomicU32::new(HANDLE_NULL);
    static CLOSE_FAIL_COUNT: AtomicU32 = AtomicU32::new(0);
    static WAIT_STATUS: AtomicI32 = AtomicI32::new(1);
    static WAIT_EXIT: AtomicI32 = AtomicI32::new(42);
    static NOW_MS: AtomicU64 = AtomicU64::new(2_000);
    static NEXT_HANDLE: AtomicU32 = AtomicU32::new(100);
    static SPAWN_COUNT: AtomicU32 = AtomicU32::new(0);
    static KILL_COUNT: AtomicU32 = AtomicU32::new(0);
    static CLOSE_COUNT: AtomicU32 = AtomicU32::new(0);
    static WATCH_CALL_COUNT: AtomicU32 = AtomicU32::new(0);
    static WATCH_FAIL_CALL: AtomicU32 = AtomicU32::new(0);
    static REGISTRY_RECV_COUNT: AtomicU32 = AtomicU32::new(0);
    static REGISTRY_REPLY_STATUS: AtomicI32 = AtomicI32::new(0);
    static REGISTRY_REPLY_LOG: Mutex<std::vec::Vec<i32>> = Mutex::new(std::vec::Vec::new());
    static EXIT_AFTER_SLEEP: AtomicI32 = AtomicI32::new(0);

    #[test]
    fn ufsd_block_parser_accepts_bounded_values_with_ascii_whitespace() {
        for index in 1..=MAX_UFSD_BLOCK {
            let mut line = std::vec::Vec::from(&b"root=x\ta20.ufsd_blk="[..]);
            line.extend_from_slice(index.to_string().as_bytes());
            line.extend_from_slice(b"\nquiet");
            assert_eq!(
                parse_ufsd_block_config(&line),
                UfsdBlockConfig { index, from_cmdline: true },
            );
        }
    }

    #[test]
    fn ufsd_block_parser_keeps_default_for_invalid_or_overflow_values() {
        for line in [
            &b"a20.ufsd_blk="[..],
            &b"a20.ufsd_blk=0"[..],
            &b"a20.ufsd_blk=16"[..],
            &b"a20.ufsd_blk=garbage"[..],
            &b"a20.ufsd_blk=12junk"[..],
            &b"a20.ufsd_blk=1,2"[..],
            &b"a20.ufsd_blk=4294967297"[..],
            &b"a20.ufsd_blk=4294967305"[..],
            &b"a20.ufsd_blk=18446744073709551617"[..],
            &b"a20.ufsd_blk=999999999999999999999999999999999999999999"[..],
        ] {
            assert_eq!(
                parse_ufsd_block_config(line),
                UfsdBlockConfig { index: DEFAULT_UFSD_BLOCK, from_cmdline: false },
                "invalid command line {line:?}",
            );
        }
    }

    #[test]
    fn ufsd_block_parser_ignores_invalid_duplicates_and_uses_last_valid_value() {
        assert_eq!(
            parse_ufsd_block_config(b"a20.ufsd_blk=5 a20.ufsd_blk=4294967305"),
            UfsdBlockConfig { index: 5, from_cmdline: true },
        );
        assert_eq!(
            parse_ufsd_block_config(b"a20.ufsd_blk=5\ta20.ufsd_blk=9"),
            UfsdBlockConfig { index: 9, from_cmdline: true },
        );
    }

    fn reset_stubs() {
        CLOSE_FAIL_HANDLE.store(HANDLE_NULL, Ordering::SeqCst);
        CLOSE_FAIL_COUNT.store(0, Ordering::SeqCst);
        WAIT_STATUS.store(1, Ordering::SeqCst);
        WAIT_EXIT.store(42, Ordering::SeqCst);
        NOW_MS.store(2_000, Ordering::SeqCst);
        NEXT_HANDLE.store(100, Ordering::SeqCst);
        SPAWN_COUNT.store(0, Ordering::SeqCst);
        KILL_COUNT.store(0, Ordering::SeqCst);
        CLOSE_COUNT.store(0, Ordering::SeqCst);
        WATCH_CALL_COUNT.store(0, Ordering::SeqCst);
        WATCH_FAIL_CALL.store(0, Ordering::SeqCst);
        REGISTRY_RECV_COUNT.store(0, Ordering::SeqCst);
        REGISTRY_REPLY_STATUS.store(0, Ordering::SeqCst);
        REGISTRY_REPLY_LOG.lock().unwrap().clear();
        EXIT_AFTER_SLEEP.store(0, Ordering::SeqCst);
    }

    fn fail_close_once(handle: Handle) {
        CLOSE_FAIL_HANDLE.store(handle, Ordering::SeqCst);
        CLOSE_FAIL_COUNT.store(1, Ordering::SeqCst);
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_close(handle: Handle) -> i64 {
        CLOSE_COUNT.fetch_add(1, Ordering::SeqCst);
        if CLOSE_FAIL_HANDLE.load(Ordering::SeqCst) == handle
            && CLOSE_FAIL_COUNT
                .fetch_update(Ordering::SeqCst, Ordering::SeqCst, |n| n.checked_sub(1))
                .is_ok()
        {
            -5
        } else {
            0
        }
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_write(_output: Handle, _bytes: *const u8, _length: u32) {}

    #[no_mangle]
    extern "C" fn a20_svcmgr_now_ms() -> u64 {
        NOW_MS.load(Ordering::SeqCst)
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_event_cancel(_queue: Handle, _target: Handle) -> i64 {
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_task_wait(_task: Handle, out_exit_code: *mut i32) -> i64 {
        unsafe { *out_exit_code = WAIT_EXIT.load(Ordering::SeqCst) };
        WAIT_STATUS.load(Ordering::SeqCst) as i64
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_task_kill(_task: Handle, signal: u32) -> i64 {
        assert_eq!(signal, SIGKILL);
        KILL_COUNT.fetch_add(1, Ordering::SeqCst);
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_sleep_ms(_milliseconds: u64) -> i64 {
        if EXIT_AFTER_SLEEP.swap(0, Ordering::SeqCst) != 0 {
            WAIT_STATUS.store(1, Ordering::SeqCst);
        }
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_channel_send(
        _endpoint: Handle,
        _bytes: *const u8,
        _length: u32,
    ) -> i64 {
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_channel_create(out_endpoints: *mut Handle) -> i64 {
        let first = NEXT_HANDLE.fetch_add(1, Ordering::SeqCst);
        unsafe {
            *out_endpoints = first;
            *out_endpoints.add(1) = first + 1;
        }
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_path_open(_path: *const c_char, out_image: *mut Handle) -> i64 {
        unsafe { *out_image = NEXT_HANDLE.fetch_add(1, Ordering::SeqCst) };
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_task_spawn(
        _image: Handle,
        _root: Handle,
        _argv: *const *const c_char,
        _argc: u32,
        _service_child: Handle,
        _service_slot: u32,
        _ping_child: Handle,
        _ping_slot: u32,
        _stdout_handle: Handle,
        out_task: *mut Handle,
    ) -> i64 {
        unsafe { *out_task = NEXT_HANDLE.fetch_add(1, Ordering::SeqCst) };
        SPAWN_COUNT.fetch_add(1, Ordering::SeqCst);
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_event_watch(
        _queue: Handle,
        _target: Handle,
        _mask: u64,
        _user_data: u64,
    ) -> i64 {
        let call = WATCH_CALL_COUNT.fetch_add(1, Ordering::SeqCst) + 1;
        if call == WATCH_FAIL_CALL.load(Ordering::SeqCst) {
            -5
        } else {
            0
        }
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_registry_claim() -> i64 {
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_event_queue_create(out_queue: *mut Handle) -> i64 {
        unsafe { *out_queue = NEXT_HANDLE.fetch_add(1, Ordering::SeqCst) };
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_event_wait(
        _queue: Handle,
        _timeout_ns: u64,
        _out_event: *mut Event,
    ) -> i64 {
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_startup(out_stdout: *mut Handle, out_root: *mut Handle) -> i64 {
        unsafe {
            *out_stdout = 1;
            *out_root = 2;
        }
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_registry_recv(
        _endpoint: Handle,
        _bytes: *mut u8,
        _capacity: u32,
        out_length: *mut u32,
        _handles: *mut Handle,
        _handle_capacity: u32,
        out_handle_count: *mut u32,
    ) -> i64 {
        REGISTRY_RECV_COUNT.fetch_add(1, Ordering::SeqCst);
        unsafe {
            *out_length = 0;
            *out_handle_count = 0;
        }
        WOULD_BLOCK
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_registry_reply(
        _endpoint: Handle,
        status: i64,
        _service_endpoint: Handle,
        _has_endpoint: u32,
    ) -> i64 {
        REGISTRY_REPLY_LOG.lock().unwrap().push(status as i32);
        REGISTRY_REPLY_STATUS.load(Ordering::SeqCst) as i64
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_read_cmdline(
        _out_bytes: *mut u8,
        _capacity: u32,
        out_length: *mut u32,
    ) -> i64 {
        unsafe { *out_length = 0 };
        0
    }

    #[no_mangle]
    extern "C" fn a20_svcmgr_panic_exit(_code: i32) -> ! {
        panic!("unexpected panic exit in manager test")
    }

    fn service() -> Service {
        Service::new(Definition {
            name: b"echod\0",
            path: b"/bin/echod\0",
            endpoint_slot: 1,
            ping_kind: 0,
        })
    }

    #[test]
    fn manager_exit_event_and_wait_poll_share_one_shot_transition() {
        let mut from_event = service();
        assert!(from_event.life.spawned(1, 10));
        from_event.task = OwnedHandle::new(7);
        assert!(observe_exit_event(&mut from_event, 1, 42));
        assert_eq!(from_event.life.state, lifecycle::State::Retiring);
        assert!(from_event.restart_after_retire);
        assert_eq!(from_event.retired_exit_code, Some(42));
        // Duplicate and stale events cannot replace the completion or spend a
        // second restart decision.
        assert!(observe_exit_event(&mut from_event, 1, 99));
        assert_eq!(from_event.retired_exit_code, Some(42));
        assert!(!observe_exit_event(&mut from_event, 0, 99));
        from_event.task = OwnedHandle::none();
        let code = take_closed_retirement(&mut from_event).unwrap();
        assert_eq!(
            from_event.life.complete_retirement(true, code, 11),
            Some(lifecycle::ExitDecision::Restart)
        );
        assert_eq!(from_event.life.window_restarts, 1);
        assert_eq!(take_closed_retirement(&mut from_event), None);

        let mut from_poll = service();
        assert!(from_poll.life.spawned(1, 10));
        from_poll.task = OwnedHandle::new(8);
        assert!(!observe_exit_poll(&mut from_poll, 0, 42));
        assert_eq!(from_poll.life.state, lifecycle::State::Up);
        assert!(observe_exit_poll(&mut from_poll, 1, 42));
        from_poll.task = OwnedHandle::none();
        assert_eq!(take_closed_retirement(&mut from_poll), Some(42));
        assert_eq!(
            from_poll.life.complete_retirement(true, 42, 11),
            Some(lifecycle::ExitDecision::Restart)
        );
    }

    #[test]
    fn ping_timeout_poll_and_close_retry_restart_through_manager() {
        let _guard = TEST_LOCK.lock().unwrap();
        reset_stubs();
        let mut service = service();
        assert!(service.life.spawned(1, 10));
        service.task = OwnedHandle::new(10);
        service.client_ep = OwnedHandle::new(11);
        service.ping_ep = OwnedHandle::new(12);
        service.life.ping_sent(10);
        fail_close_once(11);

        send_ping(&mut service, 2_000, 1);
        assert_eq!(KILL_COUNT.load(Ordering::SeqCst), 1);
        assert_eq!(service.life.state, lifecycle::State::Retiring);
        assert!(service.restart_after_retire);

        // The actual manager poll confirms exit, closes what it can, and
        // retains the code + failed endpoint for the next poll.
        poll_retiring(1, &mut service, 2, 3, 1, 1);
        assert_eq!(service.task.raw(), HANDLE_NULL);
        assert_eq!(service.client_ep.raw(), 11);
        assert_eq!(service.retired_exit_code, Some(42));
        assert_eq!(SPAWN_COUNT.load(Ordering::SeqCst), 0);

        // A later poll retries the close and consumes the health restart once.
        poll_retiring(1, &mut service, 2, 3, 1, 1);
        assert_eq!(service.life.state, lifecycle::State::Up);
        assert_eq!(service.life.generation, 2);
        assert_eq!(service.life.restarts, 1);
        assert_eq!(service.life.window_restarts, 1);
        assert_eq!(service.retired_exit_code, None);
        assert_eq!(SPAWN_COUNT.load(Ordering::SeqCst), 1);
        assert_eq!(service.task.raw(), 103);
        assert_ne!(service.client_ep.raw(), HANDLE_NULL);
        assert_ne!(service.ping_ep.raw(), HANDLE_NULL);
    }

    #[test]
    fn shutdown_exit_completion_never_requests_restart() {
        let _guard = TEST_LOCK.lock().unwrap();
        reset_stubs();
        let mut service = service();
        assert!(service.life.spawned(1, 10));
        service.task = OwnedHandle::new(19);
        service.life.retirement_started();
        service.restart_after_retire = false;
        assert!(observe_exit_event(&mut service, 1, 9));
        assert!(!service.restart_after_retire);
        service.task = OwnedHandle::none();
        let code = take_closed_retirement(&mut service).unwrap();
        assert_eq!(service.life.complete_retirement(false, code, 11), None);
        assert_eq!(service.life.state, lifecycle::State::Failed);
        assert_eq!(service.life.window_restarts, 0);
    }

    #[test]
    fn actual_exit_event_and_later_poll_cannot_restart_twice() {
        let _guard = TEST_LOCK.lock().unwrap();
        reset_stubs();
        let mut service = service();
        assert!(service.life.spawned(1, 10));
        service.task = OwnedHandle::new(20);
        service.client_ep = OwnedHandle::new(21);
        service.ping_ep = OwnedHandle::new(22);
        fail_close_once(21);

        process_exit_event(1, &mut service, 1, 42, 2, 3, 1, 1);
        assert_eq!(service.retired_exit_code, Some(42));
        assert_eq!(SPAWN_COUNT.load(Ordering::SeqCst), 0);
        assert_eq!(service.client_ep.raw(), 21);

        // The old event is stale once completion eventually installs gen 2;
        // a poll retry closes the failed endpoint and performs one restart.
        poll_retiring(1, &mut service, 2, 3, 1, 1);
        assert_eq!(service.life.generation, 2);
        assert_eq!(service.life.restarts, 1);
        assert_eq!(SPAWN_COUNT.load(Ordering::SeqCst), 1);
        process_exit_event(1, &mut service, 1, 99, 2, 3, 1, 1);
        assert_eq!(service.life.generation, 2);
        assert_eq!(service.life.restarts, 1);
        assert_eq!(SPAWN_COUNT.load(Ordering::SeqCst), 1);
    }

    #[test]
    fn shutdown_keeps_live_task_owned_until_wait_confirms_exit() {
        let _guard = TEST_LOCK.lock().unwrap();
        reset_stubs();
        WAIT_STATUS.store(0, Ordering::SeqCst);
        EXIT_AFTER_SLEEP.store(1, Ordering::SeqCst);
        let mut service = service();
        assert!(service.life.spawned(1, 10));
        service.task = OwnedHandle::new(31);
        service.client_ep = OwnedHandle::new(32);
        service.ping_ep = OwnedHandle::new(33);

        shutdown(core::slice::from_mut(&mut service), 4, 5, 1, 2, 1);

        assert_eq!(service.task.raw(), HANDLE_NULL);
        assert_eq!(service.client_ep.raw(), HANDLE_NULL);
        assert_eq!(service.ping_ep.raw(), HANDLE_NULL);
        assert_eq!(service.retired_exit_code, None);
        assert_eq!(service.life.state, lifecycle::State::Failed);
        assert_eq!(service.life.restarts, 0);
        assert_eq!(SPAWN_COUNT.load(Ordering::SeqCst), 0);
        // Initial shutdown kill plus one nonblocking poll retry before the
        // mock sleep reports the confirmed exit.
        assert_eq!(KILL_COUNT.load(Ordering::SeqCst), 2);
    }

    #[test]
    fn failed_generation_watch_rolls_back_spawn_and_closes_owned_resources() {
        let _guard = TEST_LOCK.lock().unwrap();
        for failed_watch in [1, 2] {
            reset_stubs();
            WATCH_FAIL_CALL.store(failed_watch, Ordering::SeqCst);
            let mut service = service();

            assert_eq!(spawn_one(&mut service, 2, 3, 1, 4), -5);

            assert_eq!(service.life.state, lifecycle::State::Boot);
            assert_eq!(service.life.generation, 0);
            assert_eq!(service.task.raw(), HANDLE_NULL);
            assert_eq!(service.client_ep.raw(), HANDLE_NULL);
            assert_eq!(service.ping_ep.raw(), HANDLE_NULL);
            assert_eq!(SPAWN_COUNT.load(Ordering::SeqCst), 1);
            assert_eq!(KILL_COUNT.load(Ordering::SeqCst), 1);
            // Both channel pairs, image, and task remain owned by SpawnAttempt
            // and are closed as its scope exits after confirmed rollback.
            assert_eq!(CLOSE_COUNT.load(Ordering::SeqCst), 6);
        }
    }

    #[test]
    fn full_registry_reply_queue_stops_receiving_and_flush_preserves_fifo() {
        let _guard = TEST_LOCK.lock().unwrap();
        reset_stubs();
        let mut services = [service()];
        assert!(services[0].life.spawned(1, 10));
        services[0].client_ep = OwnedHandle::new(41);

        let mut replies = ReplyQueue::new();
        for status in 0..16 {
            assert!(replies.push(PendingReply {
                status,
                service_index: 0,
                generation: 1,
            }));
        }
        registry_drain(50, &services, 1, &mut replies);
        assert_eq!(REGISTRY_RECV_COUNT.load(Ordering::SeqCst), 0);

        // A blocked channel leaves the head queued. Once writable, each
        // response is removed in its original order.
        REGISTRY_REPLY_STATUS.store(WOULD_BLOCK as i32, Ordering::SeqCst);
        flush_registry_replies(50, &services, 1, &mut replies);
        assert_eq!(replies.len, 16);
        assert_eq!(*REGISTRY_REPLY_LOG.lock().unwrap(), vec![0]);

        REGISTRY_REPLY_STATUS.store(0, Ordering::SeqCst);
        flush_registry_replies(50, &services, 1, &mut replies);
        assert_eq!(replies.len, 0);
        let sent = REGISTRY_REPLY_LOG.lock().unwrap();
        assert_eq!(sent.len(), 17);
        assert_eq!(sent[0], 0);
        assert_eq!(&sent[1..], &(0..16).collect::<std::vec::Vec<_>>());
    }
}
