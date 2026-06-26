use a20rust_support::lock::spinlock_t;
use core::ffi::{c_char, c_int, c_void};

pub const AF_UNIX: c_int = 1;
pub const AF_INET: c_int = 2;
pub const AF_INET6: c_int = 10;
pub const AF_ALG: c_int = 38;

pub const SOCK_STREAM: c_int = 1;
pub const SOCK_DGRAM: c_int = 2;
pub const SOCK_RAW: c_int = 3;
pub const SOCK_SEQPACKET: c_int = 5;
pub const SOCK_NONBLOCK: c_int = 0o4000;
pub const SOCK_CLOEXEC: c_int = 0o2000000;

pub const SOL_SOCKET: c_int = 1;
pub const SOL_ALG: c_int = 279;
pub const IPPROTO_IP: c_int = 0;
pub const IPPROTO_TCP: c_int = 6;
pub const IPPROTO_IPV6: c_int = 41;
pub const IPPROTO_ICMPV6: c_int = 58;

pub const SO_REUSEADDR: c_int = 2;
pub const SO_TYPE: c_int = 3;
pub const SO_ERROR: c_int = 4;
pub const SO_SNDBUF: c_int = 7;
pub const SO_RCVBUF: c_int = 8;
pub const SO_KEEPALIVE: c_int = 9;
pub const SO_REUSEPORT: c_int = 15;
pub const SO_RCVTIMEO: c_int = 20;
pub const SO_SNDTIMEO: c_int = 21;
pub const SO_ACCEPTCONN: c_int = 30;
pub const SO_PROTOCOL: c_int = 38;
pub const SO_DOMAIN: c_int = 39;
pub const SO_ATTACH_BPF: c_int = 50;

pub const TCP_NODELAY: c_int = 1;
pub const TCP_MAXSEG: c_int = 2;
pub const TCP_CORK: c_int = 3;
pub const TCP_KEEPIDLE: c_int = 4;
pub const TCP_KEEPINTVL: c_int = 5;
pub const TCP_KEEPCNT: c_int = 6;
pub const TCP_SYNCNT: c_int = 7;
pub const TCP_LINGER2: c_int = 8;
pub const TCP_DEFER_ACCEPT: c_int = 9;
pub const TCP_WINDOW_CLAMP: c_int = 10;
pub const TCP_INFO: c_int = 11;
pub const TCP_QUICKACK: c_int = 12;
pub const TCP_CONGESTION: c_int = 13;
pub const TCP_USER_TIMEOUT: c_int = 18;

pub const ICMP6_FILTER: c_int = 1;
pub const IPV6_ADDRFORM: c_int = 1;
pub const IPV6_2292PKTINFO: c_int = 2;
pub const IPV6_2292HOPOPTS: c_int = 3;
pub const IPV6_2292DSTOPTS: c_int = 4;
pub const IPV6_2292RTHDR: c_int = 5;
pub const IPV6_CHECKSUM: c_int = 7;
pub const IPV6_2292HOPLIMIT: c_int = 8;
pub const IPV6_FLOWINFO: c_int = 11;
pub const IPV6_MULTICAST_IF: c_int = 17;
pub const IPV6_MULTICAST_HOPS: c_int = 18;
pub const IPV6_MULTICAST_LOOP: c_int = 19;
pub const IPV6_JOIN_GROUP: c_int = 20;
pub const IPV6_LEAVE_GROUP: c_int = 21;
pub const IPV6_ROUTER_ALERT: c_int = 22;
pub const IPV6_RECVERR: c_int = 25;
pub const IPV6_V6ONLY: c_int = 26;
pub const IPV6_RECVPKTINFO: c_int = 49;
pub const IPV6_RECVHOPLIMIT: c_int = 51;
pub const IPV6_HOPLIMIT: c_int = 52;
pub const IPV6_RECVHOPOPTS: c_int = 53;
pub const IPV6_RECVRTHDR: c_int = 56;
pub const IPV6_RECVDSTOPTS: c_int = 58;
pub const IPV6_RECVTCLASS: c_int = 66;
pub const IPV6_TCLASS: c_int = 67;
pub const IPV6_UNICAST_IF: c_int = 76;

pub const MCAST_JOIN_GROUP: c_int = 42;
pub const MCAST_LEAVE_GROUP: c_int = 45;
pub const ALG_SET_KEY: c_int = 1;

pub const O_RDWR: c_int = 2;
pub const O_NONBLOCK: c_int = 0o4000;

pub const POLLIN: i16 = 0x001;
pub const POLLOUT: i16 = 0x004;
pub const POLLERR: i16 = 0x008;
pub const POLLHUP: i16 = 0x010;

pub const PROC_BLOCKED: c_int = 3;

pub const NET_MAX_STREAM_PAYLOAD: usize = 2048;
pub const NET_MAX_PAYLOAD: usize = 65535;
pub const NET_MAX_QUEUE: c_int = 128;
pub const NET_SOCKADDR_MAX: usize = 128;

pub const EFAULT: c_int = 14;
pub const EBADF: c_int = 9;
pub const EAGAIN: c_int = 11;
pub const EINVAL: c_int = 22;
pub const ENOMEM: c_int = 12;
pub const ENOTSOCK: c_int = 88;
pub const EOPNOTSUPP: c_int = 95;
pub const EADDRNOTAVAIL: c_int = 99;
pub const ENOTCONN: c_int = 107;
pub const ENOPROTOOPT: c_int = 92;
pub const ERESTARTSYS: c_int = 512;

#[repr(C)]
pub struct task_t {
    _opaque: [u8; 0],
}

#[repr(C)]
pub struct tcp_pcb {
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
    pub events: [net_bh_event_t; 16],
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

    pub fn timer_get_ticks() -> u64;
    pub fn a20_socket_control_ticks_per_sec() -> u64;
    pub fn proc_current() -> *mut task_t;
    pub fn sched();
    pub fn bpf_prog_is_loaded(fd: c_int) -> c_int;

    pub fn net_socket_from_file(gfd: c_int) -> *mut net_socket_t;
    pub fn net_accept_queue_pop_locked(listener: *mut net_socket_t) -> *mut net_socket_t;
    pub fn net_socket_install_file(s: *mut net_socket_t, flags: c_int) -> c_int;
    pub fn net_socket_wait_expired(s: *mut net_socket_t, start: u64, for_write: c_int) -> c_int;
    pub fn net_block_on_socket_locked(s: *mut net_socket_t, cur: *mut task_t);
    pub fn net_clear_socket_waiter(s: *mut net_socket_t, cur: *mut task_t);
    pub fn net_task_has_unblocked_signal(t: *mut task_t) -> c_int;
    pub fn net_inet_accept_child_ready(s: *mut net_socket_t);
    pub fn net_sockaddr_port(addr: *const c_void, len: usize, port: *mut u16) -> c_int;
    pub fn net_ntohs(x: u16) -> u16;
    pub fn net_alloc_ephemeral_port_locked() -> u16;
    pub fn net_sockaddr_loopback(s: *mut net_socket_t, port: u16);
    pub fn net_unregister_socket_locked(s: *mut net_socket_t);
    pub fn net_socket_is_valid_locked(s: *mut net_socket_t) -> c_int;
    pub fn net_inet_socket_destroy(s: *mut net_socket_t);
    pub fn net_socket_free(s: *mut net_socket_t);
    pub fn net_msg_free(m: *mut net_msg_t);
    pub fn net_alg_socket_accept(s: *mut net_socket_t, addrlen: *mut usize, flags: c_int) -> c_int;
    pub fn net_tcp_drop_pcb(s: *mut net_socket_t);

    pub fn a20_socket_control_task_state(task: *mut task_t) -> c_int;
    pub fn a20_socket_control_proc_make_ready(task: *mut task_t);
    pub fn a20_socket_control_alg_is(a: *const c_char, b: *const c_char) -> c_int;
    pub fn a20_socket_control_lwip_lock() -> u64;
    pub fn a20_socket_control_lwip_unlock(flags: u64);
    pub fn a20_socket_control_tcp_nagle_disable(pcb: *mut tcp_pcb);
    pub fn a20_socket_control_tcp_nagle_enable(pcb: *mut tcp_pcb);
    pub fn a20_socket_control_tcp_set_keep_idle_ms(pcb: *mut tcp_pcb, value_ms: u32);
    pub fn a20_socket_control_tcp_set_keep_intvl_ms(pcb: *mut tcp_pcb, value_ms: u32);
    pub fn a20_socket_control_tcp_set_keep_cnt(pcb: *mut tcp_pcb, value: u32);
    pub fn a20_socket_control_tcp_get_keep_idle_ms(pcb: *mut tcp_pcb) -> u32;
    pub fn a20_socket_control_tcp_get_keep_intvl_ms(pcb: *mut tcp_pcb) -> u32;
    pub fn a20_socket_control_tcp_get_keep_cnt(pcb: *mut tcp_pcb) -> u32;
    pub fn a20_socket_control_tcp_set_so_keepalive(pcb: *mut tcp_pcb, enabled: c_int);
}
