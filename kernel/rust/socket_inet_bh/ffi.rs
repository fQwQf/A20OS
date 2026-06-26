use a20rust_support::lock::spinlock_t;
use core::ffi::{c_int, c_void};

pub const NET_MAX_SOCKETS: usize = 1024;
pub const NET_BH_RING_SIZE: usize = 16;
pub const NET_MAX_PAYLOAD: usize = 65535;
pub const NET_SOCKADDR_MAX: usize = 128;
pub const NET_BH_RECV: c_int = 0;
pub const PROC_BLOCKED: c_int = 3;

pub const ERR_OK: c_int = 0;
pub const ERR_MEM: c_int = -1;

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct udp_pcb {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct raw_pcb {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct tcp_pcb {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct pbuf {
    pub next: *mut pbuf,
    pub payload: *mut c_void,
    pub tot_len: u16,
    pub len: u16,
    pub type_internal: u8,
    pub flags: u8,
    pub r#ref: u16,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ip4_addr_t {
    pub addr: u32,
}

#[repr(C)]
#[derive(Clone, Copy)]
pub struct ip6_addr_t {
    pub addr: [u32; 4],
}

#[repr(C)]
pub union ip_addr_u {
    pub ip6: ip6_addr_t,
    pub ip4: ip4_addr_t,
}

#[repr(C)]
pub struct ip_addr_t {
    pub u_addr: ip_addr_u,
    pub type_: u8,
}

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
pub struct net_bh_ring_t {
    pub events: [net_bh_event_t; NET_BH_RING_SIZE],
    pub head: u32,
    pub tail: u32,
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
    pub udp: *mut udp_pcb,
    pub raw: *mut raw_pcb,
    pub tcp: *mut tcp_pcb,
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
    pub bh_ring: net_bh_ring_t,
    pub bh_connected: c_int,
    pub bh_closed: c_int,
    pub bh_error: c_int,
    pub bh_err_code: c_int,
    pub bh_tx_wake: c_int,
    pub bh_pending: c_int,
}

unsafe extern "C" {
    pub static mut g_net_lock: spinlock_t;
    pub static mut g_sockets: [*mut net_socket_t; NET_MAX_SOCKETS];
    pub static mut g_net_bh_pending: [c_int; NET_MAX_SOCKETS];

    pub fn net_lwip_ip_to_sockaddr(
        ip: *const ip_addr_t,
        port: u16,
        out: *mut u8,
        outlen: *mut usize,
    ) -> c_int;
    pub fn net_enqueue_msg_locked_meta(
        dst: *mut net_socket_t,
        buf: *const c_void,
        len: usize,
        addr: *const c_void,
        addrlen: usize,
        meta: *const net_bh_event_t,
    ) -> c_int;
    pub fn net_socket_is_valid_locked(s: *mut net_socket_t) -> c_int;

    pub fn pbuf_copy_partial(buf: *const pbuf, dataptr: *mut c_void, len: u16, offset: u16) -> u16;
    pub fn pbuf_free(buf: *mut pbuf) -> u8;

    pub fn a20_socket_bh_task_state(task: *mut task_t) -> c_int;
    pub fn a20_socket_bh_proc_make_ready(task: *mut task_t);

    pub fn a20_socket_bh_atomic_load_u32_relaxed(ptr: *const u32) -> u32;
    pub fn a20_socket_bh_atomic_load_u32_acquire(ptr: *const u32) -> u32;
    pub fn a20_socket_bh_atomic_fetch_add_u32_relaxed(ptr: *mut u32, val: u32) -> u32;
    pub fn a20_socket_bh_atomic_fetch_add_u32_release(ptr: *mut u32, val: u32) -> u32;
    pub fn a20_socket_bh_atomic_thread_fence_release();
    pub fn a20_socket_bh_atomic_thread_fence_acquire();
    pub fn a20_socket_bh_atomic_store_int_release(ptr: *mut c_int, val: c_int);
    pub fn a20_socket_bh_atomic_load_int_acquire(ptr: *const c_int) -> c_int;
    pub fn a20_socket_bh_atomic_load_int_relaxed(ptr: *const c_int) -> c_int;
    pub fn a20_socket_bh_atomic_exchange_int_acquire(ptr: *mut c_int, val: c_int) -> c_int;

    pub fn a20_socket_bh_ip_current_is_v6() -> c_int;
    pub fn a20_socket_bh_ip_current_input_ifindex() -> u32;
    pub fn a20_socket_bh_ip6_current_dest_addr_copy(out: *mut u8);
    pub fn a20_socket_bh_ip6_current_hoplimit() -> u8;
    pub fn a20_socket_bh_ip6_current_tclass() -> u8;
    pub fn a20_socket_bh_raw_should_passthrough_icmp_echo(s: *const net_socket_t, p: *const pbuf) -> c_int;
}
