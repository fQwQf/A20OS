use a20rust_support::lock::spinlock_t;
use core::ffi::c_int;

pub const AF_ALG: c_int = 38;
pub const SOCK_STREAM: c_int = 1;
pub const SOCK_DGRAM: c_int = 2;
pub const MSG_DONTWAIT: c_int = 0x0040;

pub const NET_MAX_PAYLOAD: usize = 65535;
pub const NET_MAX_QUEUE: c_int = 128;
pub const NET_SOCKADDR_MAX: usize = 128;

pub const PROC_BLOCKED: c_int = 3;

pub const ENOTSOCK: c_int = 88;
pub const ENOTCONN: c_int = 107;
pub const EMSGSIZE: c_int = 90;
pub const EAGAIN: c_int = 11;
pub const EINVAL: c_int = 22;
pub const ERESTARTSYS: c_int = 512;

#[repr(C)]
#[derive(Clone, Copy)]
pub struct net_bh_event_t {
    pub next: *mut net_bh_event_t,
    pub type_: c_int,
    pub err: c_int,
    pub len: usize,
    pub addr: [u8; NET_SOCKADDR_MAX],
    pub addrlen: usize,
    pub has_pktinfo: u8,
    pub has_hoplimit: u8,
    pub has_tclass: u8,
    pub pad: u8,
    pub pktinfo_ifindex: u32,
    pub pktinfo_addr: [u8; 16],
    pub hoplimit: u8,
    pub tclass: u8,
    pub __pad_meta: u16,
    pub data: [u8; NET_MAX_PAYLOAD],
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct net_recv_meta_t {
    pub has_pktinfo: u8,
    pub has_hoplimit: u8,
    pub has_tclass: u8,
    pub pad: u8,
    pub pktinfo_ifindex: u32,
    pub pktinfo_addr: [u8; 16],
    pub hoplimit: u8,
    pub tclass: u8,
    pub __pad_meta: u16,
}

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct net_msg_t {
    pub next: *mut net_msg_t,
    pub len: usize,
    pub off: usize,
    pub addr: [u8; NET_SOCKADDR_MAX],
    pub addrlen: usize,
    pub has_pktinfo: u8,
    pub has_hoplimit: u8,
    pub has_tclass: u8,
    pub pad: u8,
    pub pktinfo_ifindex: u32,
    pub pktinfo_addr: [u8; 16],
    pub hoplimit: u8,
    pub tclass: u8,
    pub __pad_meta: u16,
    pub data: [u8; NET_MAX_PAYLOAD],
}

#[repr(C)]
pub struct net_socket_t {
    pub domain: c_int,
    pub type_: c_int,
    pub protocol: c_int,
    pub nonblock: c_int,
    pub closed: c_int,
    pub shut_rd: c_int,
    pub shut_wr: c_int,
    pub peer_closed: c_int,
    pub bound: c_int,
    pub connected: c_int,
    pub listening: c_int,
    pub local: [u8; NET_SOCKADDR_MAX],
    pub local_len: usize,
    pub peer_addr: [u8; NET_SOCKADDR_MAX],
    pub peer_len: usize,
    pub peer: *mut net_socket_t,
    pub rx_head: *mut net_msg_t,
    pub rx_tail: *mut net_msg_t,
    pub rx_count: c_int,
    pub waiter: *mut task_t,
    pub send_waiter: *mut task_t,
    pub udp: *mut core::ffi::c_void,
    pub raw: *mut core::ffi::c_void,
    pub tcp: *mut core::ffi::c_void,
    pub local_tcp: c_int,
    pub tcp_connecting: c_int,
    pub tcp_err: c_int,
    pub tcp_nodelay: c_int,
    pub reuseaddr: c_int,
    pub reuseport: c_int,
    pub keepalive: c_int,
    pub keep_idle: c_int,
    pub keep_intvl: c_int,
    pub keep_cnt: c_int,
    pub recv_timeout_ticks: u64,
    pub send_timeout_ticks: u64,
    pub ipv6_checksum_offset: c_int,
    pub icmp6_filter: [u32; 8],
    pub icmp6_filter_set: c_int,
    pub ipv6_recv_pktinfo: c_int,
    pub ipv6_recv_tclass: c_int,
    pub ipv6_recv_hoplimit: c_int,
    pub ipv6_recv_rthdr: c_int,
    pub ipv6_recv_hopopts: c_int,
    pub ipv6_recv_dstopts: c_int,
    pub ipv6_recv_err: c_int,
    pub ipv6_recv_2292_pktinfo: c_int,
    pub ipv6_recv_2292_hoplimit: c_int,
    pub ipv6_recv_2292_rthdr: c_int,
    pub ipv6_recv_2292_hopopts: c_int,
    pub ipv6_recv_2292_dstopts: c_int,
    pub bpf_prog_fd: c_int,
    pub alg_last: [u8; 2048],
    pub alg_last_len: usize,
    pub alg_type: [u8; 16],
    pub alg_name: [u8; 64],
    pub accept_next: *mut net_socket_t,
    pub accept_head: *mut net_socket_t,
    pub accept_tail: *mut net_socket_t,
    pub accept_count: c_int,
    pub in_registry: c_int,
    pub reg_idx: c_int,
}

unsafe extern "C" {
    pub static mut g_net_lock: spinlock_t;

    pub fn net_msg_alloc() -> *mut net_msg_t;
    pub fn net_msg_free(msg: *mut net_msg_t);

    pub fn a20_net_socket_from_file(gfd: c_int) -> *mut net_socket_t;
    pub fn a20_net_lwip_poll();
    pub fn a20_net_tcp_recved(socket: *mut net_socket_t, len: usize);

    pub fn a20_proc_current_task() -> *mut task_t;
    pub fn a20_sched_yield();
    pub fn a20_proc_make_ready_task(task: *mut task_t);
    pub fn a20_proc_set_wake_time_task(task: *mut task_t, wake_time: u64);
    pub fn a20_task_state_value(task: *mut task_t) -> c_int;
    pub fn a20_task_set_state_value(task: *mut task_t, state: c_int);
    pub fn a20_task_has_unblocked_signal(task: *mut task_t) -> c_int;
    pub fn a20_net_socket_wait_expired(socket: *mut net_socket_t, start: u64, for_write: c_int) -> c_int;
    pub fn a20_bpf_run_socket_filter(fd: c_int);
    pub fn a20_net_wait_ticks_value() -> u64;
    pub fn a20_ms_to_ticks_value(ms: u64) -> u64;
}
