use a20rust_support::lock::spinlock_t;
use core::ffi::{c_int, c_void};

pub const AF_INET: c_int = 2;
pub const AF_INET6: c_int = 10;

pub const SOCK_STREAM: c_int = 1;
pub const SOCK_DGRAM: c_int = 2;
pub const SOCK_RAW: c_int = 3;

pub const IPPROTO_TCP: c_int = 6;
pub const IPPROTO_UDP: c_int = 17;

pub const MSG_DONTWAIT: c_int = 0x0040;

pub const PROC_BLOCKED: c_int = 3;

pub const NET_MAX_STREAM_PAYLOAD: usize = 2048;
pub const NET_MAX_PAYLOAD: usize = 65535;
pub const NET_MAX_QUEUE: c_int = 128;
pub const NET_SOCKADDR_MAX: usize = 128;
pub const NET_MAX_SOCKETS: usize = 1024;
pub const NET_BH_RING_SIZE: usize = 16;
pub const EINVAL: c_int = 22;
pub const ENOMEM: c_int = 12;
pub const EAGAIN: c_int = 11;
pub const EINTR: c_int = 4;
pub const EIO: c_int = 5;
pub const ENOTCONN: c_int = 107;
pub const EADDRINUSE: c_int = 98;
pub const EAFNOSUPPORT: c_int = 97;
pub const ENETUNREACH: c_int = 101;
pub const EINPROGRESS: c_int = 115;
pub const ECONNREFUSED: c_int = 111;
pub const ETIMEDOUT: c_int = 110;
pub const ERESTARTSYS: c_int = 512;
pub const EDESTADDRREQ: c_int = 89;
pub const EOPNOTSUPP: c_int = 95;
pub const EPIPE: c_int = 32;

pub const ERR_OK: c_int = 0;
pub const ERR_INPROGRESS: c_int = -5;

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
pub struct net_sockaddr_in_t {
    pub sin_family: u16,
    pub sin_port: u16,
    pub sin_addr: u32,
    pub sin_zero: [u8; 8],
}

#[repr(C)]
pub struct net_sockaddr_in6_t {
    pub sin6_family: u16,
    pub sin6_port: u16,
    pub sin6_flowinfo: u32,
    pub sin6_addr: [u8; 16],
    pub sin6_scope_id: u32,
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
    pub alg_last: [u8; NET_MAX_STREAM_PAYLOAD],
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

    pub fn timer_get_ticks() -> u64;
    pub fn a20_lwip_poll();
    pub fn a20_socket_inet_pcb_ms_to_ticks(ms: u64) -> u64;
    pub fn a20_socket_inet_pcb_connect_timeout_ticks() -> u64;

    pub fn net_alloc_ephemeral_port_locked() -> u16;
    pub fn net_sockaddr_loopback(s: *mut net_socket_t, port: u16);
    pub fn net_sockaddr_port(addr: *const c_void, len: usize, port: *mut u16) -> c_int;
    pub fn net_sockaddr_to_lwip_ip(
        addr: *const c_void,
        len: usize,
        ip: *mut ip_addr_t,
        port: *mut u16,
    ) -> c_int;
    pub fn net_ntohs(x: u16) -> u16;

    pub fn net_socket_alloc() -> *mut net_socket_t;
    pub fn net_socket_free(s: *mut net_socket_t);
    pub fn net_register_socket_locked(s: *mut net_socket_t) -> c_int;
    pub fn net_unregister_socket_locked(s: *mut net_socket_t);
    pub fn net_socket_is_valid_locked(s: *mut net_socket_t) -> c_int;

    pub fn net_accept_queue_push_locked(listener: *mut net_socket_t, child: *mut net_socket_t) -> c_int;

    pub fn net_enqueue_msg_locked(
        dst: *mut net_socket_t,
        buf: *const c_void,
        len: usize,
        addr: *const c_void,
        addrlen: usize,
    ) -> c_int;
    pub fn net_enqueue_msg_blocking(
        s: *mut net_socket_t,
        dst: *mut net_socket_t,
        buf: *const c_void,
        len: usize,
        addr: *const c_void,
        addrlen: usize,
        dontwait: c_int,
        timeout_ticks: u64,
    ) -> c_int;

    pub fn net_block_on_socket_locked(s: *mut net_socket_t, cur: *mut task_t);
    pub fn net_clear_socket_waiter(s: *mut net_socket_t, cur: *mut task_t);
    pub fn net_task_has_unblocked_signal(t: *mut task_t) -> c_int;

    pub fn proc_current() -> *mut task_t;
    pub fn proc_make_ready(task: *mut task_t);
    pub fn sched();
    pub fn a20_socket_inet_pcb_task_state(task: *mut task_t) -> c_int;

    pub fn a20_socket_inet_udp_new(domain: c_int, s: *mut net_socket_t) -> *mut udp_pcb;
    pub fn a20_socket_inet_raw_new(domain: c_int, protocol: c_int, s: *mut net_socket_t) -> *mut raw_pcb;
    pub fn a20_socket_inet_tcp_new_v4(s: *mut net_socket_t) -> *mut tcp_pcb;

    pub fn a20_socket_inet_udp_remove(pcb: *mut udp_pcb);
    pub fn a20_socket_inet_raw_remove(pcb: *mut raw_pcb);
    pub fn a20_socket_inet_tcp_destroy_abort(pcb: *mut tcp_pcb);

    pub fn a20_socket_inet_udp_bind(pcb: *mut udp_pcb, ip: *const ip_addr_t, port: u16) -> c_int;
    pub fn a20_socket_inet_udp_bind_any(pcb: *mut udp_pcb, port: u16) -> c_int;
    pub fn a20_socket_inet_raw_bind(pcb: *mut raw_pcb, ip: *const ip_addr_t) -> c_int;
    pub fn a20_socket_inet_tcp_bind(pcb: *mut tcp_pcb, ip: *const ip_addr_t, port: u16) -> c_int;

    pub fn a20_socket_inet_udp_connect(pcb: *mut udp_pcb, ip: *const ip_addr_t, port: u16) -> c_int;
    pub fn a20_socket_inet_raw_connect(pcb: *mut raw_pcb, ip: *const ip_addr_t) -> c_int;
    pub fn a20_socket_inet_tcp_connect(pcb: *mut tcp_pcb, ip: *const ip_addr_t, port: u16) -> c_int;

    pub fn a20_socket_inet_udp_sendto(
        pcb: *mut udp_pcb,
        buf: *const c_void,
        len: usize,
        ip: *const ip_addr_t,
        port: u16,
        connected: c_int,
    ) -> c_int;
    pub fn a20_socket_inet_raw_sendto(
        pcb: *mut raw_pcb,
        buf: *const c_void,
        len: usize,
        ip: *const ip_addr_t,
        connected: c_int,
    ) -> c_int;

    pub fn a20_socket_inet_tcp_close_socket(s: *mut net_socket_t);
    pub fn a20_socket_inet_tcp_drop_socket(s: *mut net_socket_t);
    pub fn a20_socket_inet_tcp_sndbuf(pcb: *mut tcp_pcb) -> u16;
    pub fn a20_socket_inet_tcp_write_output(pcb: *mut tcp_pcb, buf: *const c_void, len: u16) -> c_int;
    pub fn a20_socket_inet_tcp_backlog_accepted(pcb: *mut tcp_pcb);
    pub fn a20_socket_inet_tcp_recved(pcb: *mut tcp_pcb, len: usize);
}
